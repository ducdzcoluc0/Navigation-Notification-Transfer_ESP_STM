#include <Arduino.h>
#include <SPI.h>
#include "Adafruit_GFX.h"
#include "config.h"
#include "mbedtls/base64.h"

#ifdef USE_TFT_ST7789
#include "Adafruit_ST7789.h"
#include "U8g2_for_Adafruit_GFX.h"
U8G2_FOR_ADAFRUIT_GFX u8g2;
#endif

#include "NimBLEDevice.h"
#include "myfont.h"
#include "disconnected_icon_9.h"
#include "arrived_icon.h" // 

#ifdef USE_TFT_ST7789
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
#define DISPLAY_COLOR_WHITE ST77XX_WHITE
#define DISPLAY_COLOR_BLACK ST77XX_BLACK
#define DISPLAY_COLOR_GREEN ST77XX_GREEN
#define DISPLAY_COLOR_RED   ST77XX_RED
#endif

extern const uint8_t u8g2_font_unifont_t_vietnamese2[15330] U8G2_FONT_SECTION("u8g2_font_unifont_t_vietnamese2");
extern const uint8_t u8g2_font_inr33_mf[11616] U8G2_FONT_SECTION("u8g2_font_inr33_mf");
extern const uint8_t u8g2_font_helvB18_tf[4956] U8G2_FONT_SECTION("u8g2_font_helvB18_tf");

static NimBLEServer* pServer;
static NimBLECharacteristic* pCharacteristic;
static bool deviceConnected = false;
static bool displayNeedsUpdate = true;

// QUẢN LÝ GIAO DIỆN CHỐNG GIẬT (ANTI-FLICKER)
static int currentDisplayMode = 0; // 1: Disconnect, 2: Waiting, 3: Navigating
static String lastBleSignature = ""; // Dấu ấn để kiểm tra BLE có đổi không

// DỮ LIỆU BLUETOOTH
String bleReceiveBuffer = "";
String title = "N/A", totalDistance = "N/A", eta = "N/A", dist = "N/A"; 
int iconID = -1;
uint8_t ramIconBuffer[512];
bool hasRamIcon = false;

// DỮ LIỆU UART (STM32)
String stmUartBuffer = "";
String currentGPSCoords = "Đang tìm vệ tinh..."; 
String currentGPSSpeed = "0"; 
String currentGPSTime = "--:--"; 
String currentGPSDate = "--/--/----";
String currentBattery = "0"; // 🔥 ĐÃ THÊM BIẾN QUẢN LÝ PIN

// QUẢN LÝ 3 GIÂY
unsigned long connectionStartTime = 0;
bool showConnectedText3s = false;

void drawBitmapPROGMEM(int16_t x, int16_t y, const uint8_t* bitmap, int16_t w, int16_t h, bool invertColor) {
    if (!bitmap) return;
    for (int16_t j = 0; j < h; j++) {
        int16_t startX = x; int16_t lineStart = 0; bool lastPixel = false;
        for (int16_t i = 0; i < w; i++) {
            int32_t pixelIndex = j * w + i;
            uint8_t b = pgm_read_byte(&bitmap[pixelIndex / 8]);
            bool currentPixel = (b >> (7 - (pixelIndex % 8))) & 0x01;
            if (currentPixel != lastPixel || i == w - 1) {
                if (i == w - 1 && currentPixel == lastPixel) i++;
                int16_t segmentWidth = i - lineStart;
                if (segmentWidth > 0) {
                    uint16_t color;
                    if (invertColor) color = lastPixel ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
                    else color = lastPixel ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;
                    tft.drawFastHLine(startX, y + j, segmentWidth, color);
                }
                startX = x + i; lineStart = i; lastPixel = currentPixel;
            }
        }
    }
}

