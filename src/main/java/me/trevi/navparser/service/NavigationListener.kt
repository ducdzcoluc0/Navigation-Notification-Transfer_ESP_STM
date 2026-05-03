package me.trevi.navparser.service

import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.os.Build
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Base64
import me.trevi.navparser.lib.*
import timber.log.Timber as Log
import java.text.Normalizer
import android.annotation.SuppressLint

private const val GMAPS_PACKAGE = "com.google.android.apps.maps"

open class NavigationListener : NotificationListenerService() {

    companion object {
        var bleDataCallback: ((String) -> Unit)? = null
        @SuppressLint("StaticFieldLeak")
        var instance: NavigationListener? = null
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        instance = this
        Log.d("Service đã sẵn sàng nhận lệnh Snapshot")
    }

    override fun onListenerDisconnected() {
        instance = null
        super.onListenerDisconnected()
    }

    fun fetchCurrentMapsNotification() {
        try {
            val activeNotifs = activeNotifications
            for (sbn in activeNotifs) {
                if (sbn.packageName == "com.google.android.apps.maps") {
                    Log.d("Snapshot tìm thấy Maps! Đang ép cập nhật dữ liệu...")
                    onNotificationPosted(sbn)
                    return
                }
            }
            Log.d("Snapshot hoàn tất: Không thấy Google Maps đang dẫn đường.")
        } catch (e: Exception) {
            Log.e("Lỗi Snapshot: $e")
        }
    }

    private var lastNextDist: String = ""
    private var lastInstruction: String = "Đang chờ lộ trình..."
    private var lastDistanceInMeters: Int = 999999

    private fun parseToMeters(distRaw: String): Int {
        try {
            val s = distRaw.lowercase().replace(" ", "").replace(",", ".")
            if (s.contains("km")) {
                return (s.replace("km", "").toFloat() * 1000).toInt()
            }
            if (s.contains("m")) {
                return s.replace("m", "").toFloat().toInt()
            }
        } catch (e: Exception) {}
        return 999999
    }

    fun haveNotificationsAccess(): Boolean {
        val listeners = Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
        return listeners != null && this::class.qualifiedName.toString() in listeners
    }

    private fun isGoogleMapsNotification(sbn: StatusBarNotification?): Boolean {
        return sbn != null && sbn.isOngoing && GMAPS_PACKAGE in sbn.packageName
    }

    private fun removeAccentsAndPunctuation(s: String): String {
        val normalized = Normalizer.normalize(s, Normalizer.Form.NFD)
        val noAccent = normalized.replace("\\p{InCombiningDiacriticalMarks}+".toRegex(), "")
            .replace("đ", "d").replace("Đ", "D")
        return noAccent.replace("[\\p{Punct}]".toRegex(), " ")
    }

    private fun getIconBase64(sourceBitmap: Bitmap?): String {
        if (sourceBitmap == null) return ""
        try {
            val swBitmap = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && sourceBitmap.config == Bitmap.Config.HARDWARE) {
                sourceBitmap.copy(Bitmap.Config.ARGB_8888, true)
            } else {
                sourceBitmap
            }
            if (swBitmap == null) return ""

            val size = 48
            val finalBitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(finalBitmap)

            val zoom = 1.25f
            val zoomedSize = (size * zoom).toInt()
            val offset = (size - zoomedSize) / 2f

            val zoomedSource = Bitmap.createScaledBitmap(swBitmap, zoomedSize, zoomedSize, true)
            canvas.drawColor(Color.BLACK)
            canvas.drawBitmap(zoomedSource, offset, offset, null)

            val byteWidth = (size + 7) / 8
            val buffer = ByteArray(byteWidth * size) { 0 }

            for (y in 0 until size) {
                for (x in 0 until size) {
                    val pixel = finalBitmap.getPixel(x, y)
                    val alpha = Color.alpha(pixel)
                    val r = Color.red(pixel)
                    val g = Color.green(pixel)
                    val b = Color.blue(pixel)

                    val brightness = (r + g + b) / 3
                    val isFg = if (alpha > 128 && brightness > 127) 1 else 0

                    if (isFg == 1) {
                        val byteIndex = y * byteWidth + x / 8
                        buffer[byteIndex] = (buffer[byteIndex].toInt() or (1 shl (7 - (x % 8)))).toByte()
                    }
                }
            }
            return Base64.encodeToString(buffer, Base64.NO_WRAP)
        } catch (e: Exception) {
            Log.e("Lỗi xử lý ảnh Hardware: ${e.message}")
            return ""
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn == null || !isGoogleMapsNotification(sbn)) return

