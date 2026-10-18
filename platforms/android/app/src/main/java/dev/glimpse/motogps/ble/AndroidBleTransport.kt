package dev.glimpse.motogps.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.util.UUID

/**
 * BluetoothGatt implementation of [BleTransport].
 *
 * All GATT operations are serialised: at most one write is in flight and the
 * next is issued from onCharacteristicWrite. Frames arriving from higher layers
 * are queued, never interleaved, because the protocol requires whole logical
 * messages to be written back to back.
 */
@SuppressLint("MissingPermission")
class AndroidBleTransport(
    context: Context,
    private val listener: BleTransport.Listener,
) : BleTransport {

    private val appContext = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())
    private val bluetoothManager =
        appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? = bluetoothManager?.adapter

    private val serviceUuid = UUID.fromString(BleProtocol.SERVICE_UUID)
    private val phoneToDeviceUuid = UUID.fromString(BleProtocol.PHONE_TO_DEVICE_UUID)
    private val deviceToPhoneUuid = UUID.fromString(BleProtocol.DEVICE_TO_PHONE_UUID)
    private val cccdUuid = UUID.fromString(BleProtocol.CCCD_UUID)

    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private val writeQueue = ArrayDeque<Pair<ByteArray, Boolean>>()
    private var writeInFlight = false
    private var pendingMtu = 23
    private var discoverFallback: Runnable? = null

    override var connectedAddress: String? = null
        private set

    override var negotiatedMtu: Int = 23
        private set

    val adapterAvailable: Boolean get() = adapter != null && adapter.isEnabled
    val bluetoothAdapter: BluetoothAdapter? get() = adapter

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            val name = runCatching { device.name }.getOrNull()
            listener.onScanResult(
                DiscoveredDevice(device.address, name, result.rssi),
            )
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            listener.onError("scan failed: $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            handler.post {
                if (status != BluetoothGatt.GATT_SUCCESS &&
                    newState != BluetoothProfile.STATE_CONNECTED
                ) {
                    connectedAddress = null
                    listener.onDisconnected("status=$status")
                    return@post
                }
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        connectedAddress = gatt.device.address
                        listener.onConnected(gatt.device.address)
                        // Request a large MTU, then discover services on the callback.
                        val requested = runCatching { gatt.requestMtu(517) }.getOrDefault(false)
                        if (!requested) {
                            discoverServices(gatt)
                        } else {
                            // Some stacks never answer requestMtu; fall back after 800 ms.
                            discoverFallback = Runnable {
                                if (gatt === this@AndroidBleTransport.gatt) discoverServices(gatt)
                            }.also { handler.postDelayed(it, 800) }
                        }
                    }

                    BluetoothProfile.STATE_DISCONNECTED -> {
                        cleanupGatt()
                        connectedAddress = null
                        listener.onDisconnected(null)
                    }
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            handler.post {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    pendingMtu = mtu
                    negotiatedMtu = mtu
                    listener.onMtuChanged(mtu)
                }
                discoverServices(gatt)
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            handler.post {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    listener.onError("service discovery failed: $status")
                    return@post
                }
                val service = gatt.getService(serviceUuid) ?: run {
                    listener.onError("moto BLE service not found")
                    return@post
                }
                val rx = service.getCharacteristic(phoneToDeviceUuid) ?: run {
                    listener.onError("phone->device characteristic missing")
                    return@post
                }
                val tx = service.getCharacteristic(deviceToPhoneUuid) ?: run {
                    listener.onError("device->phone characteristic missing")
                    return@post
                }
                val notifications = gatt.setCharacteristicNotification(tx, true)
                if (!notifications) {
                    listener.onError("cannot enable notifications")
                    return@post
                }
                val cccd = tx.getDescriptor(cccdUuid) ?: run {
                    listener.onError("CCCD missing")
                    return@post
                }
                val started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                        BluetoothGatt.GATT_SUCCESS
                } else {
                    @Suppress("DEPRECATION")
                    cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    gatt.writeDescriptor(cccd)
                }
                if (!started) listener.onError("cannot write CCCD")
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            handler.post {
                if (descriptor.uuid == cccdUuid && status == BluetoothGatt.GATT_SUCCESS) {
                    listener.onReady()
                } else if (descriptor.uuid == cccdUuid) {
                    listener.onError("CCCD write failed: $status")
                }
            }
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            @Suppress("DEPRECATION")
            val value = characteristic.value ?: return
            handler.post { listener.onNotification(value.copyOf()) }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handler.post { listener.onNotification(value.copyOf()) }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            handler.post {
                writeInFlight = false
                listener.onWriteComplete(status == BluetoothGatt.GATT_SUCCESS)
                pumpWrites()
            }
        }
    }

    private fun discoverServices(gatt: BluetoothGatt) {
        discoverFallback?.let { handler.removeCallbacks(it) }
        discoverFallback = null
        if (!gatt.discoverServices()) {
            listener.onError("discoverServices returned false")
        }
    }

    override fun startScan() {
        val adapter = adapter ?: run {
            listener.onBluetoothUnavailable("no Bluetooth adapter")
            return
        }
        if (!hasScanPermission()) {
            listener.onBluetoothUnavailable("扫描权限未授予")
            return
        }
        if (!appContext.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) {
            listener.onBluetoothUnavailable("BLE not supported")
            return
        }
        val enabled = runCatching { adapter.isEnabled }.getOrDefault(false)
        if (!enabled) {
            listener.onBluetoothUnavailable("Bluetooth is off")
            return
        }
        if (scanning) return
        val scanner = runCatching { adapter.bluetoothLeScanner }.getOrNull() ?: run {
            listener.onBluetoothUnavailable("BLE scanner unavailable")
            return
        }
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(serviceUuid))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .build()
        scanning = true
        runCatching { scanner.startScan(listOf(filter), settings, scanCallback) }
            .onFailure {
                scanning = false
                listener.onError("startScan failed: ${it.message}")
            }
    }

    override fun stopScan() {
        if (!scanning) return
        scanning = false
        val scanner = adapter?.bluetoothLeScanner ?: return
        runCatching { scanner.stopScan(scanCallback) }
        listener.onScanStopped()
    }

    override fun connect(address: String) {
        val adapter = adapter ?: run {
            listener.onBluetoothUnavailable("no Bluetooth adapter")
            return
        }
        if (!hasConnectPermission()) {
            listener.onBluetoothUnavailable("蓝牙连接权限未授予")
            return
        }
        val enabled = runCatching { adapter.isEnabled }.getOrDefault(false)
        if (!enabled) {
            listener.onBluetoothUnavailable("Bluetooth is off")
            return
        }
        cleanupGatt()
        writeQueue.clear()
        writeInFlight = false
        val device: BluetoothDevice = runCatching { adapter.getRemoteDevice(address) }
            .getOrElse {
                listener.onError("invalid device address")
                return
            }
        connectedAddress = address
        val created = runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } else {
                device.connectGatt(appContext, false, gattCallback)
            }
        }.getOrNull()
        gatt = created
        if (created == null) listener.onError("connectGatt failed")
    }

    private fun hasScanPermission(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            appContext.checkSelfPermission(android.Manifest.permission.BLUETOOTH_SCAN) ==
            PackageManager.PERMISSION_GRANTED

    private fun hasConnectPermission(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            appContext.checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED

    override fun disconnect() {
        stopScan()
        val current = gatt
        connectedAddress = null
        if (current != null) {
            runCatching { current.disconnect() }
        } else {
            listener.onDisconnected(null)
        }
    }

    override fun write(frame: ByteArray, withResponse: Boolean) {
        writeQueue.addLast(frame to withResponse)
        pumpWrites()
    }

    private fun pumpWrites() {
        if (writeInFlight) return
        val current = gatt ?: return
        val next = writeQueue.removeFirstOrNull() ?: return
        val (frame, withResponse) = next
        val service = current.getService(serviceUuid) ?: return
        val rx = service.getCharacteristic(phoneToDeviceUuid) ?: return
        rx.writeType = if (withResponse) {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        } else {
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        }
        writeInFlight = true
        val submitted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            current.writeCharacteristic(rx, frame, rx.writeType) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            rx.value = frame
            @Suppress("DEPRECATION")
            current.writeCharacteristic(rx)
        }
        if (!submitted) {
            writeInFlight = false
            listener.onWriteComplete(false)
        }
    }

    private fun cleanupGatt() {
        discoverFallback?.let { handler.removeCallbacks(it) }
        discoverFallback = null
        val current = gatt
        gatt = null
        writeInFlight = false
        writeQueue.clear()
        if (current != null) {
            runCatching { current.close() }
        }
    }

    override fun close() {
        stopScan()
        cleanupGatt()
        handler.removeCallbacksAndMessages(null)
    }
}
