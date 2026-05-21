#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>
#include "esp_system.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ─── CONFIG ───────────────────────────────────────────
const char* WIFI_SSID      = "A";
const char* WIFI_PASSWORD  = "12345678";
const char* SERVER_URL     = "https://server-smart-delivery-systme-production.up.railway.app/upload";
const char* COMMAND_URL    = "https://server-smart-delivery-systme-production.up.railway.app/command";
const char* HEALTH_URL     = "https://server-smart-delivery-systme-production.up.railway.app/command"; // reused as a cheap GET
const char* TZ_INFO        = "EET-2EEST,M4.5.5/0,M10.5.4/24";
const char* BLE_DEVICE_NAME = "SmartLockBox";

// BLE UUIDs — standard SPP-like service
#define BLE_SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHARACTERISTIC_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// ─── MODE / FALLBACK CONFIG ───────────────────────────
#define WIFI_DOWN_THRESHOLD_MS    60000UL  // WiFi must be unreachable for this long → switch to BLE
#define WIFI_HEALTH_CHECK_MS      10000UL  // how often to ping the server in WiFi mode
#define WIFI_RECOVERY_INTERVAL_MS 60000UL  // how often to re-check WiFi while in BLE mode
#define WIFI_RECOVERY_TIMEOUT_MS  10000UL  // how long to wait for WiFi during a recovery attempt
#define DECISION_TIMEOUT_MS       30000UL  // give up waiting for unlock decision after this long
// ──────────────────────────────────────────────────────

// ESP32-CAM AI-Thinker pin definitions
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ─── DS3231 RTC I2C PINS ──────────────────────────────
#define RTC_SDA_PIN 14
#define RTC_SCL_PIN 15

// ─── USER I/O ─────────────────────────────────────────
#define FLASH_LED_PIN 4
#define TRIGGER_PIN   13

// ─── LATCH RELAY ──────────────────────────────────────
#define LATCH_RELAY_PIN    12
#define RELAY_ACTIVE_LEVEL HIGH
#define RELAY_IDLE_LEVEL   LOW
#define LATCH_OPEN_MS      10000

// ─── MODE STATE ───────────────────────────────────────
enum ConnectivityMode { MODE_WIFI, MODE_BLE };
volatile ConnectivityMode currentMode = MODE_WIFI;

// Guards
volatile bool bleConnected = false;
volatile bool waitingForCommand = false;   // only used for WiFi HTTP poll path
volatile bool modeTransitionInProgress = false;

BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool bleInitialized = false;

RTC_DS3231 rtc;
bool rtcAvailable = false;

// Trigger debounce
bool lastTriggerReading    = false;
bool debouncedTriggerState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelayMs = 50;

// Latch state
bool latchIsOpen = false;
unsigned long latchOpenedAt = 0;

// WiFi health tracking
unsigned long lastHealthCheck = 0;
unsigned long firstWiFiFailureAt = 0;  // 0 means "currently healthy"
unsigned long lastWiFiRecoveryAttempt = 0;

// Decision timeout tracking
unsigned long waitingForCommandStartedAt = 0;

// Mutex to serialize HTTPS requests across tasks (prevents SSL heap exhaustion)
SemaphoreHandle_t httpsMutex = nullptr;
#define HTTPS_MUTEX_TIMEOUT_MS 5000UL

// Forward declarations
void initBLE();
void deinitBLE();
bool connectWiFi(unsigned long timeoutMs);
void switchToBLEMode();
void attemptWiFiRecovery();
void openLatch();

// ─── SYSTEM HELPERS ───────────────────────────────────
void safeRestart(const char* reason, uint32_t delayMs = 3000) {
    Serial.println();
    Serial.println("========================================");
    Serial.print("[System] Critical error: ");
    Serial.println(reason);
    Serial.printf("[System] Restarting in %lu ms...\n", (unsigned long)delayMs);
    Serial.println("========================================");
    Serial.flush();
    delay(delayMs);
    ESP.restart();
}