        try {
            // =================================================================
            // XỬ LÝ CHỈ ĐƯỜNG XE MÁY / XE ĐẠP / Ô TÔ (Turn-by-Turn)
            // =================================================================
            val gmapsNotification = GMapsNotification(applicationContext, sbn)
            val navData = gmapsNotification.navigationData

            applicationContext.sendBroadcast(Intent(NAVIGATION_DATA_UPDATED).apply {
                putExtra(NAVIGATION_DATA, navData)
            })

            val extras = sbn.notification.extras
            val titleExtra = extras.getCharSequence(android.app.Notification.EXTRA_TITLE)?.toString() ?: ""
            val textExtra = extras.getCharSequence(android.app.Notification.EXTRA_TEXT)?.toString() ?: ""

            var nextDist = navData.nextDirection.navigationDistance?.localeString ?: ""
            val totalDistRaw = navData.remainingDistance.localeString ?: ""
            val rawTime = navData.eta.duration?.localeString ?: (navData.eta.localeString ?: "")
            var instruction = navData.nextDirection.localeString ?: ""

            val distPattern = Regex("\\b\\d+([.,]\\d+)?\\s*(km|m)\\b", RegexOption.IGNORE_CASE)

            if (nextDist.isEmpty()) {
                val allText = "$titleExtra | $textExtra"
                val matches = distPattern.findAll(allText).map { it.value }.toList()
                val totalClean = totalDistRaw.replace(" ", "")
                val candidate = matches.firstOrNull { it.replace(" ", "") != totalClean }
                if (candidate != null) nextDist = candidate
            }

            if (nextDist.isEmpty() && distPattern.containsMatchIn(instruction)) {
                nextDist = distPattern.find(instruction)?.value ?: ""
            }

            if (nextDist.isNotEmpty()) lastNextDist = nextDist else nextDist = lastNextDist

            if (instruction.isEmpty() || distPattern.matches(instruction.trim())) {
                val tClean = titleExtra.replace(distPattern, "").trim()
                val xClean = textExtra.replace(distPattern, "").trim()
                instruction = if (tClean.length > xClean.length) tClean else xClean
            }

            instruction = instruction.replace(distPattern, "").replace("-", "").replace("  ", " ").trim()
            if (instruction.isNotEmpty()) lastInstruction = instruction else instruction = lastInstruction

            val sourceBitmap = navData.actionIcon?.bitmap
            val base64Image = getIconBase64(sourceBitmap)
            var iconId = if (base64Image.isNotEmpty()) 99 else 0

            val strClean = removeAccentsAndPunctuation(instruction).lowercase()

            val hasArrivedText = "den noi" in strClean || "đến nơi" in strClean ||
                    "da den" in strClean || "đã đến" in strClean ||
                    "den dich" in strClean || "đến đích" in strClean

            if (hasArrivedText) {
                iconId = 5
                instruction = "Đã đến nơi!"
            }

            val finalTotal = totalDistRaw.replace("\\s+".toRegex(), "")

            // Liên tục cập nhật khoảng cách thực tế để check "Về đích"
            lastDistanceInMeters = parseToMeters(finalTotal)

            val finalNext = nextDist.replace("\\s+".toRegex(), "")
            val finalEta = if (rawTime.isNotEmpty()) rawTime else "--"

            val finalPayload = ">>>>>$iconId|$base64Image|$instruction|$finalTotal|$finalEta|$finalNext<<<<<"
            android.util.Log.d("WeNav_Debug", "BẮT ĐƯỢC MAPS: IconID=$iconId | Lệnh=$instruction | Cách=$finalNext")

            bleDataCallback?.invoke(finalPayload)

        } catch (e: Exception) {
            Log.e("Lỗi bóc tách Maps: $e")
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        super.onNotificationRemoved(sbn)

        if (sbn.packageName == "com.google.android.apps.maps") {

            // KIỂM TRA NGƯỠNG KHOẢNG CÁCH (20 MÉT)
            if (lastDistanceInMeters <= 20) {
                Log.d("Logic: Đã đến đích! Khoảng cách cuối: $lastDistanceInMeters m")

                val arrivedPayload = ">>>>>5||Đã đến nơi!|--|--|--<<<<<"
                bleDataCallback?.invoke(arrivedPayload)

                android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                    val resetPayload = ">>>>>15||Đang chờ lộ trình...|--|--|--<<<<<"
                    bleDataCallback?.invoke(resetPayload)
                }, 5000)

            } else {
                Log.d("Logic: Hủy chuyến đi. Khoảng cách cuối: $lastDistanceInMeters m")
                val resetPayload = ">>>>>15||Kết thúc hành trình|--|--|--<<<<<"
                bleDataCallback?.invoke(resetPayload)
            }

            lastDistanceInMeters = 999999
        }
    }
}