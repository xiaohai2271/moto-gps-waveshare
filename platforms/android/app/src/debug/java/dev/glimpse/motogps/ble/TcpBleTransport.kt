package dev.glimpse.motogps.ble

import android.os.Handler
import android.os.Looper
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Debug-only BLE transport that talks to the host virtual device
 * (`platforms/desktop`, `moto_device_sim`) over loopback TCP.
 *
 * The link carries exactly one encoded v1 BLE frame per message:
 *
 *     magic "MOTO" (4) | version 1 (1) | kind 1 (1) | length BE (2) | payload
 *
 * This is the same framing the simulator and its scripted phone use. The
 * transport reports an ATT MTU of 185 so the existing codec negotiates the
 * same 182-byte frames as the measured phone link; no production BLE code is
 * bypassed except the radio itself.
 *
 * Enable it from a debug build by creating `files/ble_sim` with an optional
 * `host:port` value:
 *
 *     adb shell run-as dev.glimpse.motogps \
 *         sh -c 'echo 127.0.0.1:9100 > files/ble_sim'
 *     adb reverse tcp:9100 tcp:9100
 */
class TcpBleTransport(
    private val listener: BleTransport.Listener,
    private val host: String = "127.0.0.1",
    private val port: Int = 9100,
) : BleTransport {

    companion object {
        private const val MAGIC_0 = 0x4D
        private const val MAGIC_1 = 0x4F
        private const val MAGIC_2 = 0x54
        private const val MAGIC_3 = 0x4F
        private const val LINK_VERSION = 1
        private const val LINK_KIND_BLE_FRAME = 1
        private const val HEADER_SIZE = 8
        private const val MAX_PAYLOAD = 512
        private const val SIMULATED_MTU = 185
        private const val CONNECT_TIMEOUT_MS = 3_000
    }

    private val main = Handler(Looper.getMainLooper())
    private val closed = AtomicBoolean(false)
    private val disconnectReported = AtomicBoolean(false)

    @Volatile
    private var stopRequested = false

    @Volatile
    private var socket: Socket? = null

    @Volatile
    private var output: OutputStream? = null

    @Volatile
    private var negotiated = SIMULATED_MTU

    override val connectedAddress: String?
        get() = if (socket?.isConnected == true) "$host:$port" else null

    override val negotiatedMtu: Int
        get() = negotiated

    override fun startScan() {
        if (closed.get()) return
        main.postDelayed({
            if (closed.get()) return@postDelayed
            listener.onScanResult(
                DiscoveredDevice(
                    address = "$host:$port",
                    name = "MOTO GPS (virtual)",
                    rssi = -45,
                ),
            )
            listener.onScanStopped()
        }, 250L)
    }

    override fun stopScan() = Unit

    override fun connect(address: String) {
        if (closed.get()) return
        stopRequested = false
        disconnectReported.set(false)
        closeSocket()

        Thread({
            try {
                val client = Socket()
                client.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
                client.tcpNoDelay = true
                socket = client
                output = client.getOutputStream()
                negotiated = SIMULATED_MTU
                main.post {
                    if (closed.get()) return@post
                    listener.onConnected("$host:$port")
                    listener.onMtuChanged(negotiated)
                    listener.onReady()
                }
                readLoop(client.getInputStream())
            } catch (error: IOException) {
                closeSocket()
                reportDisconnected(error.message ?: "virtual link failed")
            }
        }, "moto-sim-ble").apply { isDaemon = true }.start()
    }

    override fun disconnect() {
        stopRequested = true
        closeSocket()
    }

    override fun write(frame: ByteArray, withResponse: Boolean) {
        val stream = output
        if (stream == null) {
            main.post { listener.onError("virtual BLE link is not connected") }
            return
        }
        if (frame.size > MAX_PAYLOAD) {
            main.post { listener.onError("frame exceeds the BLE attribute size") }
            return
        }
        try {
            synchronized(this) {
                stream.write(header(frame.size))
                stream.write(frame)
                stream.flush()
            }
            if (withResponse) {
                main.post { listener.onWriteComplete(true) }
            }
        } catch (error: IOException) {
            closeSocket()
            reportDisconnected(error.message ?: "virtual link write failed")
        }
    }

    override fun close() {
        closed.set(true)
        stopRequested = true
        closeSocket()
    }

    private fun readLoop(input: InputStream) {
        val header = ByteArray(HEADER_SIZE)
        while (!closed.get() && !stopRequested) {
            if (!readFully(input, header)) break
            if (header[0] != MAGIC_0.toByte() || header[1] != MAGIC_1.toByte() ||
                header[2] != MAGIC_2.toByte() || header[3] != MAGIC_3.toByte() ||
                header[4].toInt() != LINK_VERSION ||
                header[5].toInt() != LINK_KIND_BLE_FRAME
            ) {
                break
            }
            val length = ((header[6].toInt() and 0xFF) shl 8) or
                (header[7].toInt() and 0xFF)
            if (length !in 0..MAX_PAYLOAD) break
            val payload = ByteArray(length)
            if (!readFully(input, payload)) break
            main.post {
                if (!closed.get()) listener.onNotification(payload)
            }
        }
        closeSocket()
        reportDisconnected(null)
    }

    private fun reportDisconnected(reason: String?) {
        if (stopRequested || closed.get()) return
        if (!disconnectReported.compareAndSet(false, true)) return
        main.post {
            if (!stopRequested && !closed.get()) {
                listener.onDisconnected(reason)
            }
        }
    }

    private fun closeSocket() {
        val current = socket
        socket = null
        output = null
        runCatching { current?.close() }
    }

    private fun readFully(input: InputStream, target: ByteArray): Boolean {
        var offset = 0
        while (offset < target.size) {
            val read = try {
                input.read(target, offset, target.size - offset)
            } catch (error: IOException) {
                return false
            }
            if (read <= 0) return false
            offset += read
        }
        return true
    }

    private fun header(size: Int): ByteArray = byteArrayOf(
        MAGIC_0.toByte(),
        MAGIC_1.toByte(),
        MAGIC_2.toByte(),
        MAGIC_3.toByte(),
        LINK_VERSION.toByte(),
        LINK_KIND_BLE_FRAME.toByte(),
        ((size shr 8) and 0xFF).toByte(),
        (size and 0xFF).toByte(),
    )
}