void printResetReason() {
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.print("[System] Reset reason: ");
    switch (reason) {
        case ESP_RST_POWERON:   Serial.println("Power-on reset"); break;
        case ESP_RST_SW:        Serial.println("Software reset"); break;
        case ESP_RST_PANIC:     Serial.println("Exception / panic reset"); break;
        case ESP_RST_INT_WDT:   Serial.println("Interrupt watchdog reset"); break;
        case ESP_RST_TASK_WDT:  Serial.println("Task watchdog reset"); break;
        case ESP_RST_WDT:       Serial.println("Other watchdog reset"); break;
        case ESP_RST_DEEPSLEEP: Serial.println("Wake from deep sleep"); break;
        case ESP_RST_BROWNOUT:
            Serial.println("Brownout reset");
            Serial.println("[System] Previous reset was caused by power drop.");
            break;
        case ESP_RST_SDIO:      Serial.println("SDIO reset"); break;
        default:                Serial.println("Unknown reset"); break;
    }
}

// ─── LATCH ────────────────────────────────────────────
void initLatch() {
    digitalWrite(LATCH_RELAY_PIN, RELAY_IDLE_LEVEL);
    pinMode(LATCH_RELAY_PIN, OUTPUT);
    digitalWrite(LATCH_RELAY_PIN, RELAY_IDLE_LEVEL);
    latchIsOpen = false;
    Serial.println("[Latch] Initialized.");
}

void openLatch() {
    Serial.printf("[Latch] Opening for %lu ms...\n", (unsigned long)LATCH_OPEN_MS);
    digitalWrite(LATCH_RELAY_PIN, RELAY_ACTIVE_LEVEL);
    latchIsOpen = true;
    latchOpenedAt = millis();
}

void closeLatch() {
    digitalWrite(LATCH_RELAY_PIN, RELAY_IDLE_LEVEL);
    latchIsOpen = false;
    Serial.println("[Latch] Closed.");
}

void serviceLatch() {
    if (latchIsOpen && (millis() - latchOpenedAt >= LATCH_OPEN_MS)) {
        closeLatch();
    }
}

// ─── BLE CALLBACKS ────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        bleConnected = true;
        Serial.println("[BLE] Phone connected!");
    }

    void onDisconnect(BLEServer* pServer) override {
        bleConnected = false;
        Serial.println("[BLE] Phone disconnected.");
        if (currentMode == MODE_BLE && bleInitialized) {
            pServer->getAdvertising()->start();
            Serial.println("[BLE] Advertising restarted — waiting for reconnection...");
        }
    }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        String value = pCharacteristic->getValue().c_str();
        value.trim();

        Serial.printf("[BLE] Received from phone: '%s'\n", value.c_str());

        // Direct remote control: accept true/false anytime in BLE mode
        if (value == "true") {
            Serial.println("[BLE] Unlock command received!");
            openLatch();
        } else if (value == "false") {
            Serial.println("[BLE] Deny command received. Staying locked.");
        } else {
            Serial.printf("[BLE] Unknown command: '%s' — ignoring.\n", value.c_str());
        }
    }
};

void initBLE() {
    if (bleInitialized) {
        Serial.println("[BLE] Already initialized.");
        return;
    }

    Serial.printf("[BLE] Initializing as '%s'...\n", BLE_DEVICE_NAME);

    BLEDevice::init(BLE_DEVICE_NAME);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(BLE_SERVICE_UUID);

    pCharacteristic = pService->createCharacteristic(
        BLE_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    pCharacteristic->setCallbacks(new CharacteristicCallbacks());
    pCharacteristic->addDescriptor(new BLE2902());

    pService->start();

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();

    bleInitialized = true;
    Serial.println("[BLE] Advertising started — discoverable.");
}

void deinitBLE() {
    if (!bleInitialized) return;

    Serial.println("[BLE] Deinitializing BLE stack...");
    BLEDevice::deinit(false);
    bleConnected = false;
    bleInitialized = false;
    pServer = nullptr;
    pCharacteristic = nullptr;
    Serial.println("[BLE] BLE stack released.");
}

// ─── CAMERA ───────────────────────────────────────────
bool initCamera() {
    camera_config_t config;
    memset(&config, 0, sizeof(config));

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[Camera] Init failed: 0x%x\n", err);
        return false;
    }
    Serial.println("[Camera] Initialized OK");
    return true;
}

// ─── WIFI ─────────────────────────────────────────────
bool connectWiFi(unsigned long timeoutMs) {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("[WiFi] Connection failed.");
    return false;
}

