/*
 * ---------------------------------------------------------------------------
 *  ESP32-C3 JJY Time Signal Transmitter by Rigor Abargos
 *  
 *  Description:
 *      Emulates the Japanese JJY (40kHz) radio time signal to synchronize 
 *      radio-controlled clocks. Designed for unstable network environments 
 *      and long battery life.
 *
 *  Core Strategies:
 *  1. STRICT PRECISION:
 *     - Connects and Syncs NTP before *every* hourly broadcast.
 *     - If NTP Sync fails, the device goes to Sleep (No broadcast of wrong time).
 *     - Uses "Active Wait" (CPU Spin) for the last few minutes before the hour
 *       to guarantee the broadcast starts exactly at 00:00:00.00.
 *
 *  2. DEEP SLEEP / BATTERY SAVING:
 *     - Daytime (06:00 - 23:55): Deep Sleep (~5µA).
 *     - Night Idle (Min 15 - 55): Deep Sleep between hourly broadcasts.
 *     - WiFi is only active for ~60 seconds per hour.
 *
 *  3. BOOT TEST MODE:
 *     - On power-up/reset, transmits immediately for 10 minutes (if NTP syncs)
 *       to allow instant testing without waiting for midnight.
 *
 *  Schedule (JST):
 *      - 23:55 : Wake Up, Sync, Prep for Midnight.
 *      - 00:00 - 06:00 : Hourly Broadcasts (Minutes 00-15).
 * ---------------------------------------------------------------------------
 */

#include <WiFi.h>
#include <WiFiManager.h> 
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>      
#include <esp32-hal-ledc.h> 
#include <Preferences.h>
#include <esp_sntp.h>
#include <esp_netif_sntp.h> 
#include <esp_timer.h>

// ================= CONFIGURATION =================

const char* WIFI_SSID_DEFAULT = "your_wifi";
const char* WIFI_PASS_DEFAULT = "your_password";

// Hardware Pins
const int PIN_LED = 8;    
const int PIN_ANT = 1;    

// Signal Settings
const long GMT_OFFSET_SEC = 9 * 3600; // JST (UTC+9)
const int JJY_FREQ = 40000; 
const int TIME_SHIFT_SEC = 0; 

// Schedule Settings
const int BOOT_TX_MINUTES = 10;      // Initial test duration
const int NIGHT_TX_MINUTES = 15;     // Duration of hourly broadcast
const int NIGHT_START_HOUR = 0;      
const int NIGHT_END_HOUR = 6;        // Ends at 06:00
const int WAKEUP_HOUR = 23;          
const int WAKEUP_MIN = 55;           // Prep time before midnight

// =================================================

struct tm timeinfo;
Preferences preferences;

esp_timer_handle_t jjyTimerHandle = nullptr;

const int PWM_RES = 8;
const int DUTY_50 = 127;

const int CODE_MARKER = 2; 
const int CODE_LOW = 0;
const int CODE_HIGH = 1;

volatile int currentSecondCode = -1; 
volatile int currentSlot100ms = 0;

// Prototypes
void setupWiFi();
bool robustNTPSync(); 
void startTransmission();
void stopTransmission();
void enterDeepSleepUntil(int targetHour, int targetMin);
void waitForTopOfHour();
void IRAM_ATTR onTimer(void* arg); 
int calculateJJYCodeForTime(time_t t);
int int3bcd(int a);
int parity8(int a);
void time_sync_notification_cb(struct timeval *tv);

