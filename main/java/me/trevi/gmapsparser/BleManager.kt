package me.trevi.gmapsparser

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Handler
import android.os.Looper
import timber.log.Timber
import java.util.UUID
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter

@SuppressLint("MissingPermission")
class BleManager(private val context: Context) {

    private val bluetoothManager: BluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var bluetoothGatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null

    private val SERVICE_UUID = UUID.fromString("18199909-f923-426c-9fdd-1e7a884d8aa2")
    private val CHAR_UUID = UUID.fromString("a37b8b6d-00e9-41db-ad37-9808464cba1b")
    private val DEVICE_NAME = "WeNav_ESP32"

    var isConnected = false
    private var isScanning = false
    private val handler = Handler(Looper.getMainLooper())

    // LƯU THIẾT BỊ ĐỂ AUTO-CONNECT
    private var targetDevice: BluetoothDevice? = null

    // BIẾN LƯU DỮ LIỆU ĐỌC NGẦM (Cho phép MainActivity đọc được)
    var currentPayload: String? = null
        private set

    var onConnectionStateChange: ((Boolean) -> Unit)? = null


    private val bluetoothStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == BluetoothAdapter.ACTION_STATE_CHANGED) {
                val state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)

                // NHÁNH 1: KHI BLUETOOTH TẮT
                if (state == BluetoothAdapter.STATE_TURNING_OFF || state == BluetoothAdapter.STATE_OFF) {
                    Timber.d("Phát hiện hệ điều hành vừa tắt Bluetooth!")

                    // 1. Ép hệ thống ghi nhận ngắt kết nối ngay lập tức
                    isConnected = false
                    isScanning = false
                    writeChar = null

                    // 2. Báo ra UI
                    handler.post { onConnectionStateChange?.invoke(false) }

                    // 3. Đóng băng GATT
                    try {
                        bluetoothGatt?.close()
                    } catch (e: Exception) {}
                    bluetoothGatt = null
                }
                // NHÁNH 2 (MỚI THÊM): KHI BLUETOOTH BẬT LẠI
                else if (state == BluetoothAdapter.STATE_ON) {
                    Timber.d("Phát hiện hệ điều hành vừa BẬT Bluetooth!")

                    // Đợi 1 giây để phần cứng Bluetooth khởi động hoàn toàn, sau đó tự động quét/kết nối lại
                    handler.postDelayed({
                        // Gọi lại startScan.
                        // Vì targetDevice đã được lưu từ lần kết nối trước,
                        // hàm startScan sẽ tự động gọi connectGatt cực kỳ thông minh!
                        startScan()
                    }, 1000)
                }
            }
        }
    }

    init {
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        context.registerReceiver(bluetoothStateReceiver, filter)
    }

    fun startScan() {
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) return
        if (isConnected) return

        // Ưu tiên tìm trong danh sách thiết bị đã Ghép nối (Cài đặt hệ thống)
        val pairedDevices = bluetoothAdapter.bondedDevices
        val pairedESP = pairedDevices?.find { it.name == DEVICE_NAME }
        if (pairedESP != null) {
            targetDevice = pairedESP
        }

        if (targetDevice != null) {
            Timber.d("Đã nhớ ESP32, tiến hành kết nối trực tiếp...")
            // Dọn rác GATT cũ trước khi kết nối mới
            bluetoothGatt?.close()

            // Dùng autoConnect = false ở lần chủ động kết nối để ép Android ưu tiên 100% công suất
            handler.post {
                bluetoothGatt = targetDevice!!.connectGatt(context, false, gattCallback)
            }
            return
        }

        if (isScanning) stopScan()

        isScanning = true
        Timber.d("Bắt đầu bật Radar quét tìm ESP32 lần đầu...")
        try {
            bluetoothAdapter.bluetoothLeScanner?.startScan(scanCallback)
            handler.postDelayed({ stopScan() }, 10000)
        } catch (e: Exception) {
            isScanning = false
        }
    }

    private fun stopScan() {
        if (isScanning) {
            try { bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback) } catch (e: Exception) {}
            isScanning = false
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            if (device.name == DEVICE_NAME) {
                Timber.d("Đã quét thấy ESP32!")
                stopScan()
                targetDevice = device // LƯU LẠI ĐỂ DÙNG LÂU DÀI
                handler.post {
                    bluetoothGatt = device.connectGatt(context, false, gattCallback)
                }
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                isConnected = true
                handler.post { onConnectionStateChange?.invoke(true) }
                gatt.discoverServices()
            }
            else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                isConnected = false
                writeChar = null
                handler.post { onConnectionStateChange?.invoke(false) }

                bluetoothGatt?.close()
                bluetoothGatt = null

                Timber.d("Mất kết nối! Kiểm tra điều kiện trước khi reconnect...")

                targetDevice?.let { device ->
                    // Nghỉ 1.5 giây để hệ điều hành xả hết lỗi xung đột
                    handler.postDelayed({

                        // Chỉ cho phép tự động kết nối lại NẾU Bluetooth đang BẬT
                        if (bluetoothAdapter?.isEnabled == true) {
                            Timber.d("Bluetooth đang bật, tiến hành reconnect ngầm...")
                            // Dùng autoConnect = true khi chạy ngầm chờ ESP bật lại

                            // SỬA this@BleManager THÀNH this
                            bluetoothGatt = device.connectGatt(context, true, this)

                        } else {
                            Timber.d("Bluetooth đang tắt, HỦY BỎ lệnh reconnect bóng ma.")
                        }

                    }, 1500)
                } ?: run {
                    // Tương tự, chỉ quét lại khi Bluetooth đang bật
                    if (bluetoothAdapter?.isEnabled == true) {
                        startScan()
                    }
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                val service = gatt.getService(SERVICE_UUID)
                writeChar = service?.getCharacteristic(CHAR_UUID)
                writeChar?.let { gatt.requestMtu(512) }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt?, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                currentPayload?.let { payload ->
                    handler.postDelayed({ sendData(payload) }, 500)
                }
            }
        }
    }

    fun sendData(data: String) {
        // Cập nhật dữ liệu ngầm bất kể đang kết nối hay không
        if (data.contains(">>>>>") && !data.contains(">>>>>15|") && !data.contains(">>>>>-1|")) {
            currentPayload = data
        }

        if (!isConnected || writeChar == null || bluetoothGatt == null) return
        try {
            val bytes = data.toByteArray(Charsets.UTF_8)
            writeChar?.value = bytes
            writeChar?.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            bluetoothGatt?.writeCharacteristic(writeChar)
        } catch (e: Exception) {}
    }

    fun resetPayload() {
        currentPayload = null
        Timber.d("Đã xóa bộ nhớ đệm lộ trình.")
    }

    fun cleanup() {
        try {
            // Hủy đăng ký lắng nghe sự kiện bật/tắt Bluetooth
            context.unregisterReceiver(bluetoothStateReceiver)
            Timber.d("Đã dọn dẹp BroadcastReceiver.")
        } catch (e: IllegalArgumentException) {
            Timber.e("Receiver chưa được đăng ký hoặc đã bị hủy: $e")
        } catch (e: Exception) {
            Timber.e("Lỗi khi hủy đăng ký Receiver: $e")
        }

        // Đóng cổng GATT để trả lại tài nguyên cho điện thoại
        bluetoothGatt?.close()
        bluetoothGatt = null
    }
}