// Lightweight ping: do a quick GET to the server. Returns true if reachable.
bool checkServerReachable() {
    if (WiFi.status() != WL_CONNECTED) return false;

    // Acquire HTTPS mutex to avoid concurrent TLS contexts
    if (xSemaphoreTake(httpsMutex, pdMS_TO_TICKS(HTTPS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        Serial.println("[Health] Could not acquire HTTPS mutex — skipping check.");
        return true;  // assume healthy rather than triggering false-positive failure
    }

    bool result = false;
    {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        if (http.begin(client, HEALTH_URL)) {
            http.setTimeout(3000);
            int code = http.GET();
            http.end();
            result = (code > 0);
        }
    }

    xSemaphoreGive(httpsMutex);
    return result;
}

// ─── MODE TRANSITIONS ─────────────────────────────────
void switchToBLEMode() {
    if (currentMode == MODE_BLE) return;
    modeTransitionInProgress = true;

    Serial.println();
    Serial.println("========================================");
    Serial.println("[Mode] WiFi unreachable for >60s — switching to BLE fallback");
    Serial.println("========================================");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);

    initBLE();

    currentMode = MODE_BLE;
    lastWiFiRecoveryAttempt = millis();
    waitingForCommand = false;
    modeTransitionInProgress = false;

    Serial.println("[Mode] Now in BLE mode. Phone can connect and send true/false anytime.");
}

void attemptWiFiRecovery() {
    if (currentMode != MODE_BLE) return;

    Serial.println();
    Serial.println("----------------------------------------");
    Serial.println("[Recovery] Starting WiFi recovery attempt...");
    if (bleConnected) {
        Serial.println("[Recovery] Note: Phone is connected via BLE. It will be disconnected briefly.");
    }
    Serial.println("----------------------------------------");

    modeTransitionInProgress = true;

    Serial.println("[Recovery] Step 1/4: Tearing down BLE stack...");
    deinitBLE();
    delay(200);

    Serial.printf("[Recovery] Step 2/4: Attempting WiFi connection (timeout %lu ms)...\n",
                  (unsigned long)WIFI_RECOVERY_TIMEOUT_MS);
    bool wifiOk = connectWiFi(WIFI_RECOVERY_TIMEOUT_MS);

    bool serverOk = false;
    if (wifiOk) {
        Serial.println("[Recovery] Step 3/4: WiFi connected. Verifying server is reachable...");
        serverOk = checkServerReachable();
        Serial.printf("[Recovery] Step 3/4: Server reachable: %s\n", serverOk ? "YES" : "NO");
    } else {
        Serial.println("[Recovery] Step 3/4: WiFi connection failed — skipping server check.");
    }

    if (wifiOk && serverOk) {
        Serial.println("[Recovery] Step 4/4: Committing to WiFi mode.");
        currentMode = MODE_WIFI;
        firstWiFiFailureAt = 0;
        lastHealthCheck = millis();
        modeTransitionInProgress = false;
        Serial.println("[Recovery] ✓ SUCCESS — Now in WiFi mode.");
        Serial.println("----------------------------------------");
    } else {
        Serial.println("[Recovery] Step 4/4: Recovery failed — restoring BLE mode.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(200);
        initBLE();
        lastWiFiRecoveryAttempt = millis();
        modeTransitionInProgress = false;
        Serial.printf("[Recovery] ✗ FAILED — staying in BLE. Next attempt in %lu sec.\n",
                      (unsigned long)(WIFI_RECOVERY_INTERVAL_MS / 1000));
        Serial.println("----------------------------------------");
    }
}

// ─── NTP ──────────────────────────────────────────────
bool syncTimeFromNTP() {
    Serial.println("[NTP] Starting time sync...");
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < 20) {
        Serial.print(".");
        delay(500);
        retries++;
    }
    Serial.println();

    if (retries >= 20) {
        Serial.println("[NTP] Failed to get time.");
        return false;
    }
    Serial.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return true;
}

// ─── RTC ──────────────────────────────────────────────
bool initRTC() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    if (!rtc.begin(&Wire)) {
        Serial.println("[RTC] DS3231 not found on I2C bus.");
        return false;
    }

    DateTime now = rtc.now();
    Serial.printf("[RTC] Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
    return true;
}

bool updateRTCFromNTP() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("[RTC] Cannot update: NTP time not available.");
        return false;
    }
    DateTime ntpTime(
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec
    );
    rtc.adjust(ntpTime);
    delay(100);
    DateTime verify = rtc.now();
    Serial.printf("[RTC] Updated to: %04d-%02d-%02d %02d:%02d:%02d\n",
                  verify.year(), verify.month(), verify.day(),
                  verify.hour(), verify.minute(), verify.second());
    return true;
}

