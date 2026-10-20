package dev.glimpse.motogps.ble

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import dev.glimpse.motogps.NativeBridge
import java.util.UUID
import kotlin.random.Random

/**
 * Owns one ESP32 round-display session: scan, connect, v1 handshake, heartbeat
 * and reconnect. All public calls and BLE callbacks run on the main thread.
 *
 * Protocol invariants implemented here:
 *  - handshake is three steps and business data is sent only after the second
 *    Device Ready;
 *  - heartbeat payload uses session elapsed time, never system uptime;
 *  - the negotiated frame size follows the MTU, never a hard-coded 512;
 *  - on disconnect the queue is cleared and a fresh session id is used.
 */
class Esp32BleCentral(
    context: Context,
    private val now: () -> Long = { SystemClock.elapsedRealtime() },
    private val transportFactory: ((BleTransport.Listener) -> BleTransport)? = null,
) : BleTransport.Listener {

    sealed interface State {
        data object Idle : State
        data class Unavailable(val reason: String) : State
        data object Scanning : State
        data class Connecting(val address: String) : State
        data class Handshaking(val phase: HandshakeMachine.Phase) : State
        data class Connected(
            val address: String,
            val frameSize: Int,
            val capabilities: Long,
            val degraded: Boolean,
        ) : State
        data class Reconnecting(val attempt: Int, val delayMs: Long) : State
        data class Failed(val message: String) : State
    }

    private val main = Handler(Looper.getMainLooper())
    private val appContext = context.applicationContext
    /** Last round display that completed a handshake, reused after a restart. */
    private val linkPrefs =
        appContext.getSharedPreferences("ble_link", Context.MODE_PRIVATE)
    private val codec = MotoBleCodec()
    private val handshake = HandshakeMachine()
    private val outbound = OutboundQueue()
    private val pump = WritePumpPolicy()
    private val reconnect = ReconnectPolicy()
    private val transport: BleTransport =
        transportFactory?.invoke(this) ?: AndroidBleTransport(context, this)

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state.asStateFlow()

    private val _devices = MutableStateFlow<List<DiscoveredDevice>>(emptyList())
    val devices: StateFlow<List<DiscoveredDevice>> = _devices.asStateFlow()

    private val _log = MutableStateFlow<List<String>>(emptyList())
    val log: StateFlow<List<String>> = _log.asStateFlow()

    /** Receives decoded device gestures (page select, swipe, music keys). */
    var commandListener: ((RemoteCommand) -> Unit)? = null

    /**
     * Receives the device UiLayoutState (protocol §UiLayoutState): sent once
     * after handshake Ready and again after a layout is applied. The editor
     * compares layoutRevision to decide whether to push.
     */
    var layoutStateListener: ((DecodedLayoutState) -> Unit)? = null

    /**
     * Receives the device ACK for a pushed UiLayout (§5.6): true = Ok/Duplicate,
     * false = rejected. Drives the editor's sync state.
     */
    var layoutAckListener: ((Boolean) -> Unit)? = null

    private var targetAddress: String? = null
    private var userInitiatedDisconnect = false
    private var txSequence = 0
    private var lastWriteAtMs = 0L
    private var lastValidFrameAtMs = 0L
    private var handshakeAttempts = 0
    private var lastHandshakeResendAtMs = 0L
    private var heartbeatIntervalMs = BleProtocol.DEFAULT_HEARTBEAT_INTERVAL_MS
    private var pumpScheduled = false
    private var tickerRunning = false
    private var pendingSceneRevision = 0L
    private var sceneAttempts = 0
    private var lastSceneEncoded: ByteArray? = null
    /** Layout push bookkeeping: one pending revision, ACK-tracked. */
    private var pendingLayoutRevision = 0L
    private var layoutAttempts = 0
    private var lastLayoutEncoded: ByteArray? = null
    /** Revision the device reported via UiLayoutState (0 = not reported). */
    private var deviceLayoutRevision = 0L

    private val _wifiStatus = MutableStateFlow<DecodedWifiStatus?>(null)
    private val _wifiScan = MutableStateFlow<DecodedWifiScan?>(null)
    /** Recent device command ids for the current session, oldest first. */
    private val commandDeduper = DeviceCommandDeduper()

    private val heartbeatTicker = object : Runnable {
        override fun run() {
            if (!tickerRunning) return
            onTicker()
            main.postDelayed(this, 1_000)
        }
    }

    // --- public API -------------------------------------------------------

    fun startScan() {
        _devices.value = emptyList()
        _state.value = State.Scanning
        logLine("开始扫描 moto BLE 服务…")
        transport.startScan()
        main.postDelayed({ transport.stopScan() }, SCAN_DURATION_MS)
    }

    fun stopScan() {
        transport.stopScan()
        if (_state.value is State.Scanning) _state.value = State.Idle
    }

    fun connect(address: String) {
        userInitiatedDisconnect = false
        targetAddress = address
        reconnect.reset()
        _state.value = State.Connecting(address)
        transport.stopScan()
        try {
            transport.connect(address)
        } catch (error: SecurityException) {
            // Android 12+ throws when BLUETOOTH_CONNECT is missing; surface it
            // as a state instead of crashing the process.
            _state.value = State.Failed("缺少蓝牙连接权限：${error.message ?: "BLUETOOTH_CONNECT"}")
        }
    }

    /**
     * Reconnects the round display that completed the last handshake, so
     * reopening the app (or recovering from process death) returns to the
     * previous session without a manual scan. Returns false when there is no
     * saved device. Permission and transport failures are published as
     * [State.Failed] with a user-readable reason.
     */
    fun autoConnectLast(): Boolean {
        val address = linkPrefs.getString(KEY_LAST_ADDRESS, null)
        if (address.isNullOrEmpty()) return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            _state.value = State.Failed("缺少蓝牙权限，无法自动连接上次的圆屏")
            return false
        }
        logLine("自动连接上次的圆屏 $address")
        connect(address)
        return true
    }

    fun disconnect() {
        userInitiatedDisconnect = true
        targetAddress = null
        main.removeCallbacksAndMessages(null)
        tickerRunning = false
        pumpScheduled = false
        pendingSceneRevision = 0
        sceneAttempts = 0
        lastSceneEncoded = null
        main.removeCallbacks(sceneTimeout)
        main.removeCallbacks(layoutTimeout)
        pendingLayoutRevision = 0
        layoutAttempts = 0
        lastLayoutEncoded = null
        deviceLayoutRevision = 0
        handshakeAttempts = 0
        lastValidFrameAtMs = 0
        handshake.onTransportLost()
        codec.resetReassembler()
        outbound.clear()
        transport.disconnect()
        _state.value = State.Idle
        logLine("已断开连接")
    }

    fun shutdown() {
        disconnect()
        transport.close()
        codec.close()
    }

    /**
     * Sends one NavigationSnapshot. Returns false when the session is not Ready,
     * so the caller never treats a queued frame as an acknowledged one.
     */
    fun sendNavigationSnapshot(
        numeric: LongArray,
        roadName: String,
        instruction: String,
        priority: Boolean = false,
    ): Boolean {
        if (_state.value !is State.Connected) return false
        val payload = runCatching {
            NativeBridge.encodeNavigationSnapshot(numeric, roadName, instruction)
        }.getOrElse {
            logLine("导航快照编码失败：${it.message}")
            return false
        }
        sendMessage(
            BleProtocol.MessageType.NAVIGATION_SNAPSHOT, payload,
            withResponse = false,
            label = if (priority) "nav-page-echo" else "nav-snapshot",
            priority = priority,
        )
        return true
    }

    /** Sends one route-geometry chunk (already sliced by the caller). */
    fun sendRouteGeometry(meta: LongArray, pointsLonLat: DoubleArray): Boolean {
        if (_state.value !is State.Connected) return false
        val payload = runCatching {
            NativeBridge.encodeRouteGeometry(meta, pointsLonLat)
        }.getOrElse {
            logLine("路线几何编码失败：${it.message}")
            return false
        }
        sendMessage(
            BleProtocol.MessageType.ROUTE_GEOMETRY, payload,
            withResponse = false,
            applicationFlags = BleProtocol.Flags.ACK_REQUESTED,
            label = "route-geometry",
        )
        return true
    }

    /**
     * Sends MediaState (metadata only; the display has no audio). Requires the
     * negotiated capability bit; returns false when the device did not offer it.
     */
    fun sendMediaState(
        flags: Int,
        trackToken: Long,
        positionS: Int,
        durationS: Int,
        sourceName: String,
        trackTitle: String,
        artistName: String,
    ): Boolean {
        val connected = _state.value as? State.Connected ?: return false
        if (connected.capabilities and BleProtocol.Capability.MEDIA_STATE == 0L) return false
        val payload = runCatching {
            NativeBridge.encodeMediaState(
                flags, trackToken, positionS, durationS, sourceName, trackTitle, artistName,
            )
        }.getOrElse {
            logLine("媒体状态编码失败：${it.message}")
            return false
        }
        sendMessage(
            BleProtocol.MessageType.MEDIA_STATE, payload,
            withResponse = false, label = "media-state",
        )
        return true
    }

    /**
     * Sends a MapScene and waits for the device ACK (3 s, up to 3 attempts).
     * Only one scene is pending at a time; a newer revision supersedes it.
     */
    fun sendMapScene(revision: Long, encoded: ByteArray): Boolean {
        if (_state.value !is State.Connected) return false
        pendingSceneRevision = revision
        sceneAttempts = 1
        lastSceneEncoded = encoded
        fragmentScene(encoded)
        scheduleSceneTimeout()
        return true
    }

    private fun fragmentScene(encoded: ByteArray) {
        val frames = runCatching {
            codec.fragment(
                BleProtocol.MessageType.MAP_SCENE, nextSequence(), encoded,
                currentFrameSize(), BleProtocol.Flags.ACK_REQUESTED,
            )
        }.getOrElse {
            logLine("地图场景分片失败：${it.message}")
            return
        }
        outbound.enqueueMessage(frames, "map-scene")
        schedulePump()
    }

    private fun scheduleSceneTimeout() {
        main.removeCallbacks(sceneTimeout)
        main.postDelayed(sceneTimeout, SCENE_ACK_TIMEOUT_MS)
    }

    private val sceneTimeout = object : Runnable {
        override fun run() {
            if (pendingSceneRevision == 0L) return
            if (sceneAttempts >= MAX_SCENE_ATTEMPTS) {
                logLine("地图场景未确认，放弃本轮（revision=$pendingSceneRevision）")
                pendingSceneRevision = 0
                return
            }
            logLine("地图场景 ACK 超时，重发（第 ${sceneAttempts + 1} 次）")
            sceneAttempts += 1
            lastSceneEncoded?.let { fragmentScene(it) }
            main.postDelayed(this, SCENE_ACK_TIMEOUT_MS)
        }
    }

    /**
     * Sends one UiLayout (already encoded by layout/LayoutCodec) and waits for
     * the device ACK (§5.6: 750 ms, up to 2 retries). Requires capability
     * bit8; returns false when the device did not offer it.
     */
    fun sendUiLayout(layoutRevision: Long, encoded: ByteArray): Boolean {
        val connected = _state.value as? State.Connected ?: return false
        if (connected.capabilities and BleProtocol.Capability.UI_LAYOUT == 0L) return false
        pendingLayoutRevision = layoutRevision
        layoutAttempts = 1
        lastLayoutEncoded = encoded
        fragmentLayout(encoded)
        scheduleLayoutTimeout()
        return true
    }

    /** The layout revision the device last reported (UiLayoutState), 0 if none. */
    fun reportedLayoutRevision(): Long = deviceLayoutRevision

    // --- WiFi provisioning (docs §5.5) -----------------------------------

    /** Latest WifiStatus pushed by the device, null until the first report. */
    val deviceWifiStatus: StateFlow<DecodedWifiStatus?> get() = _wifiStatus

    /** Latest WifiScanResult pushed by the device, null until the first scan. */
    val deviceWifiScan: StateFlow<DecodedWifiScan?> get() = _wifiScan

    /** WifiStatus listener (device pushes on every state change). */
    var wifiStatusListener: ((DecodedWifiStatus) -> Unit)? = null

    /** WifiScanResult listener. */
    var wifiScanListener: ((DecodedWifiScan) -> Unit)? = null

    /**
     * Sends a WifiConfig message. Requires capability bit9; returns false when
     * the device did not offer WiFi provisioning or the connection is down.
     */
    fun sendWifiConfig(
        action: Int,
        flags: Int,
        ssid: String,
        password: String,
    ): Boolean {
        val connected = _state.value as? State.Connected ?: return false
        if (connected.capabilities and BleProtocol.Capability.WIFI_PROVISIONING == 0L) return false
        val encoded = runCatching { codec.encodeWifiConfig(action, flags, ssid, password) }
            .getOrElse {
                logLine("WifiConfig 编码失败：${it.message}")
                return false
            }
        val frames = runCatching {
            codec.fragment(
                BleProtocol.MessageType.WIFI_CONFIG, nextSequence(), encoded,
                currentFrameSize(), BleProtocol.Flags.ACK_REQUESTED,
            )
        }.getOrElse {
            logLine("WifiConfig 分片失败：${it.message}")
            return false
        }
        outbound.enqueueMessage(frames, "wifi-config")
        schedulePump()
        return true
    }

    /** Sends a WifiScanRequest; results arrive via wifiScanListener. */
    fun sendWifiScanRequest(requestId: Int): Boolean {
        val connected = _state.value as? State.Connected ?: return false
        if (connected.capabilities and BleProtocol.Capability.WIFI_PROVISIONING == 0L) return false
        val encoded = runCatching { codec.encodeWifiScanRequest(requestId) }
            .getOrElse {
                logLine("WifiScanRequest 编码失败：${it.message}")
                return false
            }
        val frames = runCatching {
            codec.fragment(
                BleProtocol.MessageType.WIFI_SCAN_REQUEST, nextSequence(), encoded,
                currentFrameSize(), 0,
            )
        }.getOrElse {
            logLine("WifiScanRequest 分片失败：${it.message}")
            return false
        }
        outbound.enqueueMessage(frames, "wifi-scan")
        schedulePump()
        return true
    }

    private fun fragmentLayout(encoded: ByteArray) {
        val frames = runCatching {
            codec.fragment(
                BleProtocol.MessageType.UI_LAYOUT, nextSequence(), encoded,
                currentFrameSize(), BleProtocol.Flags.ACK_REQUESTED,
            )
        }.getOrElse {
            logLine("圆屏布局分片失败：${it.message}")
            return
        }
        outbound.enqueueMessage(frames, "ui-layout")
        schedulePump()
    }

    private fun scheduleLayoutTimeout() {
        main.removeCallbacks(layoutTimeout)
        main.postDelayed(layoutTimeout, LAYOUT_ACK_TIMEOUT_MS)
    }

    private val layoutTimeout = object : Runnable {
        override fun run() {
            if (pendingLayoutRevision == 0L) return
            if (layoutAttempts >= MAX_LAYOUT_ATTEMPTS) {
                logLine("圆屏布局未确认，标记待同步（revision=$pendingLayoutRevision）")
                pendingLayoutRevision = 0
                return
            }
            logLine("圆屏布局 ACK 超时，重发（第 ${layoutAttempts + 1} 次）")
            layoutAttempts += 1
            lastLayoutEncoded?.let { fragmentLayout(it) }
            main.postDelayed(this, LAYOUT_ACK_TIMEOUT_MS)
        }
    }

    // --- BleTransport.Listener -------------------------------------------

    override fun onBluetoothUnavailable(reason: String) {
        _state.value = State.Unavailable(reason)
        logLine("蓝牙不可用：$reason")
    }

    override fun onScanResult(device: DiscoveredDevice) {
        if (_state.value !is State.Scanning && _state.value !is State.Idle) return
        val existing = _devices.value
        _devices.value = if (existing.any { it.address == device.address }) {
            existing.map { if (it.address == device.address) device else it }
        } else {
            existing + device
        }
    }

    override fun onScanStopped() {
        if (_state.value is State.Scanning) {
            _state.value = if (_devices.value.isEmpty()) State.Idle else _state.value
        }
    }

    override fun onConnected(address: String) {
        targetAddress = address
        logLine("GATT 已连接：$address")
        _state.value = State.Connecting(address)
    }

    override fun onMtuChanged(mtu: Int) {
        logLine("MTU=$mtu，帧上限=${BleProtocol.mtuToFrameSize(mtu)}")
    }

    override fun onReady() {
        logLine("服务发现完成，订阅 TX 成功，开始握手")
        val sessionId = generateSessionId()
        commandDeduper.reset()
        val intent = handshake.start(sessionId, now())
        handshakeAttempts = 0
        lastHandshakeResendAtMs = now()
        handleIntent(intent)
        startTicker()
    }

    override fun onNotification(value: ByteArray) {
        lastValidFrameAtMs = now()
        // Some OEM stacks report the CCCD write as failed although
        // notifications are already flowing. If a frame arrives before the
        // handshake started, start the session from here.
        if (handshake.phase == HandshakeMachine.Phase.Idle &&
            (_state.value is State.Connecting || _state.value is State.Failed)
        ) {
            logLine("收到通知但握手未启动，补启动会话")
            val sessionId = generateSessionId()
            commandDeduper.reset()
            handshakeAttempts = 0
            lastHandshakeResendAtMs = now()
            handleIntent(handshake.start(sessionId, now()))
            startTicker()
        }
        when (val reassembled = codec.pushFrame(value, now())) {
            is MotoBleCodec.Reassembly.Complete -> onCompleteMessage(reassembled)
            is MotoBleCodec.Reassembly.Duplicate -> {
                logLine("重复帧 type=${reassembled.type} seq=${reassembled.sequence}")
            }
            is MotoBleCodec.Reassembly.Failed -> {
                logLine("重组失败 error=${reassembled.error}")
            }
            is MotoBleCodec.Reassembly.InProgress -> Unit
        }
    }

    override fun onWriteComplete(success: Boolean) {
        if (!success) logLine("GATT 写失败")
        lastWriteAtMs = now()
        schedulePump()
    }

    override fun onDisconnected(reason: String?) {
        tickerRunning = false
        main.removeCallbacks(heartbeatTicker)
        handshake.onTransportLost()
        codec.resetReassembler()
        commandDeduper.reset()
        outbound.clear()
        _wifiStatus.value = null
        _wifiScan.value = null
        logLine("连接断开" + (reason?.let { "：$it" } ?: ""))
        if (userInitiatedDisconnect || targetAddress == null) {
            _state.value = State.Idle
            return
        }
        val delay = reconnect.nextDelayMs()
        _state.value = State.Reconnecting(reconnect.attempt, delay)
        logLine("${delay / 1000}s 后重连（第 ${reconnect.attempt} 次）")
        val address = targetAddress ?: return
        main.postDelayed({ if (!userInitiatedDisconnect) transport.connect(address) }, delay)
    }

    override fun onError(message: String) {
        logLine("错误：$message")
        _state.value = State.Failed(message)
    }

    override fun onWarning(message: String) {
        logLine("警告：$message")
    }

    // --- internals --------------------------------------------------------

    private fun onCompleteMessage(message: MotoBleCodec.Reassembly.Complete) {
        when (message.type) {
            BleProtocol.MessageType.CONNECTION_STATUS -> {
                val status = runCatching { codec.decodeConnectionStatus(message.payload) }
                    .getOrElse {
                        logLine("ConnectionStatus 解码失败")
                        return
                    }
                handleIntent(handshake.onDeviceStatus(status))
            }

            BleProtocol.MessageType.ACK -> {
                val ack = runCatching { codec.decodeAck(message.payload) }.getOrNull() ?: return
                logLine("ACK seq=${ack.acknowledgedSequence} status=${ack.status}")
                if (pendingSceneRevision != 0L && (ack.status == 0 || ack.status == 4)) {
                    val revision = pendingSceneRevision
                    pendingSceneRevision = 0
                    main.removeCallbacks(sceneTimeout)
                    lastSceneEncoded = null
                    logLine("地图场景已确认 revision=$revision")
                }
                if (pendingLayoutRevision != 0L && (ack.status == 0 || ack.status == 4)) {
                    val revision = pendingLayoutRevision
                    pendingLayoutRevision = 0
                    main.removeCallbacks(layoutTimeout)
                    lastLayoutEncoded = null
                    logLine("圆屏布局已确认 revision=$revision")
                    runCatching { layoutAckListener?.invoke(true) }
                } else if (pendingLayoutRevision != 0L && ack.status != 0 && ack.status != 4) {
                    val revision = pendingLayoutRevision
                    pendingLayoutRevision = 0
                    main.removeCallbacks(layoutTimeout)
                    lastLayoutEncoded = null
                    logLine("圆屏布局被拒绝 revision=$revision status=${ack.status}")
                    runCatching { layoutAckListener?.invoke(false) }
                }
            }

            BleProtocol.MessageType.UI_LAYOUT_STATE -> {
                val state = runCatching { codec.decodeUiLayoutState(message.payload) }
                    .getOrElse {
                        logLine("UiLayoutState 解码失败")
                        return
                    }
                deviceLayoutRevision = state.layoutRevision.toLong()
                logLine(
                    "圆屏布局状态 revision=${state.layoutRevision} " +
                        "source=${state.source} mask=0x${state.supportedMask.toString(16)}",
                )
                runCatching { layoutStateListener?.invoke(state) }
            }

            BleProtocol.MessageType.WIFI_STATUS -> {
                val status = runCatching { codec.decodeWifiStatus(message.payload) }
                    .getOrElse {
                        logLine("WifiStatus 解码失败")
                        return
                    }
                _wifiStatus.value = status
                logLine(
                    "圆屏 WiFi 状态 state=${status.state} rssi=${status.rssi} " +
                        "error=${status.error} ssid=${status.ssid}",
                )
                runCatching { wifiStatusListener?.invoke(status) }
            }

            BleProtocol.MessageType.WIFI_SCAN_RESULT -> {
                val scan = runCatching { codec.decodeWifiScanResult(message.payload) }
                    .getOrElse {
                        logLine("WifiScanResult 解码失败")
                        return
                    }
                _wifiScan.value = scan
                logLine("圆屏 WiFi 扫描 id=${scan.requestId} AP=${scan.accessPoints.size}")
                runCatching { wifiScanListener?.invoke(scan) }
            }

            BleProtocol.MessageType.DEVICE_COMMAND -> {
                val command = runCatching { codec.decodeDeviceCommand(message.payload) }
                    .getOrNull() ?: return
                // The protocol requires deduplication by (session_id, command_id):
                // the display retransmits a command until it sees an ACK, so a
                // lost ACK must not replay the gesture (a repeated page change
                // snaps the UI back, a repeated play/pause toggles twice).
                if (!commandDeduper.accept(command.commandId.toLong())) {
                    if (message.flags and BleProtocol.Flags.ACK_REQUESTED != 0) {
                        sendAck(message.sequence, STATUS_DUPLICATE, command.commandId)
                    }
                    logLine("重复设备命令 id=${command.commandId}，已忽略")
                    return
                }
                logLine("设备命令 kind=${command.kind} id=${command.commandId} page=${command.page}")
                // Spec §4.3: the ACK is sent only after the action has been
                // committed, so a retransmission can never slip in between the
                // ACK and the actual state change.
                val committed = runCatching { commandListener?.invoke(command) }.isSuccess
                if (message.flags and BleProtocol.Flags.ACK_REQUESTED != 0) {
                    sendAck(
                        message.sequence,
                        if (committed) STATUS_OK else STATUS_FAILED,
                        command.commandId,
                    )
                }
            }

            else -> logLine("收到 type=${message.type} len=${message.payload.size}")
        }
    }

    private fun handleIntent(intent: HandshakeMachine.Intent) {
        when (intent) {
            is HandshakeMachine.Intent.SendStatus -> {
                val payload = codec.encodeConnectionStatus(
                    intent.role, intent.state, handshake.sessionId,
                    currentFrameSize(), heartbeatIntervalMs,
                )
                sendMessage(
                    BleProtocol.MessageType.CONNECTION_STATUS, payload,
                    withResponse = true, label = "phone-status-${intent.state}",
                )
            }

            is HandshakeMachine.Intent.SendStatusAndReady -> {
                val payload = codec.encodeConnectionStatus(
                    intent.role, intent.state, handshake.sessionId,
                    currentFrameSize(), heartbeatIntervalMs,
                )
                sendMessage(
                    BleProtocol.MessageType.CONNECTION_STATUS, payload,
                    withResponse = true, label = "phone-ready",
                )
                _state.value = State.Handshaking(HandshakeMachine.Phase.AwaitingFinalReady)
            }

            is HandshakeMachine.Intent.Complete -> {
                val negotiated = intent.negotiated
                heartbeatIntervalMs = negotiated.heartbeatIntervalMs
                reconnect.reset()
                handshakeAttempts = 0
                outbound.acknowledgeGeometry()
                _state.value = State.Connected(
                    address = targetAddress ?: "",
                    frameSize = negotiated.frameSize,
                    capabilities = negotiated.capabilities,
                    degraded = negotiated.degraded,
                )
                targetAddress?.takeIf { it.isNotEmpty() }?.let { address ->
                    linkPrefs.edit().putString(KEY_LAST_ADDRESS, address).apply()
                }
                logLine(
                    "握手完成：frameSize=${negotiated.frameSize} " +
                        "capabilities=0x${negotiated.capabilities.toString(16)}" +
                        if (negotiated.degraded) " (degraded)" else "",
                )
            }

            is HandshakeMachine.Intent.Restart -> {
                logLine("握手中止：${intent.reason}，重新建立会话")
                handshake.onTransportLost()
                outbound.clear()
                codec.resetReassembler()
                val address = targetAddress
                if (address != null) transport.disconnect()
            }

            is HandshakeMachine.Intent.Failed -> {
                logLine("握手失败：${intent.reason}")
                _state.value = State.Failed(intent.reason)
                transport.disconnect()
            }

            HandshakeMachine.Intent.None -> Unit
        }
    }

    private fun onTicker() {
        val state = _state.value
        when (state) {
            is State.Handshaking -> {
                val t = now()
                if (t - lastHandshakeResendAtMs >= 1_000) {
                    lastHandshakeResendAtMs = t
                    handshakeAttempts += 1
                    if (handshakeAttempts > MAX_HANDSHAKE_ATTEMPTS) {
                        logLine("握手超时，重连")
                        transport.disconnect()
                        return
                    }
                    resendHandshakeStatus()
                }
            }

            is State.Connected -> {
                val t = now()
                handshake.onTick(t)
                if (lastValidFrameAtMs != 0L && t - lastValidFrameAtMs > DEVICE_TIMEOUT_MS) {
                    logLine("设备心跳超时，重连")
                    transport.disconnect()
                    return
                }
                val payload = codec.encodeHeartbeat(handshake.sessionId, handshake.sessionElapsedMs)
                sendMessage(
                    BleProtocol.MessageType.HEARTBEAT, payload,
                    withResponse = false, label = "heartbeat",
                )
            }

            else -> Unit
        }
    }

    private fun resendHandshakeStatus() {
        val phase = handshake.phase
        val phaseState = when (phase) {
            HandshakeMachine.Phase.AwaitingDeviceReady -> BleProtocol.State.STARTING
            HandshakeMachine.Phase.AwaitingFinalReady -> BleProtocol.State.READY
            else -> return
        }
        val payload = codec.encodeConnectionStatus(
            BleProtocol.Role.PHONE, phaseState, handshake.sessionId,
            currentFrameSize(), heartbeatIntervalMs,
        )
        sendMessage(
            BleProtocol.MessageType.CONNECTION_STATUS, payload,
            withResponse = true, label = "handshake-resend",
        )
    }

    private fun sendAck(sequence: Int, status: Int, commandId: Int) {
        val payload = codec.encodeAck(sequence, status, commandId)
        sendMessage(BleProtocol.MessageType.ACK, payload, withResponse = true, label = "ack")
    }

    private fun sendMessage(
        type: Int,
        payload: ByteArray,
        withResponse: Boolean,
        applicationFlags: Int = 0,
        label: String,
        priority: Boolean = false,
    ) {
        val frames = runCatching {
            codec.fragment(type, nextSequence(), payload, currentFrameSize(), applicationFlags)
        }.getOrElse {
            logLine("分片失败：${it.message}")
            return
        }
        if (priority) {
            outbound.enqueueFront(frames, label)
        } else if (!outbound.enqueueMessage(frames, label)) {
            logLine("发送队列溢出，已清空队列（需要重发完整路线几何）")
        }
        schedulePump()
    }

    private fun schedulePump() {
        if (pumpScheduled) return
        pumpScheduled = true
        main.postDelayed({ runPump() }, pump.delayUntilNextWrite(lastWriteAtMs, now()))
    }

    private fun runPump() {
        pumpScheduled = false
        val entry = outbound.poll() ?: return
        if (!pump.mayWrite(lastWriteAtMs, now())) {
            // Put it back and retry after the pacing window.
            outbound.enqueueMessage(listOf(entry.bytes), entry.label)
            pumpScheduled = true
            main.postDelayed({ runPump() }, pump.delayUntilNextWrite(lastWriteAtMs, now()))
            return
        }
        transport.write(entry.bytes, entry.label != "heartbeat" && !entry.label.startsWith("nav-"))
        lastWriteAtMs = now()
        if (outbound.size > 0) schedulePump()
    }

    private fun currentFrameSize(): Int =
        BleProtocol.mtuToFrameSize(transport.negotiatedMtu)

    private fun nextSequence(): Int {
        txSequence = if (txSequence >= 0xFFFF) 1 else txSequence + 1
        return txSequence
    }

    private fun startTicker() {
        if (tickerRunning) return
        tickerRunning = true
        main.post(heartbeatTicker)
    }

    private fun generateSessionId(): Long =
        (Random.nextLong(1, 0xFFFFFFFFL))

    private fun logLine(line: String) {
        android.util.Log.i("MotoGpsBle", line)
        val stamped = "${java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US).format(java.util.Date())}  $line"
        _log.value = (_log.value + stamped).takeLast(200)
    }

    private companion object {
        const val KEY_LAST_ADDRESS = "last_device_address"
        const val SCAN_DURATION_MS = 20_000L
        const val DEVICE_TIMEOUT_MS = 5_000L
        const val MAX_HANDSHAKE_ATTEMPTS = 5
        const val SCENE_ACK_TIMEOUT_MS = 3_000L
        const val MAX_SCENE_ATTEMPTS = 3
        /** §5.6: ACK timeout 750 ms, up to 2 retries for the layout push. */
        const val LAYOUT_ACK_TIMEOUT_MS = 750L
        const val MAX_LAYOUT_ATTEMPTS = 3
        const val STATUS_OK = 0
        /** Protocol status for an already handled (session_id, command_id). */
        const val STATUS_DUPLICATE = 4
        /** Protocol status when the action could not be committed. */
        const val STATUS_FAILED = 3
    }
}
