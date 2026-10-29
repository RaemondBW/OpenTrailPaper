package com.raemond.opentrailpaper.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothGattCharacteristic
import java.util.ArrayDeque

/**
 * Serializes GATT traffic.
 *
 * This is the one structural difference between CoreBluetooth and Android's
 * stack, and everything above it depends on getting it right. iOS lets you fire
 * writes freely and tells you when the pipe is full
 * (`canSendWriteWithoutResponse` / `peripheralIsReady`); Android's
 * `BluetoothGatt` accepts exactly ONE outstanding operation and silently drops
 * anything issued before the previous one's callback lands. Firmware and map
 * transfers are thousands of packets, so "silently drops" would mean a corrupt
 * image on the device's SD card.
 *
 * So every read, write, descriptor write and MTU request goes through here and
 * the next one starts only when the previous completes. The transfer pumps
 * enqueue their whole payload up front and let this drain it — the queue IS the
 * flow control, which is why there is no equivalent of the iOS pump loop.
 */
@SuppressLint("MissingPermission")
class GattQueue(private val onIdle: () -> Unit = {}) {

    sealed interface Op {
        data class Write(
            val characteristic: BluetoothGattCharacteristic,
            val value: ByteArray,
            val writeType: Int,
            /** Fired after the stack accepts this write — used to advance progress. */
            val onWritten: ((success: Boolean) -> Unit)? = null,
        ) : Op

        data class Read(val characteristic: BluetoothGattCharacteristic) : Op

        data class Notify(
            val characteristic: BluetoothGattCharacteristic,
            val enable: Boolean,
        ) : Op

        data class Mtu(val size: Int) : Op
    }

    private val queue = ArrayDeque<Op>()
    private var current: Op? = null
    private var gatt: RadioGatt? = null

    @Synchronized
    fun attach(g: RadioGatt?) {
        // An operation in flight on another (closed) BluetoothGatt never
        // reports back. Left as `current`, it would block every later enqueue
        // forever: the next session's MTU request and admission read never
        // went out, and the app sat on "Connecting…" while the device showed
        // it connected (2026-10-11).
        if (g !== gatt) dropInFlight()
        gatt = g
        if (g == null) clear()
    }

    /** A new GATT session on [g]: start from an empty queue. */
    @Synchronized
    fun reset(g: RadioGatt) {
        dropInFlight()
        queue.clear()
        gatt = g
    }

    private fun dropInFlight() {
        val stale = current ?: return
        current = null
        if (stale is Op.Write) stale.onWritten?.invoke(false)
    }

    @Synchronized
    fun enqueue(op: Op) {
        queue.addLast(op)
        if (current == null) next()
    }

    /** Drop everything pending. The in-flight operation still reports back. */
    @Synchronized
    fun clear() {
        queue.clear()
    }

    /** Called from every GATT callback that completes an operation. */
    @Synchronized
    fun complete(success: Boolean) {
        val done = current
        current = null
        if (done is Op.Write) done.onWritten?.invoke(success)
        next()
    }

    private fun next() {
        val g = gatt ?: run { queue.clear(); return }
        while (current == null) {
            val op = queue.pollFirst() ?: run { onIdle(); return }
            current = op
            val issued = when (op) {
                is Op.Write -> g.writeCharacteristic(op.characteristic, op.value, op.writeType)
                is Op.Read -> g.readCharacteristic(op.characteristic)
                // setCharacteristicNotification + the CCCD write (Radio.kt).
                is Op.Notify -> g.setNotify(op.characteristic, op.enable)
                is Op.Mtu -> g.requestMtu(op.size)
            }
            // A rejected issue never produces a callback, so unwind it here or
            // the queue stalls forever on a single bad operation.
            if (!issued) {
                val failed = current
                current = null
                if (failed is Op.Write) failed.onWritten?.invoke(false)
            }
        }
    }
}
