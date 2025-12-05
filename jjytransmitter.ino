/*
 * ---------------------------------------------------------------------------
 *  ESP32-C3 JJY Time Signal Transmitter (Production)
 *  
 *  Description:
 *      Generates a 40kHz JJY-compatible time signal to synchronize radio 
 *      clocks. Optimized for long battery life and unreliable internet.
 *  
 *  Key Features:
 *      1. Precision Timing: Uses High-Res Hardware Timer (esp_timer) in IRAM 
 *         and forced XTAL during sleep to eliminate drift.
 *      2. Robust NTP: Prioritizes low-latency Google servers (Asia). Falls 
 *         back to hardcoded IPs if DNS fails.
 *      3. Drift Fix: Detects wake source (Deep Sleep vs Reset) to prevent 
 *         test signals from corrupting midnight alignment.
 *      4. Smart Power: Deep Sleep during day (5uA). Light Sleep at night 
 *         to maintain clock precision.
 *      5. Remote Logging: Batches system logs and uploads to Google Sheets 
 *         upon boot and before deep sleep.
 * ---------------------------------------------------------------------------
 */

#include <WiFi.h>
#include <WiFiManager.h> 
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>      
#include <Preferences.h>
#include <esp_sntp.h>
#include <esp_netif_sntp.h> 
#include <esp_timer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ================= CONFIGURATION =================

const char* G_SCRIPT_URL = "https://script.google.com/macros/s/YOUR_LONG_DEPLOYMENT_ID_HERE/exec";
const char* DEVICE_NAME  = "ESP32-JJY-01";

const char* WIFI_SSID_DEFAULT = "your_wifi";
const char* WIFI_PASS_DEFAULT = "your_password";

const int PIN_LED = 8;    
const int PIN_ANT = 1;    

const long GMT_OFFSET_SEC = 9 * 3600; // JST (UTC+9)
const int JJY_FREQ = 40000; 
const int TIME_SHIFT_SEC = 0; 

// Schedule (Hours in 24h format)
const int BOOT_TX_MINUTES = 10;      
const int NIGHT_TX_MINUTES = 15;     
const int NIGHT_START_HOUR = 0;      
const int NIGHT_END_HOUR = 5;        
const int WAKEUP_HOUR = 23;          
const int WAKEUP_MIN = 50;

// ================= GLOBALS =================

enum SystemState { STATE_BOOT_SYNC, STATE_RUNNING };
SystemState currentState = STATE_BOOT_SYNC;

struct tm timeinfo;
Preferences preferences;

unsigned long bootTime = 0;
esp_timer_handle_t jjyTimerHandle = nullptr;
bool isScheduledWake = false; 
String logBuffer = ""; 

const int PWM_RES = 8;
const int DUTY_50 = 127;
const int CODE_MARKER = 2; 
const int CODE_LOW = 0;
const int CODE_HIGH = 1;

volatile int currentSecondCode = -1; 
volatile int currentSlot100ms = 0;

// ================= PROTOTYPES =================
void setupWiFi();
void ensureNTPSyncOrReboot();
void startTransmission();
void stopTransmission();
void enterDaytimeDeepSleep();
void enterLightSleep(uint64_t seconds);
void IRAM_ATTR onTimer(void* arg); 
int calculateJJYCodeForTime(time_t t);
int int3bcd(int a);
int parity8(int a);
void time_sync_notification_cb(struct timeval *tv);
void sysLog(String msg);
void uploadLogs();