void readGPSDataFromSTM32() {
    while (Serial2.available()) {
        char c = Serial2.read();
        if (c == '\n') {
            int startIdx = stmUartBuffer.indexOf("$$GPS|");
            int endIdx = stmUartBuffer.indexOf("$$", startIdx + 6);
            if (startIdx != -1 && endIdx != -1 && endIdx > startIdx) {
                String payload = stmUartBuffer.substring(startIdx + 6, endIdx);
                
                int p1 = payload.indexOf('|');
                int p2 = payload.indexOf('|', p1 + 1);
                int p3 = payload.indexOf('|', p2 + 1);
                int p4 = payload.indexOf('|', p3 + 1);

                if (p1 != -1 && p2 != -1) {
                    String newCoords = payload.substring(0, p1);
                    String newSpeed  = payload.substring(p1 + 1, p2);
                    String newTime   = "--:--";
                    String newDate   = currentGPSDate; 
                    String newBattery = currentBattery;

                    // 🔥 ĐÃ FIX: Tách chuẩn xác 5 trường dữ liệu (Tọa độ | Tốc độ | Giờ | Ngày | Pin)
                    if (p4 != -1) { 
                        newTime = payload.substring(p2 + 1, p3);
                        newDate = payload.substring(p3 + 1, p4);
                        newBattery = payload.substring(p4 + 1);
                    } else if (p3 != -1) { // Fallback 4 trường
                        newTime = payload.substring(p2 + 1, p3);
                        newDate = payload.substring(p3 + 1);
                    } else { // Fallback 3 trường
                        newTime = payload.substring(p2 + 1);
                    }

                    if (newCoords == "NO_FIX") newCoords = "Đang tìm vệ tinh...";

                    if (newCoords != currentGPSCoords || newSpeed != currentGPSSpeed || newTime != currentGPSTime || newDate != currentGPSDate || newBattery != currentBattery) {
                        currentGPSCoords = newCoords;
                        currentGPSSpeed = newSpeed;
                        currentGPSTime = newTime;
                        currentGPSDate = newDate;
                        currentBattery = newBattery; // Lưu lại Pin
                        displayNeedsUpdate = true; 
                    }
                }
            }
            stmUartBuffer = ""; 
        } else if (c != '\r') { stmUartBuffer += c; }
    }
}

void processBLEData() {
    int startIdx = bleReceiveBuffer.indexOf(">>>>>");
    int endIdx = bleReceiveBuffer.indexOf("<<<<<");
    if (startIdx != -1 && endIdx != -1 && endIdx > startIdx) {
        String payload = bleReceiveBuffer.substring(startIdx + 5, endIdx);
        
        int p1 = payload.indexOf('|'); int p2 = payload.indexOf('|', p1 + 1);
        int p3 = payload.indexOf('|', p2 + 1); int p4 = payload.indexOf('|', p3 + 1);
        int p5 = payload.indexOf('|', p4 + 1); 
        
        if (p1 != -1 && p2 != -1 && p3 != -1 && p4 != -1 && p5 != -1) {
            iconID = payload.substring(0, p1).toInt();
            String b64Image = payload.substring(p1 + 1, p2);
            title = payload.substring(p2 + 1, p3);
            totalDistance = payload.substring(p3 + 1, p4);
            eta = payload.substring(p4 + 1, p5);
            dist = payload.substring(p5 + 1); 

            Serial.println("\n[BLE GỬI TỚI]:");
            Serial.println("Icon ID  : " + String(iconID));
            Serial.println("Lệnh rẽ  : " + title);
            Serial.println("Sắp tới  : " + dist);
            Serial.println("Tổng KM  : " + totalDistance);
            Serial.println("ETA      : " + eta);
            Serial.println("Ảnh Base64: Nhận được " + String(b64Image.length()) + " ký tự");

            if (iconID == 99 && b64Image.length() > 0) {
                size_t outLen = 0;
                mbedtls_base64_decode(ramIconBuffer, sizeof(ramIconBuffer), &outLen, (const unsigned char*)b64Image.c_str(), b64Image.length());
                hasRamIcon = true;
            } else { hasRamIcon = false; }
            
            bleReceiveBuffer = bleReceiveBuffer.substring(endIdx + 5);
            displayNeedsUpdate = true;
            
        } else { bleReceiveBuffer = ""; }
    } else if (bleReceiveBuffer.length() > 1000) { bleReceiveBuffer = ""; }
}

class MyCharacteristicCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            bleReceiveBuffer += String(value.c_str());
            processBLEData();
        }
    }
};

class MyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override {
        deviceConnected = true;
        displayNeedsUpdate = true;
        connectionStartTime = millis();
        showConnectedText3s = true;
        Serial.println("\n>>> BLUETOOTH: ĐÃ KẾT NỐI APP! <<<\n");
    }
    void onDisconnect(NimBLEServer* pServer) override {
        deviceConnected = false;
        displayNeedsUpdate = true;
        showConnectedText3s = false;
        title = "N/A"; eta = "N/A"; dist = "N/A"; iconID = -1; hasRamIcon = false;
        bleReceiveBuffer = "";
        NimBLEDevice::startAdvertising();
        Serial.println("\n>>> BLUETOOTH: ĐÃ NGẮT KẾT NỐI! <<<\n");
    }
};

