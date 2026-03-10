#include <M5AtomS3.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedANADIG.h>
#include <cstring>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ピン設定
// ATOMIC TF Card Base (SPI):
//   CS: -1, MOSI: GPIO6, MISO: GPIO8, SCK: GPIO7
// Voltmeter Unit (I2C, Grove Port A):
//   SDA: GPIO2, SCL: GPIO1

// Unit ADC V1.1 (ADS1110) I2C address (default 0x48)
#define VMETER_ADDR 0x48

// Unit ADC V1.1 settings
uint8_t vmeter_addr = VMETER_ADDR;
bool vmeter_initialized = false;
constexpr float VMETER_FACTOR = 2.048F / 12.0F;        // Unit ADC V1.1(0-12V)の入力補正係数
float latest_voltage = 0.0F;

m5::unit::UnitUnified vmeter_units;
m5::unit::UnitADC11 vmeter_unit{VMETER_FACTOR, VMETER_ADDR};

// Recording flag
int rec_flag = 0;
unsigned long lastRecordTime = 0;
unsigned long lastPostTime = 0;
unsigned long lastButtonTime = 0;
bool wifiConnected = false;
static bool g_gotIp = false;

static void showWiFiProgress(const String& line1, const String& line2 = "") {
    // Reserve the lower area for transient WiFi progress logs.
    AtomS3.Display.fillRect(0, 88, 128, 40, BLACK);
    AtomS3.Display.setCursor(0, 88);
    AtomS3.Display.setTextSize(1);
    AtomS3.Display.setTextColor(CYAN, BLACK);
    AtomS3.Display.println(line1);
    if (line2.length() > 0) {
        AtomS3.Display.println(line2);
    }
}