// ============================================================
// MAIN SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000); 
  sysLog("=== Boot Sequence Started ===");

  // Determine if this is a scheduled wake (23:50) or a manual reset
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
      sysLog("Wake Source: Scheduled Deep Sleep. Skipping Boot Test.");
      isScheduledWake = true;
  } else {
      sysLog("Wake Source: Manual Reset. Boot Test Enabled.");
      isScheduledWake = false;
  }

  // Force 40MHz Crystal ON during Light Sleep to prevent time drift
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_ON);
  
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_ANT, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  digitalWrite(PIN_ANT, LOW);

  bootTime = millis();
  
  setupWiFi();

  currentState = STATE_BOOT_SYNC;
  ensureNTPSyncOrReboot(); 

  // Upload initial boot logs before cutting WiFi
  sysLog("Setup complete. Disconnecting WiFi.");
  uploadLogs();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  currentState = STATE_RUNNING;
  
  // Only start immediate transmission if it's a manual reset (test mode)
  if (!isScheduledWake && (millis() - bootTime < (BOOT_TX_MINUTES * 60 * 1000UL))) {
      startTransmission();
  }
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  time_t now;
  time(&now);
  time_t jst_now = now + GMT_OFFSET_SEC;
  localtime_r(&jst_now, &timeinfo);

  // Pre-calculate JJY code for the ISR
  static time_t lastSec = 0;
  if (now != lastSec) {
    lastSec = now;
    currentSecondCode = calculateJJYCodeForTime(jst_now + TIME_SHIFT_SEC);
  }

  // 1. Boot Test Mode (Manual Reset Only)
  if (!isScheduledWake && (millis() - bootTime < (BOOT_TX_MINUTES * 60 * 1000UL))) {
      if (!jjyTimerHandle) startTransmission();
      delay(100); 
      return;
  }

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;

  // 2. Pre-Midnight Buffer (23:50 -> 00:00)
  // Stops TX and holds precise time until midnight alignment
  if (h == 23 && m >= WAKEUP_MIN) {
     if (jjyTimerHandle) stopTransmission(); 
     int secRemaining = (59 - m) * 60 + (60 - timeinfo.tm_sec);
     
     sysLog("Pre-Midnight. Holding XTAL (" + String(secRemaining) + "s)");
     enterLightSleep(secRemaining);
     return;
  }

  // 3. Night Schedule (00:00 -> 05:00)
  if (h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR) {
    // Transmit Window
    if (m < NIGHT_TX_MINUTES) {
      if (!jjyTimerHandle) startTransmission();
      delay(100);
      return; 
    } 
    // Idle Window (Power Saving)
    else {
      if (jjyTimerHandle) stopTransmission();
      int secRemaining = (59 - m) * 60 + (60 - timeinfo.tm_sec);
      
      sysLog("Night Idle (" + String(h) + ":15). Sleep " + String(secRemaining) + "s");
      enterLightSleep(secRemaining);
      return;
    }
  }

  // 4. Daytime Deep Sleep
  if (jjyTimerHandle) stopTransmission();
  
  sysLog("Morning reached. Uploading Night Logs & Sleeping.");
  uploadLogs(); 
  
  enterDaytimeDeepSleep();
}

// ============================================================
// LOGGING & UPLOAD
// ============================================================

void sysLog(String msg) {
  Serial.println(msg); 
  logBuffer += msg + " | "; 
}

void uploadLogs() {
  if (logBuffer == "") return; 

  Serial.println(">> Uploading logs...");

  bool localWiFiConnect = false;
  
  // Reconnect if needed
  if (WiFi.status() != WL_CONNECTED) {
      localWiFiConnect = true;
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm); // Low power mode

      preferences.begin("jjy-config", true);
      String ssid = preferences.getString("ssid", WIFI_SSID_DEFAULT);
      String pass = preferences.getString("pass", WIFI_PASS_DEFAULT);
      preferences.end();
      
      if(ssid == "your_wifi") { ssid = WIFI_SSID_DEFAULT; pass = WIFI_PASS_DEFAULT; }
      
      WiFi.begin(ssid.c_str(), pass.c_str());
      int retries = 0;
      while(WiFi.status() != WL_CONNECTED && retries < 20) {
          delay(500); retries++;
      }
  }

  if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      WiFiClientSecure client;
      client.setInsecure(); // Skip cert validation for speed/memory
      
      http.begin(client, G_SCRIPT_URL);
      http.addHeader("Content-Type", "application/json");
      
      String json = "{\"deviceId\":\"" + String(DEVICE_NAME) + "\", \"log\":\"" + logBuffer + "\"}";
      int httpResponseCode = http.POST(json);
      
      if (httpResponseCode > 0) {
          Serial.printf(">> Upload Success: %d\n", httpResponseCode);
          logBuffer = ""; 
      } else {
          Serial.printf(">> Upload Failed: %s\n", http.errorToString(httpResponseCode).c_str());
      }
      http.end();
  }

  if (localWiFiConnect) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
  }
}

// ============================================================
// ROBUST NTP
// ============================================================

