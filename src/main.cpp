#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>

//config
const char* WIFI_SSID     = "malek";
const char* WIFI_PASSWORD = "malek2004";
const char* SERVER_URL    = "https://server-smart-delivery-systme-production.up.railway.app/upload";
const char* TZ_INFO       = "EET-2EEST,M4.5.5/0,M10.5.4/24";

//ESP32-CAM pin def
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

//RTC I2C pins
#define RTC_SDA_PIN 14
#define RTC_SCL_PIN 15

//user I/O
#define FLASH_LED_PIN 4
#define TRIGGER_PIN   13

//RTOS
static QueueHandle_t     frameQueue;
static SemaphoreHandle_t uploadBusy; // taken while upload is in progress

//frame pointer & timestamp
typedef struct {
    camera_fb_t* fb;
    char timestamp[24];
} FramePacket;

RTC_DS3231 rtc;
bool rtcAvailable = false;

// Latch state: true --> trigger already fired, wait for removal
static bool triggerFired = false;

bool initCamera() {
    camera_config_t config;
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
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }
    Serial.println("Camera initialized OK");
    return true;
}


bool connectWiFi() {
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("WiFi connection failed!");
    return false;
}


bool syncTimeFromNTP() {
    Serial.println("Starting NTP time sync...");
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
        Serial.println("Failed to get time from NTP.");
        return false;
    }
    Serial.printf("NTP time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return true;
}

bool initRTC() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!rtc.begin(&Wire)) {
        Serial.println("DS3231 not found!");
        return false;
    }
    DateTime now = rtc.now();
    Serial.printf("RTC time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
    return true;
}

bool updateRTCFromNTP() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Cannot update RTC: NTP not available.");
        return false;
    }
    DateTime ntpTime(
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec
    );
    rtc.adjust(ntpTime);
    delay(100);
    DateTime verify = rtc.now();
    Serial.printf("RTC updated to: %04d-%02d-%02d %02d:%02d:%02d\n",
                  verify.year(), verify.month(), verify.day(),
                  verify.hour(), verify.minute(), verify.second());
    return true;
}

// return timestamp string from RTC at the moment of call
String getTimestamp() {
    if (!rtcAvailable) return "no-rtc";
    DateTime now = rtc.now();
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d-%02d-%02d",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());
    return String(buf);
}

//trigger latch
//fires once when 5V connected, reset when 5V removed
bool triggerPressedEvent() {
    bool currentReading = (digitalRead(TRIGGER_PIN) == HIGH);

    if (currentReading == true && triggerFired == false) {
        triggerFired = true;
        return true;
    }

    if (currentReading == false) {
        triggerFired = false;
    }

    return false;
}