String getTimestamp() {
    if (!rtcAvailable) return "no-rtc";
    DateTime now = rtc.now();
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d-%02d-%02d",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());
    return String(buf);
}

// ─── TRIGGER ──────────────────────────────────────────
void initTriggerInput() {
    pinMode(TRIGGER_PIN, INPUT_PULLUP);
}

bool triggerPressedEvent() {
    bool reading = (digitalRead(TRIGGER_PIN) == LOW);

    if (reading != lastTriggerReading) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelayMs) {
        if (reading != debouncedTriggerState) {
            debouncedTriggerState = reading;
            if (debouncedTriggerState == true) {
                lastTriggerReading = reading;
                return true;
            }
        }
    }

    lastTriggerReading = reading;
    return false;
}

// ─── CAPTURE & SEND (WiFi mode only) ──────────────────
bool captureAndSend() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Upload] WiFi not connected — cannot upload.");
        return false;
    }

    String timestamp = getTimestamp();
    Serial.printf("[%s] Capturing image...\n", timestamp.c_str());

    // Turn on flash BEFORE discarding stale frames so the sensor
    // adjusts exposure to the new lighting condition.
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(100);  // brief settling time for flash + sensor exposure

    // Discard up to 2 stale frames that were captured before the flash.
    // With fb_count=1, the camera continuously fills the buffer in the background,
    // so the first fb_get() returns whatever frame was completed most recently
    // (likely from before we pressed the trigger).
    for (int i = 0; i < 2; i++) {
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale) {
            esp_camera_fb_return(stale);
        }
    }

    // Now grab the actual fresh frame we want
    camera_fb_t* fb = esp_camera_fb_get();
    digitalWrite(FLASH_LED_PIN, LOW);

    if (!fb) {
        Serial.println("[Camera] Capture failed.");
        return false;
    }

    Serial.printf("[Camera] Captured image: %d bytes\n", fb->len);
    Serial.printf("[Memory] Free heap before upload: %u bytes\n", ESP.getFreeHeap());

    // Copy image data to our own buffer, then immediately release the camera framebuffer.
    // This frees ~50-80KB of internal RAM that camera was holding, which is critical
    // for the TLS handshake (mbedTLS needs ~30-50KB of contiguous heap).
    size_t imgLen = fb->len;
    uint8_t* imgBuf = (uint8_t*)malloc(imgLen);
    if (!imgBuf) {
        Serial.println("[Upload] Failed to allocate image buffer — aborting.");
        esp_camera_fb_return(fb);
        return false;
    }
    memcpy(imgBuf, fb->buf, imgLen);
    esp_camera_fb_return(fb);
    fb = nullptr;

    Serial.printf("[Memory] Free heap after fb release: %u bytes\n", ESP.getFreeHeap());

    // Acquire mutex so we don't run TLS concurrently with the health check
    if (xSemaphoreTake(httpsMutex, pdMS_TO_TICKS(HTTPS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        Serial.println("[Upload] Could not acquire HTTPS mutex — aborting.");
        free(imgBuf);
        return false;
    }

    bool success = false;
    int httpCode = 0;

    {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        http.begin(client, SERVER_URL);
        http.addHeader("Content-Type", "image/jpeg");
        http.addHeader("X-Timestamp", timestamp);
        http.setTimeout(30000);

        httpCode = http.POST(imgBuf, imgLen);

        if (httpCode == 200) {
            Serial.printf("[HTTP] Image sent successfully! [%s]\n", timestamp.c_str());
            success = true;
        } else if (httpCode > 0) {
            Serial.printf("[HTTP] POST failed, code: %d\n", httpCode);
        } else {
            Serial.printf("[HTTP] POST error: %s\n", http.errorToString(httpCode).c_str());
        }

        http.end();
    }

    xSemaphoreGive(httpsMutex);
    free(imgBuf);

    Serial.printf("[Memory] Free heap after upload: %u bytes\n", ESP.getFreeHeap());

    if (success) {
        Serial.println("[Command] Waiting for decision via HTTP poll...");
        waitingForCommand = true;
        waitingForCommandStartedAt = millis();
    }

    return success;
}

// ─── TASK 1: CAMERA + TRIGGER (Core 1) ────────────────
void cameraTask(void* pvParameters) {
    Serial.println("[CameraTask] Started.");

    for (;;) {
        serviceLatch();

        if (modeTransitionInProgress) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (triggerPressedEvent()) {
            Serial.println();
            Serial.println("[CameraTask] >>> Trigger detected <<<");

            // Log current state so user can see what's being interrupted
            if (latchIsOpen) {
                Serial.println("[CameraTask] Note: latch is currently open — capturing anyway.");
            }
            if (waitingForCommand) {
                Serial.println("[CameraTask] Note: was waiting for previous decision — resetting and starting new capture.");
            }

            if (currentMode == MODE_WIFI) {
                waitingForCommand = false;
                lastTriggerReading = false;
                debouncedTriggerState = false;
                captureAndSend();
            } else {
                // BLE mode: phone controls the lock directly via BLE writes.
                // No upload, no waiting flag — just log the trigger.
                Serial.println("[CameraTask] BLE mode — trigger logged. Phone controls lock directly.");
                lastTriggerReading = false;
                debouncedTriggerState = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── TASK 2: CONNECTIVITY MANAGER (Core 0) ────────────
void pollTask(void* pvParameters) {
    Serial.println("[PollTask] Started.");
    lastHealthCheck = millis();

    for (;;) {
        if (modeTransitionInProgress) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (currentMode == MODE_WIFI) {
            // ─── Background WiFi health check ─────────
            if ((millis() - lastHealthCheck) >= WIFI_HEALTH_CHECK_MS) {
                lastHealthCheck = millis();
                bool reachable = checkServerReachable();

                if (reachable) {
                    if (firstWiFiFailureAt != 0) {
                        Serial.println("[Health] ✓ WiFi RECOVERED — server reachable again.");
                        firstWiFiFailureAt = 0;
                    } else {
                        Serial.printf("[Health] ✓ WiFi healthy. IP: %s, RSSI: %d dBm\n",
                                      WiFi.localIP().toString().c_str(),
                                      WiFi.RSSI());
                    }
                } else {
                    wl_status_t wifiStatus = WiFi.status();
                    const char* statusStr =
                        (wifiStatus == WL_CONNECTED)      ? "associated (no internet?)" :
                        (wifiStatus == WL_NO_SSID_AVAIL)  ? "SSID not found" :
                        (wifiStatus == WL_CONNECT_FAILED) ? "connection failed" :
                        (wifiStatus == WL_DISCONNECTED)   ? "disconnected" :
                        (wifiStatus == WL_IDLE_STATUS)    ? "idle" :
                        "unknown";

                    if (firstWiFiFailureAt == 0) {
                        firstWiFiFailureAt = millis();
                        Serial.printf("[Health] ✗ WiFi unreachable (status: %s) — starting 60s timer.\n", statusStr);
                    } else {
                        unsigned long downFor = millis() - firstWiFiFailureAt;
                        unsigned long secLeft = (WIFI_DOWN_THRESHOLD_MS > downFor)
                                                ? (WIFI_DOWN_THRESHOLD_MS - downFor) / 1000
                                                : 0;
                        Serial.printf("[Health] ✗ WiFi still down (status: %s) for %lu sec — %lu sec until BLE fallback\n",
                                      statusStr, downFor / 1000, secLeft);
                        if (downFor >= WIFI_DOWN_THRESHOLD_MS) {
                            switchToBLEMode();
                            continue;
                        }
                    }
                }
            }

            // ─── Poll for HTTP unlock decision ────────
            if (waitingForCommand && WiFi.status() == WL_CONNECTED) {
                // Check if we've been waiting too long
                unsigned long waitedFor = millis() - waitingForCommandStartedAt;
                if (waitedFor >= DECISION_TIMEOUT_MS) {
                    Serial.printf("[PollTask] ⏱ Decision timeout after %lu sec — clearing wait flag. Trigger is re-armed.\n",
                                  waitedFor / 1000);
                    waitingForCommand = false;
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }

                Serial.printf("[PollTask] Checking for decision... (%lu sec elapsed, %lu sec until timeout)\n",
                              waitedFor / 1000,
                              (DECISION_TIMEOUT_MS - waitedFor) / 1000);

                // Acquire HTTPS mutex
                if (xSemaphoreTake(httpsMutex, pdMS_TO_TICKS(HTTPS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
                    Serial.println("[PollTask] Could not acquire HTTPS mutex — will retry.");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }

                WiFiClientSecure client;
                client.setInsecure();

                HTTPClient http;
                http.begin(client, COMMAND_URL);
                http.setTimeout(3000);

                int httpCode = http.GET();

                if (httpCode == 200) {
                    String payload = http.getString();
                    payload.trim();
                    Serial.printf("[PollTask] Received: %s\n", payload.c_str());

                    if (payload == "true") {
                        Serial.println("[PollTask] Unlock command received!");
                        openLatch();
                        waitingForCommand = false;
                    } else if (payload == "false") {
                        Serial.println("[PollTask] Deny command received.");
                        waitingForCommand = false;
                    } else {
                        Serial.println("[PollTask] No decision yet...");
                    }
                } else {
                    Serial.printf("[PollTask] HTTP error: %d\n", httpCode);
                }

                http.end();
                xSemaphoreGive(httpsMutex);
                vTaskDelay(pdMS_TO_TICKS(2000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            // MODE_BLE: periodically try to recover WiFi, and log heartbeat
            unsigned long sinceRecovery = millis() - lastWiFiRecoveryAttempt;
            unsigned long secUntilNext = (WIFI_RECOVERY_INTERVAL_MS > sinceRecovery)
                                         ? (WIFI_RECOVERY_INTERVAL_MS - sinceRecovery) / 1000
                                         : 0;

            // Heartbeat every 10s
            static unsigned long lastBleHeartbeat = 0;
            if ((millis() - lastBleHeartbeat) >= 10000UL) {
                lastBleHeartbeat = millis();
                Serial.printf("[BLE Mode] Heartbeat — phone %s, %lu sec until next WiFi recovery attempt\n",
                              bleConnected ? "CONNECTED" : "not connected",
                              secUntilNext);
            }

            if (sinceRecovery >= WIFI_RECOVERY_INTERVAL_MS) {
                attemptWiFiRecovery();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// ─── SETUP ────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("=== Smart Delivery Lock Box ===");

    printResetReason();

    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);
    Serial.println("[Flash] GPIO4 kept LOW.");

    initLatch();
    initTriggerInput();

    httpsMutex = xSemaphoreCreateMutex();
    if (httpsMutex == nullptr) {
        safeRestart("Failed to create HTTPS mutex", 5000);
    }

    rtcAvailable = initRTC();
    if (!rtcAvailable) {
        safeRestart("RTC initialization failed", 10000);
    }

    if (!initCamera()) {
        safeRestart("Camera initialization failed", 10000);
    }

    if (connectWiFi(20000)) {
        currentMode = MODE_WIFI;
        if (syncTimeFromNTP()) {
            updateRTCFromNTP();
        } else {
            Serial.println("[NTP] WARNING: NTP sync failed (server unreachable?). Using RTC's existing time.");
            Serial.println("[NTP] Note: NTP failure does NOT mean WiFi is down. Health check will verify.");
        }
    } else {
        Serial.println("[Setup] WiFi unavailable at boot — starting in BLE mode.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(200);
        initBLE();
        currentMode = MODE_BLE;
        lastWiFiRecoveryAttempt = millis();
    }

    xTaskCreatePinnedToCore(cameraTask, "CameraTask", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(pollTask,   "PollTask",   8192, NULL, 1, NULL, 0);

    Serial.println("[System] Setup complete. All tasks started.");
    Serial.println();
    Serial.println("========================================");
    Serial.printf("[System] READY — Mode: %s\n",
                  currentMode == MODE_WIFI ? "WiFi" : "BLE");
    Serial.println("[System] Press the trigger button to test.");
    Serial.println("========================================");
    Serial.println();
}

// ─── LOOP ─────────────────────────────────────────────
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}