void time_sync_notification_cb(struct timeval *tv) {
    sysLog("NTP Callback: Sync OK");
}

void ensureNTPSyncOrReboot() {
  sysLog("Initializing SNTP...");
  esp_netif_sntp_deinit();

  // Primary: Google Hostname
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
  config.sync_cb = time_sync_notification_cb;
  esp_netif_sntp_init(&config);
  
  // Backups
  esp_sntp_setservername(1, "time.nist.gov");
  esp_sntp_setservername(2, "pool.ntp.org");

  Serial.print("Waiting for time");
  esp_err_t err = esp_netif_sntp_sync_wait(30000 / portTICK_PERIOD_MS);

  if (err == ESP_OK) {
    sysLog("Hostname Sync OK");
  } 
  else {
    sysLog("Hostname Fail. Trying IP Fallback...");
    esp_netif_sntp_deinit();
    
    // Fallback: Google IP (Bypasses DNS)
    esp_sntp_config_t config_ip = ESP_NETIF_SNTP_DEFAULT_CONFIG("216.239.35.0"); 
    config_ip.sync_cb = time_sync_notification_cb;
    esp_netif_sntp_init(&config_ip);
    
    err = esp_netif_sntp_sync_wait(20000 / portTICK_PERIOD_MS);
    
    if (err != ESP_OK) {
        sysLog("CRITICAL: NTP Fail. Sleeping 1h.");
        uploadLogs(); 
        esp_sleep_enable_timer_wakeup(60 * 60 * 1000000ULL);
        esp_deep_sleep_start();
    }
  }
}

// ============================================================
// WIFI & UTILS
// ============================================================

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  preferences.begin("jjy-config", false);
  String ssid = preferences.getString("ssid", WIFI_SSID_DEFAULT);
  String pass = preferences.getString("pass", WIFI_PASS_DEFAULT);
  
  if(ssid == "your_wifi") { ssid = WIFI_SSID_DEFAULT; pass = WIFI_PASS_DEFAULT; }

  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("WiFi Attempt %d/3... ", attempt);
    WiFi.disconnect(); delay(100); 
    WiFi.begin(ssid.c_str(), pass.c_str());

    for(int w=0; w<20; w++) { 
      if(WiFi.status() == WL_CONNECTED) { connected = true; break; }
      delay(500);
    }
    if(connected) break;
    Serial.println("Failed.");
  }

  if (!connected) {
    Serial.println("\nLaunching Captive Portal...");
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    if (!wm.startConfigPortal("JJY-SETUP")) {
      sysLog("Portal Timeout. Reboot.");
      uploadLogs();
      ESP.restart();
    }
    preferences.putString("ssid", WiFi.SSID());
    preferences.putString("pass", WiFi.psk());
  }
  preferences.end();
  sysLog("WiFi Connected: " + WiFi.localIP().toString());
}

void enterLightSleep(uint64_t seconds) {
  if (seconds < 1) return;
  digitalWrite(PIN_LED, LOW); delay(50); digitalWrite(PIN_LED, HIGH);
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_light_sleep_start();
}

void enterDaytimeDeepSleep() {
  struct tm target_tm = timeinfo;
  target_tm.tm_hour = WAKEUP_HOUR; 
  target_tm.tm_min = WAKEUP_MIN;   
  target_tm.tm_sec = 0;
  
  time_t now;
  time(&now);
  time_t jst_now = now + GMT_OFFSET_SEC;
  
  struct tm jst_tm;
  localtime_r(&jst_now, &jst_tm);
  jst_tm.tm_hour = WAKEUP_HOUR;
  jst_tm.tm_min = WAKEUP_MIN;
  jst_tm.tm_sec = 0;
  
  time_t target_jst = mktime(&jst_tm);
  if (target_jst <= jst_now) target_jst += 24 * 3600; 
  
  uint64_t secondsToSleep = (uint64_t)(target_jst - jst_now);
  
  sysLog("Deep Sleep: " + String((unsigned long)secondsToSleep) + "s");
  uploadLogs(); 

  esp_sleep_enable_timer_wakeup(secondsToSleep * 1000000ULL);
  esp_deep_sleep_start();
}