//TASK 1: TRIGGER TASK (Core 1)
// detects trigger, check upload is free, captures, push to queue
void triggerTask(void* pvParameters) {
    Serial.println("[TriggerTask] Started. Waiting for trigger...");

    for (;;) {
        if (triggerPressedEvent()) {
            Serial.println("[TriggerTask] Trigger detected.");

            //check if upload task is still busy 
            if (xSemaphoreTake(uploadBusy, 0) != pdTRUE) {
                // Upload still running --> buffer not free yet — skip entirely
                Serial.println("[TriggerTask] Upload in progress. Skipping.");
            } else {
                // uploadBusy was free --> give it back immediately
                xSemaphoreGive(uploadBusy);

                Serial.println("[TriggerTask] Capturing...");
                digitalWrite(FLASH_LED_PIN, HIGH);
                delay(50);
                camera_fb_t* fb = esp_camera_fb_get();
                digitalWrite(FLASH_LED_PIN, LOW);

                if (fb == NULL) {
                    Serial.println("[TriggerTask] Capture failed.");
                } else {
                    Serial.printf("[TriggerTask] Captured %d bytes.\n", fb->len);

                    //stamp time at capture moment
                    FramePacket packet;
                    packet.fb = fb;
                    String ts = getTimestamp();
                    strncpy(packet.timestamp, ts.c_str(), sizeof(packet.timestamp) - 1);
                    packet.timestamp[sizeof(packet.timestamp) - 1] = '\0';

                    if (xQueueSend(frameQueue, &packet, pdMS_TO_TICKS(500)) != pdTRUE) {
                        // Queue full
                        Serial.println("[TriggerTask] Queue full! Discarding frame.");
                        esp_camera_fb_return(fb);
                    } else {
                        Serial.println("[TriggerTask] Frame queued.");
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

//TASK 2: UPLOAD TASK (Core 0)
//wait for frames, upoad over HTTPS, free buffer
void uploadTask(void* pvParameters) {
    Serial.println("[UploadTask] Started. Waiting for frames...");

    for (;;) {
        FramePacket packet;

        // Blocks here consuming zero CPU until a frame arrives
        if (xQueueReceive(frameQueue, &packet, portMAX_DELAY) == pdTRUE) {
            if (packet.fb == NULL) {
                Serial.println("[UploadTask] NULL frame, skipping.");
                continue;
            }

            // Mark upload as busy — trigger task will not fire flash while this is held
            xSemaphoreTake(uploadBusy, portMAX_DELAY);

            String timestamp = String(packet.timestamp);
            Serial.printf("[UploadTask] Uploading [%s] (%d bytes)...\n",
                          timestamp.c_str(), packet.fb->len);

            WiFiClientSecure client;
            client.setInsecure(); // skip SSL cert verification

            HTTPClient http;
            http.begin(client, SERVER_URL);
            http.addHeader("Content-Type", "image/jpeg");
            http.addHeader("X-Timestamp", timestamp);
            http.setTimeout(15000);

            int httpCode = http.POST(packet.fb->buf, packet.fb->len);

            if (httpCode == 200) {
                Serial.printf("[UploadTask] Success! [%s]\n", timestamp.c_str());
            } else if (httpCode > 0) {
                Serial.printf("[UploadTask] HTTP error: %d\n", httpCode);
            } else {
                Serial.printf("[UploadTask] Connection error: %s\n",
                              http.errorToString(httpCode).c_str());
            }

            http.end();

            // Free the buffer before releasing the semaphore
            esp_camera_fb_return(packet.fb);
            Serial.println("[UploadTask] Buffer released.");

            // Mark upload as free — trigger task can now capture again
            xSemaphoreGive(uploadBusy);
        }
    }
}

//TASK 3: WIFI WATCHDOG (Core 0)
//check WiFi every 30s, reconnect if dropped
void wifiWatchdogTask(void* pvParameters) {
    Serial.println("[WiFiWatchdog] Started.");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFiWatchdog] WiFi lost. Reconnecting...");
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
                WiFi.setSleep(false);
                Serial.printf("[WiFiWatchdog] Reconnected! IP: %s\n",
                              WiFi.localIP().toString().c_str());
            } else {
                Serial.println("[WiFiWatchdog] Failed. Restarting...");
                delay(1000);
                ESP.restart();
            }
        } else {
            Serial.println("[WiFiWatchdog] WiFi OK.");
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Smart Delivery Lock Box ===");

    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);
    pinMode(TRIGGER_PIN, INPUT_PULLDOWN);

    rtcAvailable = initRTC();
    if (!rtcAvailable) {
        Serial.println("RTC error. Restarting...");
        delay(10000);
        ESP.restart();
    }

    if (!initCamera()) {
        Serial.println("Camera error. Restarting...");
        delay(10000);
        ESP.restart();
    }

    if (!connectWiFi()) {
        Serial.println("WiFi error. Restarting...");
        delay(10000);
        ESP.restart();
    }
    WiFi.setSleep(false);

    if (syncTimeFromNTP()) {
        updateRTCFromNTP();
    } else {
        Serial.println("WARNING: NTP failed. Using existing RTC time.");
    }

    //rtos primitives
    frameQueue  = xQueueCreate(1, sizeof(FramePacket));
    uploadBusy  = xSemaphoreCreateBinary();
    xSemaphoreGive(uploadBusy); // start as available

    if (frameQueue == NULL || uploadBusy == NULL) {
        Serial.println("Failed to create RTOS primitives! Restarting...");
        delay(3000);
        ESP.restart();
    }

    xTaskCreatePinnedToCore(triggerTask,      "TriggerTask",  4096,  NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(uploadTask,       "UploadTask",   16384, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(wifiWatchdogTask, "WiFiWatchdog", 4096,  NULL, 1, NULL, 0);

    Serial.println("All tasks started. Ready.");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}