// ============================================================
// SETUP (Runs on every Wake/Boot)
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500); 
  Serial.println("\n\n=== JJY Transmitter Started ===");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_ANT, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  digitalWrite(PIN_ANT, LOW);

  // 1. GATEKEEPER: Sync Time
  setupWiFi();
  bool timeLocked = robustNTPSync();
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // Strict Rule: No Sync = No Transmission
  if (!timeLocked) {
      Serial.println("CRITICAL: NTP Failed. Aborting to prevent wrong time.");
      Serial.println("Retrying in 1 hour...");
      esp_sleep_enable_timer_wakeup(3600 * 1000000ULL);
      esp_deep_sleep_start();
      return; 
  }

  // 2. Get Accurate Time
  time_t now;
  time(&now);
  time_t jst_now = now + GMT_OFFSET_SEC;
  localtime_r(&jst_now, &timeinfo);
  
  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;

  Serial.printf("Time Locked (JST): %02d:%02d:%02d\n", h, m, timeinfo.tm_sec);

  // 3. CHECK: Boot Test Mode (First 10 mins of uptime)
  if (millis() < (BOOT_TX_MINUTES * 60 * 1000UL)) {
      Serial.println("Mode: Boot Test (Immediate TX)");
      startTransmission();
      return; 
  }

  // 4. CHECK: Daytime Schedule (06:00 - 23:54)
  if (h >= NIGHT_END_HOUR && !(h == 23 && m >= 55)) {
      Serial.println("Mode: Daytime. Sleeping until 23:55.");
      enterDeepSleepUntil(WAKEUP_HOUR, WAKEUP_MIN);
      return;
  }

  // 5. CHECK: Pre-Hour Sync/Wait (Minute 55-59)
  if (m >= 55) {
      Serial.println("Mode: Pre-Hour. Active Wait for 00:00:00...");
      waitForTopOfHour();
      // Proceed to Loop for transmission
  }
  
  // 6. CHECK: Broadcast Window (Minute 00-15)
  if (m < NIGHT_TX_MINUTES) {
      Serial.println("Mode: Broadcast Window. Transmitting...");
      startTransmission();
      return; 
  }
  
  // 7. CHECK: Night Idle (Minute 15-54)
  if (m >= NIGHT_TX_MINUTES && m < 55) {
      Serial.println("Mode: Night Idle. Sleeping until Minute 59.");
      // Wake at 59 to allow 1 min for Sync
      enterDeepSleepUntil(h, 59);
      return;
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

  // 1. Update Signal Code for ISR
  static time_t lastSec = 0;
  if (now != lastSec) {
    lastSec = now;
    currentSecondCode = calculateJJYCodeForTime(jst_now + TIME_SHIFT_SEC);
  }

  bool isBootTest = (millis() < (BOOT_TX_MINUTES * 60 * 1000UL));

  if (!isBootTest) {
      // End of Broadcast Window (Minute 15)
      if (timeinfo.tm_min >= NIGHT_TX_MINUTES) {
          Serial.println("Broadcast Finished. Sleeping.");
          stopTransmission();
          enterDeepSleepUntil(timeinfo.tm_hour, 59);
      }
      
      // End of Night Schedule (Hour 06)
      if (timeinfo.tm_hour >= NIGHT_END_HOUR) {
          Serial.println("Night Schedule End. Sleeping until 23:55.");
          stopTransmission();
          enterDeepSleepUntil(WAKEUP_HOUR, WAKEUP_MIN);
      }
  }
}

// ============================================================
// ACTIVE WAIT (Precision Logic)
// ============================================================
void waitForTopOfHour() {
  // spin-wait to catch the exact second rollover
  while(true) {
    time_t now;
    time(&now);
    time_t jst_now = now + GMT_OFFSET_SEC;
    struct tm t;
    localtime_r(&jst_now, &t);

    if (t.tm_min == 0 && t.tm_sec == 0) return; // Exact Match
    if (t.tm_min == 0 && t.tm_sec > 0) return;  // Late Entry Catch
    if (t.tm_min > 0 && t.tm_min < 10) return;  // Overflow Catch

    // Heartbeat
    if (t.tm_sec % 5 == 0) digitalWrite(PIN_LED, !digitalRead(PIN_LED)); 
    delay(20); 
  }
}