static int monthIndex(const char* mon) {
    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (int i = 0; i < 12; ++i) {
        if (strncmp(mon, months[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

static bool parseHttpDateToEpoch(const String& dateHeader, time_t& outEpoch) {
    // RFC7231 date format: "Tue, 10 Mar 2026 12:34:56 GMT"
    char wday[4] = {0};
    char mon[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(dateHeader.c_str(), "%3s, %d %3s %d %d:%d:%d GMT", wday, &day, mon, &year, &hour, &minute,
               &second) != 7) {
        return false;
    }

    const int month = monthIndex(mon);
    if (month < 0) {
        return false;
    }

    struct tm utcTm = {};
    utcTm.tm_year = year - 1900;
    utcTm.tm_mon = month;
    utcTm.tm_mday = day;
    utcTm.tm_hour = hour;
    utcTm.tm_min = minute;
    utcTm.tm_sec = second;
    utcTm.tm_isdst = 0;

    // mktime uses local TZ. Convert as UTC by temporarily switching TZ.
    setenv("TZ", "UTC0", 1);
    tzset();
    const time_t epoch = mktime(&utcTm);
    setenv("TZ", "JST-9", 1);
    tzset();

    if (epoch <= 0) {
        return false;
    }

    outEpoch = epoch;
    return true;
}

static bool syncTimeFromHttpDate(unsigned long timeoutMs, String& failReason) {
    Serial.println("HTTP Date sync start (JST)");
    failReason = "UNKNOWN";

    const char* urls[] = {
        "https://www.google.com/generate_204",
        "https://example.com/",
        "http://www.msftconnecttest.com/connecttest.txt",
    };
    constexpr int urlCount = sizeof(urls) / sizeof(urls[0]);

    const unsigned long start = millis();
    unsigned long lastUiUpdate = 0;
    int index = 0;

    while ((millis() - start) < timeoutMs) {
        HTTPClient http;
        const char* headerKeys[] = {"Date"};
        const char* url = urls[index % urlCount];
        index++;

        showWiFiProgress("Time sync (HTTP)", String("try: ") + url);
        http.collectHeaders(headerKeys, 1);

        bool begun = false;
        String urlString(url);
        WiFiClientSecure secureClient;
        if (urlString.startsWith("https://")) {
            secureClient.setInsecure();
            begun = http.begin(secureClient, urlString);
        } else {
            begun = http.begin(urlString);
        }

        if (begun) {
            const int code = http.GET();
            String dateHeader;
            if (code > 0) {
                dateHeader = http.header("Date");
                Serial.printf("HTTP %d Date='%s'\n", code, dateHeader.c_str());
                if (dateHeader.length() == 0) {
                    failReason = String("NO_DATE[") + code + "]";
                }
            } else {
                Serial.printf("HTTP GET failed: %d\n", code);
                failReason = String("HTTP_ERR[") + code + "]";
                if (code == -1) {
                    failReason = "HTTP_ERR[-1]:CONN";
                }
            }
            http.end();

            if (dateHeader.length() > 0) {
                time_t epoch = 0;
                if (parseHttpDateToEpoch(dateHeader, epoch)) {
                    struct timeval nowTv = { .tv_sec = epoch, .tv_usec = 0 };
                    settimeofday(&nowTv, nullptr);

                    time_t now = time(nullptr);
                    struct tm timeInfo;
                    if (localtime_r(&now, &timeInfo) != nullptr) {
                        Serial.printf("HTTP Date synced: %04d/%02d/%02d %02d:%02d:%02d\n",
                                      timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                                      timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
                    }
                    return true;
                }
                Serial.println("Failed to parse Date header");
                failReason = "DATE_PARSE";
            }
        } else {
            Serial.printf("HTTP begin failed: %s\n", url);
            failReason = "HTTP_BEGIN";
            http.end();
        }

        const unsigned long elapsed = millis() - start;
        if ((elapsed - lastUiUpdate) >= 1000) {
            lastUiUpdate = elapsed;
            char buf[32];
            snprintf(buf, sizeof(buf), "HTTP wait %lus", elapsed / 1000);
            showWiFiProgress("Time sync...", String(buf));
        }

        delay(500);
    }

    if (failReason == "UNKNOWN") {
        failReason = "HTTP_TIMEOUT";
    }
    Serial.printf("HTTP Date sync timeout (%s)\n", failReason.c_str());
    return false;
}

// スプライト（ダブルバッファリング）
M5Canvas canvas(&AtomS3.Display);

// POSTリクエスト用のキューとタスクハンドル
QueueHandle_t postQueue;
TaskHandle_t postTaskHandle;
QueueHandle_t recordQueue;
TaskHandle_t recordTaskHandle;

// POSTデータ構造体
typedef struct {
    char url[256];
    char json[512];
} PostData;

// Recording用データ構造体
typedef struct {
    float voltage;
    float scaledValue;
} RecordData;

// 設定値 (デフォルト値を設定)
String ssid = "default-ssid";  // SDカードから読み込まれない場合のデフォルト
String password = "default-password";
String script_url = "https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec";
float full_scale_V = 3.0;  // 3Vをフルスケールとする
float full_scale_P = 100.0;
String unit_P = "%";
String machine_id = "ATOMS3-01";
float voltage_offset = 0.0F;

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        g_gotIp = true;
        Serial.printf("[GOT_IP] %s\n", WiFi.localIP().toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("[DISCONNECTED] reason=%u\n", info.wifi_sta_disconnected.reason);
    }
}

static bool connectWithBestBssid(const char* targetSsid, const char* targetPassword, unsigned long timeoutMs) {
    int bestIndex = -1;
    int32_t bestRssi = -127;
    const int count = WiFi.scanNetworks(false, true, false, 300, 0);

    for (int i = 0; i < count; ++i) {
        String foundSsid;
        uint8_t enc = 0;
        int32_t rssi = 0;
        uint8_t* bssid = nullptr;
        int32_t channel = 0;

        if (!WiFi.getNetworkInfo(i, foundSsid, enc, rssi, bssid, channel)) {
            continue;
        }

        if (foundSsid == targetSsid && rssi > bestRssi) {
            bestRssi = rssi;
            bestIndex = i;
        }
    }

    g_gotIp = false;
    const unsigned long start = millis();

    if (bestIndex >= 0) {
        const uint8_t* bestBssid = WiFi.BSSID(bestIndex);
        const int32_t bestChannel = WiFi.channel(bestIndex);
        Serial.printf("Connecting with BSSID lock (ch=%ld, RSSI=%ld)...\n", bestChannel, bestRssi);
        showWiFiProgress("WiFi: BSSID lock", String("ch=") + bestChannel + " RSSI=" + bestRssi);
        WiFi.begin(targetSsid, targetPassword, bestChannel, bestBssid, true);
    } else {
        Serial.println("Target SSID not found in scan. Trying normal connect...");
        showWiFiProgress("WiFi: normal connect", "SSID not in scan");
        WiFi.begin(targetSsid, targetPassword);
    }

    unsigned long lastProgressUpdate = 0;
    while ((millis() - start) < timeoutMs) {
        if (g_gotIp) {
            showWiFiProgress("WiFi connected", WiFi.localIP().toString());
            return true;
        }

        const unsigned long elapsed = millis() - start;
        if (elapsed - lastProgressUpdate >= 1000) {
            lastProgressUpdate = elapsed;
            char waitLine[40];
            snprintf(waitLine, sizeof(waitLine), "wait %lus / %lus", elapsed / 1000, timeoutMs / 1000);
            showWiFiProgress("WiFi connecting...", String(waitLine));
        }
        delay(200);
    }

    showWiFiProgress("WiFi timeout", "next attempt");
    WiFi.scanDelete();
    return false;
}

// Voltmeter Unitから電圧を読み取る関数
float readVoltage() {
    if (!vmeter_initialized) {
        return 0.0F;
    }

    vmeter_units.update();
    if (vmeter_unit.updated()) {
        latest_voltage = vmeter_unit.differentialVoltage() / 1000.0F;
    }

    float voltage = latest_voltage + voltage_offset;

    if (voltage < 0.0F) {
        voltage = 0.0F;
    }
    return voltage;
}

// SDカードから設定を読み込む関数
void loadConfig() {
    // ATOMIC TF Card Base用のSPIピン設定
    // SCK: GPIO7, MISO: GPIO8, MOSI: GPIO6, CS: -1
    Serial.println("Initializing SPI...");
    SPI.begin(7, 8, 6, -1);  // SCK, MISO, MOSI, SS
    delay(100);  // SPI初期化の安定待ち
    
    Serial.println("Mounting SD card...");
    // クロック速度を下げて試す（4MHz）
    if (!SD.begin(-1, SPI, 4000000)) {
        AtomS3.Display.setTextColor(YELLOW);
        AtomS3.Display.println("SD: Using");
        AtomS3.Display.println("defaults");
        Serial.println("SD Card Mount Failed - Using Default Values");
        Serial.println("Check:");
        Serial.println("- SD card is inserted");
        Serial.println("- Pin connections: SCK=7, MISO=8, MOSI=6, CS=-1");
        Serial.println("Continuing with default configuration...");
        delay(2000);
        return;  // デフォルト値のまま続行
    }

    Serial.println("SD Card initialized");
    
    File file = SD.open("/config.txt");
    if (!file) {
        AtomS3.Display.setTextColor(YELLOW);
        AtomS3.Display.println("No config");
        AtomS3.Display.println("Using def");
        Serial.println("Failed to open config.txt - Using Default Values");
        Serial.println("Create config.txt on SD card root");
        delay(2000);
        return;  // デフォルト値のまま続行
    }

    Serial.println("Reading config.txt...");
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("ssid=")) {
            ssid = line.substring(5);
            Serial.printf("SSID: %s\n", ssid.c_str());
        } else if (line.startsWith("password=")) {
            password = line.substring(9);
            Serial.println("Password loaded");
        } else if (line.startsWith("script_url=")) {
            script_url = line.substring(11);
            Serial.printf("Script URL: %s\n", script_url.c_str());
        } else if (line.startsWith("machine_id=")) {
            machine_id = line.substring(11);
            Serial.printf("Machine ID: %s\n", machine_id.c_str());
        } else if (line.startsWith("full_scale_V=")) {
            full_scale_V = line.substring(13).toFloat();
            Serial.printf("Full scale V: %.2f\n", full_scale_V);
        } else if (line.startsWith("full_scale_P=")) {
            full_scale_P = line.substring(13).toFloat();
            Serial.printf("Full scale P: %.2f\n", full_scale_P);
        } else if (line.startsWith("unit_P=")) {
            unit_P = line.substring(7);
            Serial.printf("Unit: %s\n", unit_P.c_str());
        } else if (line.startsWith("voltage_offset=")) {
            voltage_offset = line.substring(15).toFloat();
            Serial.printf("Voltage offset: %.3f\n", voltage_offset);
        }
    }
    file.close();
    Serial.println("Config loaded successfully from SD");
}

// HTTP POSTリクエストを送信する関数
void sendPostRequest(String url, String jsonText) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);  // 5秒タイムアウト
    int httpResponseCode = http.POST(jsonText);
    
    Serial.printf("POST Response: %d\n", httpResponseCode);
    http.end();
}

// POSTリクエストをキューに追加する関数
void queuePostRequest(String url, String jsonText) {
    PostData postData;
    strncpy(postData.url, url.c_str(), sizeof(postData.url) - 1);
    strncpy(postData.json, jsonText.c_str(), sizeof(postData.json) - 1);
    postData.url[sizeof(postData.url) - 1] = '\0';
    postData.json[sizeof(postData.json) - 1] = '\0';
    
    if (xQueueSend(postQueue, &postData, 0) != pdTRUE) {
        Serial.println("POST queue full, discarding request");
    }
}

// POSTリクエストを処理するタスク
void postTask(void* parameter) {
    PostData postData;
    
    while (true) {
        // キューからデータを取得（最大100ms待機）
        if (xQueueReceive(postQueue, &postData, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.println("Processing POST request in background...");
            sendPostRequest(String(postData.url), String(postData.json));
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // 短い間隔でCPUを解放
    }
}

// mapからJSON文字列を作成する関数
String createJsonFromMap(std::map<String, String> data) {
    DynamicJsonDocument doc(1024);
    for (auto& pair : data) {
        doc[pair.first] = pair.second;
    }
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

// LOGDATA.csvを初期化する関数
void initLogFile() {
    // LOGDATA.csvが存在するかチェック
    if (SD.exists("/LOGDATA.csv")) {
        Serial.println("LOGDATA.csv already exists");
        return;
    }
    
    // 新規作成してヘッダーを書き込む
    File logFile = SD.open("/LOGDATA.csv", FILE_WRITE);
    if (logFile) {
        logFile.println("timestamp,voltage,scaled_value,unit");
        logFile.close();
        Serial.println("LOGDATA.csv created with header");
    } else {
        Serial.println("Failed to create LOGDATA.csv");
    }
}

// HTTP POSTリクエストを送信する処理（1秒ごと）
void sendRecordingPost(float voltage, float scaledValue, const char* timestamp) {
    unsigned long currentTime = millis();
    if (currentTime - lastPostTime < 1000) {
        return;  // 1秒経過していないのでスキップ
    }
    lastPostTime = currentTime;
    
    if (WiFi.status() == WL_CONNECTED) {
        std::map<String, String> data;
        data["machine_id"] = machine_id;
        data["status"] = "rec";
        data["unit_P"] = unit_P;
        data["pressure"] = String(scaledValue, 1);
        data["voltage"] = String(voltage, 3);
        data["timestamp"] = String(timestamp);
        
        String jsonText = createJsonFromMap(data);
        Serial.printf("Queueing POST: %s\n", jsonText.c_str());
        queuePostRequest(script_url, jsonText);
    }
}

// 記録処理を行う関数（CSVを0.5秒ごと、POSTを1秒ごと）
void recording(float voltage, float scaledValue) {
    // 0.5秒ごとに実行
    unsigned long currentTime = millis();
    if (currentTime - lastRecordTime < 500) {
        return;
    }
    lastRecordTime = currentTime;
    
    // 現在時刻を取得
    char timestamp[32];
    
    // WiFi接続済みの場合はgetLocalTime()を使う（NTP同期のため）
    // WiFi未接続の場合はtime()とlocaltime()を使う（高速）
    if (wifiConnected) {
        struct tm timeInfo;
        if (getLocalTime(&timeInfo, 100)) {  // 100msタイムアウト
            snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                     timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                     timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
        } else {
            // フォールバック
            time_t now = time(nullptr);
            struct tm* timeInfoPtr = localtime(&now);
            if (timeInfoPtr != nullptr) {
                snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                         timeInfoPtr->tm_year + 1900, timeInfoPtr->tm_mon + 1, timeInfoPtr->tm_mday,
                         timeInfoPtr->tm_hour, timeInfoPtr->tm_min, timeInfoPtr->tm_sec);
            } else {
                snprintf(timestamp, sizeof(timestamp), "NO_TIME");
            }
        }
    } else {
        // WiFi未接続時は直接time()とlocaltime()を使う（タイムアウト無し）
        time_t now = time(nullptr);
        struct tm* timeInfoPtr = localtime(&now);
        if (timeInfoPtr != nullptr) {
            snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                     timeInfoPtr->tm_year + 1900, timeInfoPtr->tm_mon + 1, timeInfoPtr->tm_mday,
                     timeInfoPtr->tm_hour, timeInfoPtr->tm_min, timeInfoPtr->tm_sec);
        } else {
            snprintf(timestamp, sizeof(timestamp), "NO_TIME");
        }
    }
    
    // CSVファイルに記録（0.5秒ごと）
    File logFile = SD.open("/LOGDATA.csv", FILE_APPEND);
    if (logFile) {
        logFile.printf("%s,%.3f,%.3f,%s\n", timestamp, voltage, scaledValue, unit_P.c_str());
        logFile.close();
        // デバッグ用（コメントアウトで高速化）
        Serial.printf("Logged: %s,%.3f,%.3f,%s\n", timestamp, voltage, scaledValue, unit_P.c_str());
    } else {
        Serial.println("Failed to open LOGDATA.csv for writing");
    }
    
    // HTTP POSTリクエスト送信（1秒ごと、WiFi接続済みの場合のみ）
    if (wifiConnected) {
        sendRecordingPost(voltage, scaledValue, timestamp);
    }
}

// Recording用タスク（非同期処理）
void recordTask(void* parameter) {
    RecordData data;
    
    while (true) {
        // キューからデータを取得（最大100ms待機）
        if (xQueueReceive(recordQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
            recording(data.voltage, data.scaledValue);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    // Initialize M5AtomS3
    auto cfg = M5.config();
    AtomS3.begin(cfg);
    
    // Initialize display
    AtomS3.Display.setTextSize(1);
    AtomS3.Display.setTextColor(GREEN);
    AtomS3.Display.fillScreen(BLACK);
    AtomS3.Display.setCursor(0, 0);
    AtomS3.Display.println("AtomS3 Vmeter");
    
    Serial.begin(115200);
    Serial.println("M5AtomS3 Voltmeter initialized");

    // スプライト初期化（128x128ピクセル）
    canvas.createSprite(128, 128);
    canvas.setTextSize(1);
    Serial.println("Canvas sprite created");

    // Display MAC Address
    String macAddr = WiFi.macAddress();
    AtomS3.Display.print("MAC:");
    AtomS3.Display.println(macAddr);
    Serial.printf("MAC: %s\n", macAddr.c_str());
    delay(1000);
    
    // タイムゾーンのみ先に設定（NTPサーバ設定はWiFi接続後に実施）
    setenv("TZ", "JST-9", 1);
    tzset();
    Serial.println("Timezone configured (JST)");

    // POSTリクエスト用のキューを作成（5個まで保持）
    postQueue = xQueueCreate(5, sizeof(PostData));
    if (postQueue == NULL) {
        Serial.println("Failed to create POST queue");
    }
    
    // Recording用のキューを作成（10個まで保持）
    recordQueue = xQueueCreate(10, sizeof(RecordData));
    if (recordQueue == NULL) {
        Serial.println("Failed to create Record queue");
    }

    // Initialize I2C for Voltmeter Unit (Grove Port A)
    // SDA: GPIO2, SCL: GPIO1
    Wire.begin(2, 1, 400000U);
    Serial.println("I2C initialized for Vmeter");
    
    // Initialize Unit ADC V1.1 (ADS1110)
    vmeter_initialized = false;
    for (uint8_t addr = 0x48; addr <= 0x4B; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            vmeter_addr = addr;
            vmeter_initialized = true;
            break;
        }
    }

    if (!vmeter_initialized) {
        Serial.println("Failed to initialize Unit ADC V1.1 (ADS1110)!");
        AtomS3.Display.setTextColor(RED);
        AtomS3.Display.println("ADC FAIL");
    } else {
        vmeter_unit = m5::unit::UnitADC11{VMETER_FACTOR, vmeter_addr};
        auto cfg = vmeter_unit.config();
        cfg.start_periodic = true;
        cfg.sampling_rate = m5::unit::ads1110::Sampling::Rate15;
        cfg.pga = m5::unit::ads1110::PGA::Gain1;
        cfg.factor = VMETER_FACTOR;
        vmeter_unit.config(cfg);

        if (!vmeter_units.add(vmeter_unit, Wire) || !vmeter_units.begin()) {
            vmeter_initialized = false;
            Serial.println("Failed to begin UnitADC11 (M5Unit-ANADIG)");
            AtomS3.Display.setTextColor(RED);
            AtomS3.Display.println("ADC FAIL");
        }
    }

    if (vmeter_initialized) {
        vmeter_units.update(true);

        Serial.printf("Unit ADC V1.1 initialized at 0x%02X\n", vmeter_addr);
        Serial.println("Using M5Unit-ANADIG backend");
        
        AtomS3.Display.setTextColor(GREEN);
        AtomS3.Display.println("ADC OK");
    }
    
    // Initialize SD Card and load config
    AtomS3.Display.println("Reading SD...");
    loadConfig();
    
    // Initialize log file
    initLogFile();

    if (ssid == "") {
        AtomS3.Display.setTextColor(RED);
        AtomS3.Display.println("No SSID!");
        return;
    }

    // Connect to WiFi (robust connection with event + BSSID lock)
    AtomS3.Display.println("WiFi...");
    showWiFiProgress("WiFi setup", "preparing...");

    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect(true, true);
    delay(200);

    bool connected = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        Serial.printf("Attempt %d/3 to SSID: %s\n", attempt, ssid.c_str());
        showWiFiProgress(String("WiFi attempt ") + attempt + "/3", ssid);
        connected = connectWithBestBssid(ssid.c_str(), password.c_str(), 20000);
        if (connected) {
            break;
        }
        if (attempt < 3) {
            showWiFiProgress(String("Retry ") + (attempt + 1) + "/3", "reconnecting...");
        }
        WiFi.disconnect(true, true);
        delay(500);
    }

    if (connected) {
        wifiConnected = true;
        AtomS3.Display.setTextColor(GREEN);
        AtomS3.Display.println("WiFi OK");
        
        // Sync time with NTP (JST: UTC+9)
        AtomS3.Display.println("Time sync...");
        String timeSyncReason;
        if (syncTimeFromHttpDate(30000, timeSyncReason)) {
            struct tm timeInfo;
            time_t now = time(nullptr);
            localtime_r(&now, &timeInfo);
            AtomS3.Display.println("Time OK");
            Serial.printf("Time: %04d/%02d/%02d %02d:%02d:%02d\n",
                         timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                         timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
        } else {
            AtomS3.Display.println("Time NG");
            AtomS3.Display.println(timeSyncReason);
            Serial.printf("Time sync failed: %s\n", timeSyncReason.c_str());
        }
        delay(2000);
        
        // Send reboot status
        std::map<String, String> data;
        data["machine_id"] = machine_id;
        data["status"] = "reboot";
        String jsonText = createJsonFromMap(data);
        queuePostRequest(script_url, jsonText);
        
        // POSTタスクを起動（Core 0で実行）
        xTaskCreatePinnedToCore(
            postTask,           // タスク関数
            "PostTask",         // タスク名
            8192,               // スタックサイズ
            NULL,               // パラメータ
            1,                  // 優先度
            &postTaskHandle,    // タスクハンドル
            0                   // Core 0で実行
        );
        Serial.println("POST task started on Core 0");
        
        delay(2000);
    } else {
        AtomS3.Display.setTextColor(RED);
        AtomS3.Display.println("WiFi Failed");
        
        // WiFi接続失敗時、RTCを2000年1月1日0時0分0秒（JST）に設定
        struct tm timeInfo;
        timeInfo.tm_year = 2000 - 1900;  // 1900年からの経過年数
        timeInfo.tm_mon = 0;              // 月 (0-11)
        timeInfo.tm_mday = 1;             // 日 (1-31)
        timeInfo.tm_hour = 0;             // 時 (0-23)
        timeInfo.tm_min = 0;              // 分 (0-59)
        timeInfo.tm_sec = 0;              // 秒 (0-59)
        timeInfo.tm_isdst = -1;           // サマータイム情報なし
        
        // 2000年1月1日0時0分0秒（UTC）のUnix時刻: 946684800
        // JSTなので+9時間 = 946684800 - 32400 = 946652400
        time_t t = 946652400;  // 2000-01-01 00:00:00 JST (UTC 1999-12-31 15:00:00)
        struct timeval now = { .tv_sec = t };
        settimeofday(&now, NULL);
        
        Serial.println("RTC set to 2000-01-01 00:00:00 JST");
        
        // 設定後の時刻を確認
        delay(100);
        time_t check_time = time(nullptr);
        struct tm* check_tm = localtime(&check_time);
        if (check_tm != nullptr) {
            Serial.printf("RTC check: %04d-%02d-%02d %02d:%02d:%02d\n",
                         check_tm->tm_year + 1900, check_tm->tm_mon + 1, check_tm->tm_mday,
                         check_tm->tm_hour, check_tm->tm_min, check_tm->tm_sec);
        }
        
        AtomS3.Display.println("RTC: 2000");
    }
    
    // Recordingタスクを起動（Core 0で実行、WiFi状態に関わらず）
    xTaskCreatePinnedToCore(
        recordTask,         // タスク関数
        "RecordTask",       // タスク名
        8192,               // スタックサイズ
        NULL,               // パラメータ
        1,                  // 優先度
        &recordTaskHandle,  // タスクハンドル
        0                   // Core 0で実行
    );
    Serial.println("Record task started on Core 0");
}

void loop() {
    AtomS3.update();
    
    // ボタン押下で録画開始（停止はリセットで対応）
    unsigned long currentTime = millis();
    if (AtomS3.BtnA.wasPressed() && (currentTime - lastButtonTime > 500)) {
        if (rec_flag == 0) {
            rec_flag = 1;
            lastButtonTime = currentTime;
            
            Serial.println("Recording STARTED");
            
            // 録画開始時にLED点滅
            for (int i = 0; i < 3; i++) {
                AtomS3.Display.fillScreen(RED);
                delay(50);
                AtomS3.Display.fillScreen(BLACK);
                delay(50);
            }
        }
    }
    
    // スプライトに描画（オフスクリーンバッファ）
    canvas.fillScreen(BLACK);
    canvas.setCursor(0, 0);
    
    // 録画状態を表示
    if (rec_flag) {
        canvas.setTextColor(RED);
        canvas.setTextSize(1);
        canvas.println("[REC]");
    }
    
    canvas.setTextColor(GREENYELLOW);
    canvas.setTextSize(2);
    
    // Read voltage from Voltmeter Unit
    float voltage = readVoltage();
    
    // Display voltage
    canvas.printf("%.3fV\n", voltage);
    
    // Calculate and display scaled value
    float scaledValue = (voltage / full_scale_V) * full_scale_P;
    canvas.setTextSize(3);
    canvas.printf("%.3f\n", scaledValue);
    canvas.setTextSize(2);
    canvas.printf("%s\n", unit_P.c_str());
    
    // スプライトを画面に一気に転送（ちらつき防止）
    canvas.pushSprite(0, 0);
    
    // デバッグ用（コメントアウトで高速化）
    // Serial.printf("Voltage: %.3fV, Scaled: %.3f%s, RecFlag: %d\n", voltage, scaledValue, unit_P.c_str(), rec_flag);
    
    // rec_flag=1の時は記録データをキューに追加（非同期処理）
    if (rec_flag) {
        RecordData data;
        data.voltage = voltage;
        data.scaledValue = scaledValue;
        if (xQueueSend(recordQueue, &data, 0) != pdTRUE) {
            Serial.println("Record queue full, discarding data");
        }
    }
    
    delay(20);  // 画面更新を超高速化（約50fps）
}