void updateDisplay() {
    // TÍNH TOÁN XEM ĐANG Ở CHẾ ĐỘ NÀO
    int targetMode = 3; 
    if (!deviceConnected) targetMode = 1; // Mất kết nối
    else if (iconID == 15 || iconID == -1 || title == "N/A" || title == "---") targetMode = 2; // Đang chờ
    else if (iconID == 5) targetMode = 4; // 🔥 CHẾ ĐỘ 4: ĐÃ ĐẾN NƠI (Độc chiếm màn hình)

    // CHỈ XÓA TOÀN BỘ MÀN HÌNH KHI ĐỔI CHẾ ĐỘ (CHỐNG GIẬT 2)
    bool modeChanged = (currentDisplayMode != targetMode);
    if (modeChanged) {
        tft.fillScreen(DISPLAY_COLOR_BLACK);
        currentDisplayMode = targetMode;

        // VẼ CÁC THÀNH PHẦN TĨNH (CHỈ VẼ 1 LẦN KHI CHUYỂN MODE)
        if (currentDisplayMode == 1) {
            tft.fillRect(0, 0, 240, 30, DISPLAY_COLOR_RED);
            u8g2.setFont(u8g2_font_helvB18_tf); u8g2.setForegroundColor(DISPLAY_COLOR_WHITE); u8g2.setBackgroundColor(DISPLAY_COLOR_RED);
            u8g2.setCursor(5, 22); u8g2.print("Disconnected");
            int disconnectX = (240 - 132) / 2;
            drawBitmapPROGMEM(disconnectX, 65, disconnected_icon_9, 132, 132, false); 
        } 
        else if (currentDisplayMode == 2) {
            u8g2.setFont(u8g2_font_unifont_t_vietnamese2); u8g2.setForegroundColor(DISPLAY_COLOR_WHITE); u8g2.setBackgroundColor(DISPLAY_COLOR_BLACK);
            int textW = u8g2.getUTF8Width("Đang chờ lộ trình...");
            u8g2.setCursor((240 - textW)/2, 160); u8g2.print("Đang chờ lộ trình...");
        }
        else if (currentDisplayMode == 4) {
            int arrivedIconX = (240 - 80) / 2;
            drawBitmapPROGMEM(arrivedIconX, 90, arrived_icon, 80, 80, true); 
            
            u8g2.setFont(u8g2_font_unifont_t_vietnamese2); 
            u8g2.setForegroundColor(DISPLAY_COLOR_GREEN); 
            u8g2.setBackgroundColor(DISPLAY_COLOR_BLACK);
            
            String arrivedText = "Đã đến nơi!"; 
            int textArrivedW = u8g2.getUTF8Width(arrivedText.c_str());
            u8g2.setCursor((240 - textArrivedW) / 2 > 0 ? (240 - textArrivedW) / 2 : 0, 210); 
            u8g2.print(arrivedText.c_str()); 
        }
    }

    if (currentDisplayMode == 4) return; // Nếu đã đến đích thì ngừng chạy code phía dưới

    // VẼ VỊ TRÍ GPS (Chỉ vẽ ở Chế độ 3)
    if (currentDisplayMode == 3) {
        tft.fillRect(0, 290, 240, 30, DISPLAY_COLOR_BLACK); 
        u8g2.setFont(u8g2_font_unifont_t_vietnamese2); u8g2.setForegroundColor(DISPLAY_COLOR_WHITE); u8g2.setBackgroundColor(DISPLAY_COLOR_BLACK);
        String gpsStr = "Vị trí: " + currentGPSCoords;
        int gpsW = u8g2.getUTF8Width(gpsStr.c_str());
        u8g2.setCursor((240 - gpsW) / 2 > 0 ? (240 - gpsW) / 2 : 0, 310);
        u8g2.print(gpsStr.c_str());
    }

    if (currentDisplayMode == 1) {
        tft.fillRect(0, 230, 240, 90, DISPLAY_COLOR_BLACK); 
        u8g2.setFont(u8g2_font_inr33_mf); u8g2.setForegroundColor(DISPLAY_COLOR_RED); u8g2.setBackgroundColor(DISPLAY_COLOR_BLACK);
        int timeW = u8g2.getUTF8Width(currentGPSTime.c_str());
        u8g2.setCursor((240 - timeW) / 2, 270); u8g2.print(currentGPSTime.c_str());

        u8g2.setFont(u8g2_font_helvB18_tf); u8g2.setForegroundColor(DISPLAY_COLOR_WHITE);
        int dateW = u8g2.getUTF8Width(currentGPSDate.c_str());
        u8g2.setCursor((240 - dateW) / 2, 305); u8g2.print(currentGPSDate.c_str());
    } 
    else if (currentDisplayMode == 2 || currentDisplayMode == 3) {
        float speedVal = currentGPSSpeed.toFloat();
        bool isOverspeed = (currentDisplayMode == 3 && speedVal > 35.0);

        uint16_t bgColor = isOverspeed ? DISPLAY_COLOR_RED : DISPLAY_COLOR_GREEN;
        uint16_t textColor = isOverspeed ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;

        tft.fillRect(0, 0, 240, 30, bgColor); 

        if (isOverspeed) {
            // Cảnh báo quá tốc độ -> Độc chiếm thanh trạng thái
            u8g2.setFont(u8g2_font_unifont_t_vietnamese2); 
            u8g2.setForegroundColor(textColor); 
            u8g2.setBackgroundColor(bgColor);
            String warningText = "Vượt quá tốc độ!";
            int warnW = u8g2.getUTF8Width(warningText.c_str());
            u8g2.setCursor((240 - warnW) / 2 > 0 ? (240 - warnW) / 2 : 0, 21);
            u8g2.print(warningText.c_str());
        } 
        else {
            // DÙNG 100% FONT TO ĐẬM CHO THANH TRẠNG THÁI
            u8g2.setFont(u8g2_font_helvB18_tf); 
            u8g2.setForegroundColor(textColor); 
            u8g2.setBackgroundColor(bgColor);

            if (currentDisplayMode == 2) {
                if (showConnectedText3s && (millis() - connectionStartTime < 3000)) {
                    // 3s đầu: Chữ Connected ở chính giữa
                    String connStr = "Connected";
                    int connW = u8g2.getUTF8Width(connStr.c_str());
                    u8g2.setCursor((240 - connW) / 2, 22);
                    u8g2.print(connStr.c_str());
                } else {
                    showConnectedText3s = false; 
                    
                    // Giờ góc trái
                    u8g2.setCursor(5, 22);
                    u8g2.print(currentGPSTime.c_str());
                    
                    // Pin góc phải (Dùng Font to hoàn toàn)
                    String batStr = currentBattery + "%";
                    int batW = u8g2.getUTF8Width(batStr.c_str());
                    u8g2.setCursor(235 - batW, 22);
                    u8g2.print(batStr.c_str());
                }
            } 
            else if (currentDisplayMode == 3) { // Đang dẫn đường
                // 1. TỐC ĐỘ GÓC TRÁI (Font to, bỏ dấu cách ở giữa số và km/h)
                String speedStr = currentGPSSpeed + "km/h"; 
                u8g2.setCursor(5, 22);
                u8g2.print(speedStr.c_str());
                
                // 2. GIỜ Ở CHÍNH GIỮA (Font to)
                int timeW = u8g2.getUTF8Width(currentGPSTime.c_str());
                u8g2.setCursor((240 - timeW) / 2, 22);
                u8g2.print(currentGPSTime.c_str());
                
                // 3. PIN Ở GÓC PHẢI (Font to)
                String batStr = currentBattery + "%";
                int batW = u8g2.getUTF8Width(batStr.c_str());
                u8g2.setCursor(235 - batW, 22);
                u8g2.print(batStr.c_str());
            }
        }
    }

    if (currentDisplayMode == 3) {
        String currentBleSig = String(iconID) + title + dist + totalDistance + eta;
        
        if (modeChanged || currentBleSig != lastBleSignature) {
            lastBleSignature = currentBleSig;
            tft.fillRect(0, 30, 240, 260, DISPLAY_COLOR_BLACK); 
            
            int iconX = (240 - 48) / 2;
            if (iconID == 99 && hasRamIcon) {
                tft.drawBitmap(iconX, 50, ramIconBuffer, 48, 48, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK);
            } 
            
            u8g2.setFont(u8g2_font_unifont_t_vietnamese2); u8g2.setForegroundColor(DISPLAY_COLOR_WHITE); u8g2.setBackgroundColor(DISPLAY_COLOR_BLACK);
            String line1 = title; String line2 = ""; int maxTitleWidth = 230; 
            if (u8g2.getUTF8Width(title.c_str()) > maxTitleWidth) {
                int splitIndex = -1; int mid = title.length() / 2;
                for (int i = 0; i < mid; i++) {
                    if (title.charAt(mid + i) == ' ') { splitIndex = mid + i; break; }
                    if (title.charAt(mid - i) == ' ') { splitIndex = mid - i; break; }
                }
                if (splitIndex != -1) { line1 = title.substring(0, splitIndex); line2 = title.substring(splitIndex + 1); }
            }
            int y_line1, y_line2, y_nextDist;
            if (line2 == "") { 
                y_line1 = 145; y_nextDist = 180; int w = u8g2.getUTF8Width(line1.c_str());
                u8g2.setCursor((240 - w) / 2 > 0 ? (240 - w) / 2 : 0, y_line1); u8g2.print(line1.c_str());
            } else { 
                y_line1 = 135; y_line2 = 155; y_nextDist = 190;
                int w1 = u8g2.getUTF8Width(line1.c_str()); u8g2.setCursor((240 - w1) / 2 > 0 ? (240 - w1) / 2 : 0, y_line1); u8g2.print(line1.c_str());
                int w2 = u8g2.getUTF8Width(line2.c_str()); u8g2.setCursor((240 - w2) / 2 > 0 ? (240 - w2) / 2 : 0, y_line2); u8g2.print(line2.c_str());
            }
            
            String nextText = dist; 
            if(nextText == "--") nextText = ""; 
            
            u8g2.setFont(u8g2_font_helvB18_tf);
            u8g2.setForegroundColor(DISPLAY_COLOR_WHITE); 
            int nextW = u8g2.getUTF8Width(nextText.c_str()); 
            int nextX = (240 - nextW) / 2; 
            u8g2.setCursor(nextX > 0 ? nextX : 0, y_nextDist); 
            u8g2.print(nextText.c_str());
            
            u8g2.setFont(u8g2_font_inr33_mf); u8g2.setForegroundColor(DISPLAY_COLOR_GREEN); int distW = u8g2.getUTF8Width(totalDistance.c_str());
            int distX = (240 - distW) / 2; u8g2.setCursor(distX > 0 ? distX : 0, 240); u8g2.print(totalDistance.c_str());
            u8g2.setFont(u8g2_font_unifont_t_vietnamese2); u8g2.setForegroundColor(DISPLAY_COLOR_WHITE);
            String etaText = "Dự kiến: " + eta; int etaW = u8g2.getUTF8Width(etaText.c_str());
            int etaX = (240 - etaW) / 2; u8g2.setCursor(etaX > 0 ? etaX : 0, 275); u8g2.print(etaText.c_str());
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(9600, SERIAL_8N1, 32, 33); 
    tft.init(240, 320); tft.setSPISpeed(40000000); tft.setRotation(0); tft.invertDisplay(false); tft.fillScreen(DISPLAY_COLOR_BLACK);
    u8g2.begin(tft);
    NimBLEDevice::init("WeNav_ESP32"); NimBLEDevice::setMTU(512);
    pServer = NimBLEDevice::createServer(); pServer->setCallbacks(new MyServerCallbacks());
    NimBLEService* pService = pServer->createService(NimBLEUUID("18199909-f923-426c-9fdd-1e7a884d8aa2"));
    pCharacteristic = pService->createCharacteristic(NimBLEUUID("a37b8b6d-00e9-41db-ad37-9808464cba1b"), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pCharacteristic->setCallbacks(new MyCharacteristicCallback());
    pService->start();
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(NimBLEUUID("18199909-f923-426c-9fdd-1e7a884d8aa2"));
    pAdvertising->start();
    updateDisplay();
}

void loop() {
    readGPSDataFromSTM32();

    if (deviceConnected && showConnectedText3s && (millis() - connectionStartTime >= 3000)) {
        showConnectedText3s = false;
        displayNeedsUpdate = true;
    }

    if (displayNeedsUpdate) {
        updateDisplay();
        displayNeedsUpdate = false;
    }
    delay(50);
}