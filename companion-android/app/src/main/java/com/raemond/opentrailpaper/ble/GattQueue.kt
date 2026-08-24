package com.raemond.opentrailpaper.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.os.Build
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
    private var gatt: BluetoothGatt? = null

    fun attach(g: BluetoothGatt?) {
        gatt = g
        if (g == null) clear()
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
                is Op.Write -> write(g, op)
                is Op.Read -> g.readCharacteristic(op.characteristic)
                is Op.Notify -> notify(g, op)
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

    @Suppress("DEPRECATION")
    private fun write(g: BluetoothGatt, op: Op.Write): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(op.characteristic, op.value, op.writeType) ==
                BluetoothGatt.GATT_SUCCESS
        } else {
            op.characteristic.writeType = op.writeType
            op.characteristic.value = op.value
            g.writeCharacteristic(op.characteristic)
        }

    @Suppress("DEPRECATION")
    private fun notify(g: BluetoothGatt, op: Op.Notify): Boolean {
        if (!g.setCharacteristicNotification(op.characteristic, op.enable)) return false
        // The local flag above only routes callbacks; the device is only told to
        // send anything once its CCCD is written, and THAT write is what this
        // operation waits on.
        val cccd = op.characteristic.getDescriptor(BikeUuid.cccd) ?: return false
        val value = if (op.enable) {
            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        } else {
            BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
        }
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, value) == BluetoothGatt.GATT_SUCCESS
        } else {
            cccd.value = value
            g.writeDescriptor(cccd)
        }
    }
}
