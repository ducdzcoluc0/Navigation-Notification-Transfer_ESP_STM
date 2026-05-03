package me.trevi.gmapsparser

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.TextUtils
import android.util.Base64
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.floatingactionbutton.FloatingActionButton
import me.trevi.navparser.service.NavigationListener
import timber.log.Timber
import androidx.appcompat.app.AlertDialog

class MainActivity : AppCompatActivity() {

    private lateinit var bleManager: BleManager

    // UI Elements
    private lateinit var tvBleStatus: TextView
    private lateinit var ivNavIcon: ImageView
    private lateinit var tvInstruction: TextView
    private lateinit var tvNextDist: TextView
    private lateinit var tvTotalDist: TextView
    private lateinit var tvEta: TextView
    private lateinit var fab: FloatingActionButton

    // ==========================================
    // BỘ XỬ LÝ QUYỀN VÀ BẬT BLUETOOTH
    // ==========================================
    private val enableBtLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK) {
                tvBleStatus.text = "Đang quét tìm HUD..."
                tvBleStatus.setBackgroundColor(Color.parseColor("#F59E0B")) // Vàng
                bleManager.startScan()
            } else {
                tvBleStatus.text = "Lỗi: Bạn chưa bật Bluetooth!"
                tvBleStatus.setBackgroundColor(Color.parseColor("#EF4444")) // Đỏ
            }
        }

    private val requestPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { permissions ->
            val allGranted = permissions.entries.all { it.value }
            if (allGranted) {
                checkAndEnableBluetooth()
            } else {
                tvBleStatus.text = "Lỗi: Thiếu quyền Bluetooth!"
                tvBleStatus.setBackgroundColor(Color.parseColor("#EF4444")) // Đỏ
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvBleStatus = findViewById(R.id.tvBleStatus)
        ivNavIcon = findViewById(R.id.ivNavIcon)
        tvInstruction = findViewById(R.id.tvInstruction)
        tvNextDist = findViewById(R.id.tvNextDist)
        tvTotalDist = findViewById(R.id.tvTotalDist)
        tvEta = findViewById(R.id.tvEta)
        fab = findViewById(R.id.fab)

        clearUI()

        bleManager = BleManager(this)

        // Cập nhật giao diện khi Bluetooth thay đổi
        bleManager.onConnectionStateChange = { isConnected ->
            runOnUiThread {
                if (isConnected) {
                    tvBleStatus.text = "Bluetooth: Đã kết nối HUD"
                    tvBleStatus.setBackgroundColor(Color.parseColor("#10B981"))

                    //RA LỆNH SNAPSHOT: Ép Service tìm Maps và gửi dữ liệu ngay lập tức
                    NavigationListener.instance?.fetchCurrentMapsNotification()

                    // Kiểm tra xem sau khi Snapshot có dữ liệu không, nếu không thì hiện màn hình chờ
                    if (bleManager.currentPayload != null) {
                        updateUI(bleManager.currentPayload!!)
                    } else {
                        updateUI(">>>>>15||Đang chờ lộ trình...|--|--|--<<<<<")
                    }
                } else {
                    tvBleStatus.text = "Bluetooth: Kết nối lại? Nhấn vào đây... "
                    tvBleStatus.setBackgroundColor(Color.parseColor("#EF4444"))
                    clearUI()
                }
            }
        }

        // Bấm vào thanh Bluetooth để quét lại thủ công
        tvBleStatus.setOnClickListener {
            checkBluetoothPermissions()
        }

        // Nút mở Google Maps
        fab.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("google.navigation:q="))
            intent.setPackage("com.google.android.apps.maps")

            if (intent.resolveActivity(packageManager) != null) {
                // Đã cài Google Maps -> Mở bình thường
                startActivity(intent)
            } else {
                val launchIntent = packageManager.getLaunchIntentForPackage("com.google.android.apps.maps")
                if (launchIntent != null) {
                    // Dùng cách mở dự phòng
                    startActivity(launchIntent)
                } else {
                    //CHƯA CÀI GOOGLE MAPS -> HIỂN THỊ POPUP YÊU CẦU TẢI
                    showInstallGoogleMapsDialog()
                }
            }
        }

        // ĐỐI TƯỢNG BẮT DỮ LIỆU TỪ NAVIGATION LISTENER
        NavigationListener.bleDataCallback = { payload ->

            // Quét xem có phải lệnh Reset (ID 15) không
            if (payload.contains(">>>>>15|")) {
                bleManager.resetPayload() // Xóa sạch trí nhớ lộ trình cũ
            }

            bleManager.sendData(payload) // Luôn gửi cho ESP32 (bleManager sẽ tự chặn nếu mất kết nối)

            // Chỉ khi nào kết nối với ESP32 mới cho phép App hiển thị dữ liệu
            if (bleManager.isConnected) {
                runOnUiThread { updateUI(payload) }
            }
        }
    }

    // ==========================================
    // HÀM HIỂN THỊ HỘP THOẠI YÊU CẦU TẢI GOOGLE MAPS
    // ==========================================
    private fun showInstallGoogleMapsDialog() {
        AlertDialog.Builder(this)
            .setTitle("Thiếu ứng dụng bản đồ")
            .setMessage("WeNav hiện hiện đang tối ưu tốt nhất với Google Maps. Vui lòng tải ứng dụng để bắt đầu chuyến đi của bạn.")
            .setPositiveButton("Tải ngay") { _, _ ->
                try {
                    // Ưu tiên mở bằng ứng dụng CH Play có sẵn trên máy
                    val playStoreIntent = Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=com.google.android.apps.maps"))
                    startActivity(playStoreIntent)
                } catch (e: android.content.ActivityNotFoundException) {
                    // Nếu máy không có CH Play, mở bằng trình duyệt Web
                    val webIntent = Intent(Intent.ACTION_VIEW, Uri.parse("https://play.google.com/store/apps/details?id=com.google.android.apps.maps"))
                    startActivity(webIntent)
                }
            }
            .setNegativeButton("Hủy", null) // Bấm hủy sẽ tự đóng Dialog
            .show()
    }

    // ==========================================
    // CHẠY MỖI KHI MỞ APP (Cưỡng bức & Quét BT)
    // ==========================================
    override fun onResume() {
        super.onResume()

        // 1. Kiểm tra quyền Đọc Thông Báo
        if (isNotificationServiceEnabled()) {
            // NẾU ĐÃ CÓ QUYỀN -> DÙNG HÀM CƯỠNG BỨC ĐỂ ĐÁNH THỨC DỊCH VỤ
            toggleNotificationListenerService()
        } else {
            // CHƯA CÓ QUYỀN -> MỞ CÀI ĐẶT
            startActivity(Intent("android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"))
        }

        // 2. Yêu cầu bật Bluetooth và Tự động quét
        checkBluetoothPermissions()
    }


    private fun toggleNotificationListenerService() {
        try {
            val pm = packageManager
            val componentName = ComponentName(this, NavigationListener::class.java)


            pm.setComponentEnabledSetting(
                componentName,
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP
            )
            pm.setComponentEnabledSetting(
                componentName,
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP
            )

            Timber.d("Đã kích hoạt cưỡng bức NotificationListener!")
        } catch (e: Exception) {
            Timber.e("Lỗi kích hoạt cưỡng bức: $e")
        }
    }

    private fun checkBluetoothPermissions() {
        val permissionsNeeded = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            permissionsNeeded.add(Manifest.permission.BLUETOOTH_SCAN)
            permissionsNeeded.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            permissionsNeeded.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        val missingPermissions = permissionsNeeded.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (missingPermissions.isNotEmpty()) {
            requestPermissionLauncher.launch(missingPermissions.toTypedArray())
        } else {
            checkAndEnableBluetooth()
        }
    }

    private fun checkAndEnableBluetooth() {
        val bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter
        if (adapter == null || !adapter.isEnabled) {
            // YÊU CẦU BẬT BLUETOOTH NẾU ĐANG TẮT
            val enableBtIntent = Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)
            enableBtLauncher.launch(enableBtIntent)
        } else {
            // ĐÃ FIX LỖI Ở ĐÂY: Kiểm tra xem đã kết nối chưa trước khi báo Đang quét
            if (bleManager.isConnected) {
                tvBleStatus.text = "Bluetooth: Đã kết nối HUD"
                tvBleStatus.setBackgroundColor(Color.parseColor("#10B981"))
            } else {
                tvBleStatus.text = "Đang quét tìm HUD..."
                tvBleStatus.setBackgroundColor(Color.parseColor("#F59E0B"))
                bleManager.startScan()
            }
        }
    }

    private fun isNotificationServiceEnabled(): Boolean {
        val pkgName = packageName
        val flat = Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
        if (!TextUtils.isEmpty(flat)) {
            val names = flat.split(":")
            for (i in names.indices) {
                val cn = ComponentName.unflattenFromString(names[i])
                if (cn != null && TextUtils.equals(pkgName, cn.packageName)) return true
            }
        }
        return false
    }

    private fun clearUI() {
        tvInstruction.text = "Đang chờ HUD..."
        tvNextDist.text = "-- m"
        tvTotalDist.text = "-- km"
        tvEta.text = "--"
        ivNavIcon.setImageResource(android.R.drawable.ic_menu_close_clear_cancel) // Hiển thị dấu X
        ivNavIcon.clearColorFilter()
    }

    private fun updateUI(payload: String) {
        try {
            val cleanPayload = payload.replace(">>>>>", "").replace("<<<<<", "")
            val parts = cleanPayload.split("|")

            if (parts.size >= 6) {
                val iconId = parts[0]
                val b64Image = parts[1]
                val instruction = parts[2]
                val totalDist = parts[3]
                val eta = parts[4]

                var nextDist = parts[5]
                if (nextDist.trim().isEmpty()) {
                    nextDist = "0m"
                }

                tvInstruction.text = instruction
                tvNextDist.text = nextDist
                tvTotalDist.text = totalDist
                tvEta.text = eta

                if (iconId == "99" && b64Image.isNotEmpty()) {
                    val bitmap = decode1BitBase64ToBitmap(b64Image)
                    if (bitmap != null) {
                        ivNavIcon.setImageBitmap(bitmap)
                        ivNavIcon.setColorFilter(Color.parseColor("#1D4ED8"))
                    }
                }
                // 🔥 THÊM NHÁNH NÀY CHO SỰ KIỆN ĐẾN ĐÍCH (ID = 5)
                else if (iconId == "5") {
                    // Dùng icon dấu Check hoàn thành có sẵn của Android
                    ivNavIcon.setImageResource(android.R.drawable.ic_menu_mylocation)
                    ivNavIcon.setColorFilter(Color.parseColor("#10B981")) // Đổi màu Xanh lá cây báo thành công
                }
                // Trạng thái chờ lộ trình (ID = 15)
                else if (iconId == "15") {
                    tvInstruction.text = "Đang chờ lộ trình..."
                    tvNextDist.text = "-- m"
                    tvTotalDist.text = "-- km"
                    tvEta.text = "--"
                    ivNavIcon.setImageResource(android.R.drawable.ic_dialog_map)
                    ivNavIcon.clearColorFilter()
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun decode1BitBase64ToBitmap(base64Str: String): Bitmap? {
        try {
            val bytes = Base64.decode(base64Str, Base64.NO_WRAP)
            val size = 48
            val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)

            for (y in 0 until size) {
                for (x in 0 until size) {
                    val byteIndex = y * (size / 8) + x / 8
                    if (byteIndex < bytes.size) {
                        val b = bytes[byteIndex].toInt()
                        val isFg = (b shr (7 - (x % 8))) and 0x01
                        val color = if (isFg == 1) Color.WHITE else Color.TRANSPARENT
                        bitmap.setPixel(x, y, color)
                    }
                }
            }
            return bitmap
        } catch (e: Exception) {
            return null
        }
    }

    // ==========================================
    // DỌN DẸP BỘ NHỚ KHI ĐÓNG APP
    // ==========================================
    override fun onDestroy() {
        super.onDestroy()

        // Gọi hàm cleanup để hủy BroadcastReceiver và đóng GATT
        if (::bleManager.isInitialized) {
            bleManager.cleanup()
        }

        Timber.d("App đã đóng hoàn toàn, thu hồi toàn bộ tài nguyên Bluetooth.")
    }
}