void startTransmission() {
  if (jjyTimerHandle) return; 
  if (!ledcAttach(PIN_ANT, JJY_FREQ, PWM_RES)) { ESP.restart(); }
  ledcWrite(PIN_ANT, 0);

  // Microsecond Alignment to the next second boundary
  struct timeval tv;
  gettimeofday(&tv, NULL);
  unsigned long usecToWait = 1000000UL - tv.tv_usec;
  unsigned long startWait = micros();
  while (micros() - startWait < usecToWait) { NOP(); }

  const esp_timer_create_args_t timer_args = { .callback = &onTimer, .name = "jjy" };
  esp_timer_create(&timer_args, &jjyTimerHandle);
  esp_timer_start_periodic(jjyTimerHandle, 100000); 
  sysLog("TX Started");
}

void stopTransmission() {
  if (jjyTimerHandle) {
    esp_timer_stop(jjyTimerHandle);
    esp_timer_delete(jjyTimerHandle);
    jjyTimerHandle = nullptr;
    ledcDetach(PIN_ANT);
    digitalWrite(PIN_ANT, LOW);
    digitalWrite(PIN_LED, HIGH); 
    sysLog("TX Stopped");
  }
}

// ISR - Runs in IRAM (High Priority)
void IRAM_ATTR onTimer(void* arg) {
  if (currentSlot100ms >= 10) currentSlot100ms = 0;
  int duty = 0;
  if (currentSecondCode == CODE_MARKER) {
     if (currentSlot100ms < 2) duty = DUTY_50;
  } else if (currentSecondCode == CODE_HIGH) {
     if (currentSlot100ms < 5) duty = DUTY_50;
  } else if (currentSecondCode == CODE_LOW) {
     if (currentSlot100ms < 8) duty = DUTY_50;
  }
  ledcWrite(PIN_ANT, duty);
  digitalWrite(PIN_LED, (duty > 0)); 
  currentSlot100ms++;
}

// JJY Encoding Logic
int int3bcd(int a) { return (a % 10) + (a / 10 % 10 * 16) + (a / 100 % 10 * 256); }
int parity8(int a) { int pa = a; for(int i = 1; i < 8; i++) pa += a >> i; return pa % 2; }

int calculateJJYCodeForTime(time_t t) {
  struct tm t_enc;
  localtime_r(&t, &t_enc);
  int h = int3bcd(t_enc.tm_hour);
  int m = int3bcd(t_enc.tm_min);
  int y = int3bcd((t_enc.tm_year + 1900) % 100);
  int d = int3bcd(t_enc.tm_yday + 1);
  int w = t_enc.tm_wday;
  int sec = t_enc.tm_sec;

  if (sec == 0 || sec == 9 || sec == 19 || sec == 29 || sec == 39 || sec == 49 || sec == 59) return CODE_MARKER;
  if (sec >= 1 && sec <= 8) {
    if (sec == 4) return CODE_LOW;
    int mask = (sec==1)?0x40:(sec==2)?0x20:(sec==3)?0x10:(sec==5)?0x08:(sec==6)?0x04:(sec==7)?0x02:0x01;
    return (m & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec >= 12 && sec <= 18) {
    if (sec == 14) return CODE_LOW;
    int mask = (sec==12)?0x20:(sec==13)?0x10:(sec==15)?0x08:(sec==16)?0x04:(sec==17)?0x02:0x01;
    return (h & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec >= 22 && sec <= 33) {
    if (sec == 24) return CODE_LOW;
    int mask = (sec==22)?0x200:(sec==23)?0x100:(sec==25)?0x080:(sec==26)?0x040:(sec==27)?0x020:(sec==28)?0x010:(sec==30)?0x008:(sec==31)?0x004:(sec==32)?0x002:0x001;
    return (d & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec == 36) return parity8(h) ? CODE_HIGH : CODE_LOW;
  if (sec == 37) return parity8(m) ? CODE_HIGH : CODE_LOW;
  if (sec >= 41 && sec <= 48) {
    int mask = (sec==41)?0x80:(sec==42)?0x40:(sec==43)?0x20:(sec==44)?0x10:(sec==45)?0x08:(sec==46)?0x04:(sec==47)?0x02:0x01;
    return (y & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec >= 50 && sec <= 52) {
    int mask = (sec==50)?0x04:(sec==51)?0x02:0x01;
    return (w & mask) ? CODE_HIGH : CODE_LOW;
  }
  return CODE_LOW;
}