// ============================================================
// SLEEP LOGIC
// ============================================================
void enterDeepSleepUntil(int targetHour, int targetMin) {
  time_t now;
  time(&now);
  time_t jst_now = now + GMT_OFFSET_SEC;
  
  struct tm target_tm;
  localtime_r(&jst_now, &target_tm);
  target_tm.tm_hour = targetHour;
  target_tm.tm_min = targetMin;
  target_tm.tm_sec = 0;
  
  time_t target_time = mktime(&target_tm);
  
  // Logic to handle day/hour rollover
  if (target_time <= jst_now) {
      if (targetHour == WAKEUP_HOUR) {
         target_time += 24 * 3600; // Next Day
      } else {
         target_time += 3600; // Next Hour
      }
  }

  uint64_t secondsToSleep = (uint64_t)(target_time - jst_now);
  
  // Safety Clamps
  if (secondsToSleep < 5) secondsToSleep = 5;
  if (secondsToSleep > 87000) secondsToSleep = 60; 

  Serial.printf("Deep Sleep: %llu seconds.\n", secondsToSleep);
  Serial.flush();
  
  esp_sleep_enable_timer_wakeup(secondsToSleep * 1000000ULL);
  esp_deep_sleep_start();
}

// ============================================================
// NTP & WIFI (Robust + IP Fallback)
// ============================================================

void time_sync_notification_cb(struct timeval *tv) {}

bool robustNTPSync() {
  Serial.println("Syncing NTP (60s Max)...");
  esp_netif_sntp_deinit();

  // Phase 1: Try Hostnames (Google -> NIST -> Pool)
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
  config.sync_cb = time_sync_notification_cb;
  esp_netif_sntp_init(&config);
  
  esp_sntp_setservername(1, "time.nist.gov");
  esp_sntp_setservername(2, "pool.ntp.org");

  esp_err_t err = esp_netif_sntp_sync_wait(35000 / portTICK_PERIOD_MS);

  // Phase 2: Try Direct IP (DNS Bypass)
  if (err != ESP_OK) {
    Serial.println("Hostname Timeout. Trying Direct IP...");
    esp_netif_sntp_deinit();
    esp_sntp_config_t config_ip = ESP_NETIF_SNTP_DEFAULT_CONFIG("216.239.35.0");
    esp_netif_sntp_init(&config_ip);
    err = esp_netif_sntp_sync_wait(25000 / portTICK_PERIOD_MS);
  }

  if (err == ESP_OK) {
    Serial.println("Time Locked.");
    return true;
  } 

  Serial.println("NTP Failed!");
  return false; 
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  preferences.begin("jjy-config", false);
  
  String ssid = preferences.getString("ssid", WIFI_SSID_DEFAULT);
  String pass = preferences.getString("pass", WIFI_PASS_DEFAULT);
  if(ssid == "your_wifi") { ssid = WIFI_SSID_DEFAULT; pass = WIFI_PASS_DEFAULT; }

  WiFi.begin(ssid.c_str(), pass.c_str());
  
  // Connection timeout: ~15 seconds
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { 
    delay(500);
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
     Serial.println("WiFi Failed.");
  }
  preferences.end();
}

// ============================================================
// SIGNAL GENERATION (Hardware Timer)
// ============================================================

void startTransmission() {
  if (jjyTimerHandle) return; 
  if (!ledcAttach(PIN_ANT, JJY_FREQ, PWM_RES)) { ESP.restart(); }
  ledcWrite(PIN_ANT, 0);

  // Align to microsecond
  struct timeval tv;
  gettimeofday(&tv, NULL);
  unsigned long usecToWait = 1000000UL - tv.tv_usec;
  unsigned long startWait = micros();
  while (micros() - startWait < usecToWait) { NOP(); }

  const esp_timer_create_args_t timer_args = { .callback = &onTimer, .name = "jjy" };
  esp_timer_create(&timer_args, &jjyTimerHandle);
  esp_timer_start_periodic(jjyTimerHandle, 100000); 
}

void stopTransmission() {
  if (jjyTimerHandle) {
    esp_timer_stop(jjyTimerHandle);
    esp_timer_delete(jjyTimerHandle);
    jjyTimerHandle = nullptr;
    ledcDetach(PIN_ANT);
    digitalWrite(PIN_ANT, LOW);
    digitalWrite(PIN_LED, HIGH); 
  }
}

// ISR - Runs in IRAM (No Flash Access)
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

// ============================================================
// JJY UTILS
// ============================================================

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
