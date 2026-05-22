#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>
#include "esp_system.h"

// ─── CONFIG ───────────────────────────────────────────
const char* WIFI_SSID     = "A";
const char* WIFI_PASSWORD = "12345678";
const char* SERVER_URL    = "https://server-smart-delivery-systme-production.up.railway.app/upload";
const char* TZ_INFO       = "EET-2EEST,M4.5.5/0,M10.5.4/24";
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
// ──────────────────────────────────────────────────────

// ─── USER I/O ─────────────────────────────────────────
#define FLASH_LED_PIN 4
#define TRIGGER_PIN   13
// ──────────────────────────────────────────────────────

// ─── LATCH RELAY ──────────────────────────────────────
// Active-LOW relay:
// LOW  = relay ON / latch opens
// HIGH = relay OFF / latch closed
#define LATCH_RELAY_PIN    2
#define RELAY_ACTIVE_LEVEL LOW
#define RELAY_IDLE_LEVEL   HIGH
#define LATCH_OPEN_MS      10000
// ──────────────────────────────────────────────────────

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

// Shared flag between tasks
volatile bool waitingForCommand = false;

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
        case ESP_RST_POWERON:
            Serial.println("Power-on reset");
            break;

        case ESP_RST_SW:
            Serial.println("Software reset");
            break;

        case ESP_RST_PANIC:
            Serial.println("Exception / panic reset");
            break;

        case ESP_RST_INT_WDT:
            Serial.println("Interrupt watchdog reset");
            break;

        case ESP_RST_TASK_WDT:
            Serial.println("Task watchdog reset");
            break;

        case ESP_RST_WDT:
            Serial.println("Other watchdog reset");
            break;

        case ESP_RST_DEEPSLEEP:
            Serial.println("Wake from deep sleep");
            break;

        case ESP_RST_BROWNOUT:
            Serial.println("Brownout reset");
            Serial.println("[System] Previous reset was caused by power drop.");
            break;

        case ESP_RST_SDIO:
            Serial.println("SDIO reset");
            break;

        default:
            Serial.println("Unknown reset");
            break;
    }
}
// ──────────────────────────────────────────────────────

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

    // These legacy names may show warnings, but they still compile.
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
bool connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("[WiFi] Connection failed.");
    return false;
}

bool ensureWiFiConnected() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.println("[WiFi] Disconnected. Reconnecting...");
    WiFi.disconnect(true);
    delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        WiFi.setSleep(false);
        return true;
    }

    safeRestart("WiFi reconnect failed", 5000);
    return false;
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
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_mday,
                  timeinfo.tm_hour,
                  timeinfo.tm_min,
                  timeinfo.tm_sec);

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
                  now.year(),
                  now.month(),
                  now.day(),
                  now.hour(),
                  now.minute(),
                  now.second());

    return true;
}

bool updateRTCFromNTP() {
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo)) {
        Serial.println("[RTC] Cannot update: NTP time not available.");
        return false;
    }

    DateTime ntpTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );

    rtc.adjust(ntpTime);
    delay(100);

    DateTime verify = rtc.now();

    Serial.printf("[RTC] Updated to: %04d-%02d-%02d %02d:%02d:%02d\n",
                  verify.year(),
                  verify.month(),
                  verify.day(),
                  verify.hour(),
                  verify.minute(),
                  verify.second());

    return true;
}

String getTimestamp() {
    if (!rtcAvailable) return "no-rtc";

    DateTime now = rtc.now();

    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d-%02d-%02d",
             now.year(),
             now.month(),
             now.day(),
             now.hour(),
             now.minute(),
             now.second());

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

// ─── LATCH ────────────────────────────────────────────
void initLatch() {
    digitalWrite(LATCH_RELAY_PIN, RELAY_IDLE_LEVEL);
    pinMode(LATCH_RELAY_PIN, OUTPUT);
    digitalWrite(LATCH_RELAY_PIN, RELAY_IDLE_LEVEL);

    latchIsOpen = false;

    Serial.println("[Latch] Initialized.");
    Serial.println("[Latch] Active-LOW: LOW = open, HIGH = closed.");
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

// ─── CAPTURE & SEND ───────────────────────────────────
bool captureAndSend() {
    if (!ensureWiFiConnected()) {
        safeRestart("Cannot capture/upload because WiFi is unavailable", 5000);
        return false;
    }

    String timestamp = getTimestamp();
    Serial.printf("[%s] Capturing image...\n", timestamp.c_str());

    // Flash disabled to avoid brownout during capture.
    // GPIO4 is kept LOW and is not turned ON before esp_camera_fb_get().
    digitalWrite(FLASH_LED_PIN, LOW);

    camera_fb_t* fb = esp_camera_fb_get();

    digitalWrite(FLASH_LED_PIN, LOW);

    if (!fb) {
        Serial.println("[Camera] Capture failed.");
        safeRestart("Camera capture failed", 3000);
        return false;
    }

    Serial.printf("[Camera] Captured image: %d bytes\n", fb->len);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Timestamp", timestamp);
    http.setTimeout(30000);

    int httpCode = http.POST(fb->buf, fb->len);

    bool success = false;

    if (httpCode == 200) {
        Serial.printf("[HTTP] Image sent successfully! [%s]\n", timestamp.c_str());
        success = true;
    } else if (httpCode > 0) {
        Serial.printf("[HTTP] POST failed, code: %d\n", httpCode);
    } else {
        Serial.printf("[HTTP] POST error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
    esp_camera_fb_return(fb);

    if (success) {
        Serial.println("[Command] Waiting for decision...");
        waitingForCommand = true;
    } else {
        Serial.println("[HTTP] Upload failed. System will continue running.");
    }

    return success;
}

// ─── TASK 1: CAMERA + TRIGGER ─────────────────────────
void cameraTask(void* pvParameters) {
    Serial.println("[CameraTask] Started.");

    for (;;) {
        serviceLatch();

        if (triggerPressedEvent()) {
            Serial.println("[CameraTask] Trigger detected.");

            waitingForCommand = false;
            lastTriggerReading = false;
            debouncedTriggerState = false;

            captureAndSend();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── TASK 2: COMMAND POLLING ──────────────────────────
void pollTask(void* pvParameters) {
    Serial.println("[PollTask] Started.");

    for (;;) {
        if (waitingForCommand) {
            if (ensureWiFiConnected()) {
                Serial.println("[PollTask] Checking for decision...");

                WiFiClientSecure client;
                client.setInsecure();

                HTTPClient http;
                http.begin(client, "https://server-smart-delivery-systme-production.up.railway.app/command");
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
            }

            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
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
    Serial.println("[Flash] Disabled. GPIO4 kept LOW during capture.");

    initLatch();
    initTriggerInput();

    rtcAvailable = initRTC();
    if (!rtcAvailable) {
        safeRestart("RTC initialization failed", 10000);
    }

    if (!initCamera()) {
        safeRestart("Camera initialization failed", 10000);
    }

    if (!connectWiFi()) {
        safeRestart("WiFi initial connection failed", 10000);
    }

    WiFi.setSleep(false);

    if (syncTimeFromNTP()) {
        updateRTCFromNTP();
    } else {
        Serial.println("[NTP] WARNING: NTP sync failed. Using RTC's existing time.");
    }

    xTaskCreatePinnedToCore(cameraTask, "CameraTask", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(pollTask,   "PollTask",   8192, NULL, 1, NULL, 0);

    Serial.println("[System] Setup complete. All tasks started.");
}

// ─── LOOP ─────────────────────────────────────────────
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}
