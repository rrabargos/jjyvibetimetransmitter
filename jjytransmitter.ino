/*
 * ---------------------------------------------------------------------------
 *  ESP32-C3 JJY Vibe Time Signal Transmitter
 *  
 *  Features:
 *  1. ROBUST NTP: Prioritizes Google (Asia), Fallback to IP if DNS fails.
 *  2. DEEP SLEEP: Sleep from 05:00 to 23:50 to save max power.
 *  3. PRECISION: Forced XTAL during Light Sleep + High-Res Timer (esp_timer).
 *  4. WIFI: Retries 3 times before Captive Portal.
 *  ---------------------------------------------------------------------------
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <esp32-hal-ledc.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <esp_timer.h>

// ================= CONFIGURATION =================

const char* WIFI_SSID_DEFAULT = "your_wifi";
const char* WIFI_PASS_DEFAULT = "your_password";

// === HARDWARE PINS ===
const int PIN_LED = 8;  // Onboard LED
const int PIN_ANT = 1;  // Antenna Output Pin

// TIME CORRECTION
const int TIME_SHIFT_SEC = 0;

// Settings
const int JJY_FREQ = 40000;
const long GMT_OFFSET_SEC = 9 * 3600;  // JST (UTC+9)

// == SCHEDULE SETTINGS ==
const int BOOT_TX_MINUTES = 10;   // Initial test transmission duration
const int NIGHT_TX_MINUTES = 15;  // Minutes to transmit every hour at night
const int NIGHT_START_HOUR = 0;
const int NIGHT_END_HOUR = 5;
const int WAKEUP_HOUR = 23;  // Wake up at 23:50 to sync
const int WAKEUP_MIN = 50;

// =================================================

enum SystemState { STATE_BOOT_SYNC,
                   STATE_RUNNING };
SystemState currentState = STATE_BOOT_SYNC;

struct tm timeinfo;
Preferences preferences;

unsigned long bootTime = 0;
esp_timer_handle_t jjyTimerHandle = nullptr;

// PWM 8-bit
const int PWM_RES = 8;
const int DUTY_50 = 127;

// JJY Code
const int CODE_MARKER = 2;
const int CODE_LOW = 0;
const int CODE_HIGH = 1;

volatile int currentSecondCode = -1;
volatile int currentSlot100ms = 0;

// Prototypes
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

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== JJY Signal Transmitter by Rigor Abargos ===");

  // 1. Force XTAL ON (Prevents drift in sleep)
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_ON);

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_ANT, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  digitalWrite(PIN_ANT, LOW);

  bootTime = millis();

  // 2. Connect WiFi
  setupWiFi();

  // 3. NTP Sync (The Fixed Logic)
  currentState = STATE_BOOT_SYNC;
  ensureNTPSyncOrReboot();

  // 4. Cleanup WiFi to save power/noise
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  currentState = STATE_RUNNING;

  // Start immediately if within boot window
  if (millis() - bootTime < (BOOT_TX_MINUTES * 60 * 1000UL)) {
    startTransmission();
  }
}

// ============================================================
// MAIN LOOP & SCHEDULER
// ============================================================
void loop() {
  // Update time
  time_t now;
  time(&now);
  localtime_r(&now, &timeinfo);

  // Pre-calculate Code for the *current* second (for the ISR)
  static time_t lastSec = 0;
  if (now != lastSec) {
    lastSec = now;
    currentSecondCode = calculateJJYCodeForTime(now + TIME_SHIFT_SEC);
  }

  // 1. BOOT PRIORITY: Always transmit for first X minutes after reset
  if (millis() - bootTime < (BOOT_TX_MINUTES * 60 * 1000UL)) {
    if (!jjyTimerHandle) startTransmission();
    delay(100);
    return;
  }

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;

  // 2. NIGHT WINDOW (00:00 - 05:00)
  if (h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR) {
    // Transmit Window (00m - 15m)
    if (m < NIGHT_TX_MINUTES) {
      if (!jjyTimerHandle) startTransmission();
      delay(100);
      return;
    }
    // Idle Window (15m - 59m)
    else {
      if (jjyTimerHandle) stopTransmission();
      int secRemaining = (59 - m) * 60 + (60 - timeinfo.tm_sec);
      Serial.printf("Night Idle. Sleeping for %d sec until next hour...\n", secRemaining);
      enterLightSleep(secRemaining);
      return;
    }
  }

  // 3. PRE-MIDNIGHT BUFFER (23:50 - 00:00)
  if (h == 23 && m >= WAKEUP_MIN) {
    if (jjyTimerHandle) stopTransmission();
    int secRemaining = (59 - m) * 60 + (60 - timeinfo.tm_sec);
    Serial.printf("Pre-Midnight. Holding accurate time for %d sec...\n", secRemaining);
    enterLightSleep(secRemaining);
    return;
  }

  // 4. DAYTIME (05:00 - 23:50) -> DEEP SLEEP
  if (jjyTimerHandle) stopTransmission();
  enterDaytimeDeepSleep();
}

// ============================================================
// SLEEP HELPERS
// ============================================================

void enterLightSleep(uint64_t seconds) {
  if (seconds < 1) return;
  digitalWrite(PIN_LED, LOW);
  delay(50);
  digitalWrite(PIN_LED, HIGH);
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_light_sleep_start();
}

void enterDaytimeDeepSleep() {
  struct tm target_tm = timeinfo;
  target_tm.tm_hour = WAKEUP_HOUR;  // 23
  target_tm.tm_min = WAKEUP_MIN;    // 50
  target_tm.tm_sec = 0;

  time_t now;
  time(&now);
  time_t target = mktime(&target_tm);

  if (target <= now) target += 24 * 3600;

  uint64_t secondsToSleep = (uint64_t)(target - now);
  Serial.printf("Entering Deep Sleep. Waking up in %llu seconds at 23:50.\n", secondsToSleep);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(secondsToSleep * 1000000ULL);
  esp_deep_sleep_start();
}

// ============================================================
// WIFI LOGIC
// ============================================================

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  preferences.begin("jjy-config", false);

  String ssid = preferences.getString("ssid", WIFI_SSID_DEFAULT);
  String pass = preferences.getString("pass", WIFI_PASS_DEFAULT);

  if (ssid == "your_wifi") {
    ssid = WIFI_SSID_DEFAULT;
    pass = WIFI_PASS_DEFAULT;
  }

  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("WiFi Attempt %d/3... ", attempt);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());

    for (int w = 0; w < 20; w++) {  // 10 sec timeout
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      delay(500);
    }
    if (connected) break;
    Serial.println("Failed.");
  }

  if (!connected) {
    Serial.println("\nLaunching Captive Portal (JJY-SETUP)...");
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    if (!wm.startConfigPortal("JJY-SETUP")) {
      Serial.println("Portal Timeout. Rebooting.");
      ESP.restart();
    }
    preferences.putString("ssid", WiFi.SSID());
    preferences.putString("pass", WiFi.psk());
  }

  preferences.end();
  Serial.println("\nWiFi Connected.");
}

// ============================================================
// ROBUST NTP SYNC (Google Priority + IP Fallback)
// ============================================================

void ensureNTPSyncOrReboot() {
  Serial.println("Initializing SNTP...");

  // 1. Clear old state
  esp_sntp_stop();
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

  // 2. Primary Configuration (Google Anycast First)
  // time.google.com -> 216.239.35.0 (Anycast)
  // time.nist.gov   -> Backup
  // pool.ntp.org    -> Last resort
  configTime(GMT_OFFSET_SEC, 0, "time.google.com", "time.nist.gov", "pool.ntp.org");

  Serial.print("Waiting for Time Sync");

  int retry = 0;
  bool syncSuccess = false;

  // Wait loop (Max 20 seconds)
  while (retry < 40) {
    sntp_sync_status_t status = sntp_get_sync_status();

    // Fallback: Check if system time updated even if status lags
    time_t now;
    time(&now);

    if (status == SNTP_SYNC_STATUS_COMPLETED) {
      syncSuccess = true;
      Serial.println("\nStatus: COMPLETED.");
      break;
    } else if (now > 1600000000L) {
      syncSuccess = true;
      Serial.println("\nStatus: LAG, but Time Valid.");
      break;
    }

    delay(500);
    Serial.print(".");
    retry++;
  }

  // 3. Fallback: Direct IP Mode (If DNS is broken)
  if (!syncSuccess) {
    Serial.println("\nDNS failed. Trying Direct IP...");
    esp_sntp_stop();
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

    // Google Public NTP IPs
    configTime(GMT_OFFSET_SEC, 0, "216.239.35.0", "216.239.35.4", "pool.ntp.org");

    retry = 0;
    while (retry < 30) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        syncSuccess = true;
        break;
      }
      delay(500);
      Serial.print("!");
      retry++;
    }
  }

  if (syncSuccess) {
    struct tm t;
    getLocalTime(&t);
    Serial.print("\nSynced: ");
    Serial.println(&t, "%A, %B %d %Y %H:%M:%S");
  }  else {
    Serial.println("\nCRITICAL: NTP Sync Failed.");
    // Sleep for 60 minutes then try again.
    Serial.println("Internet down? Sleeping for 1 hour to save battery.");
    esp_sleep_enable_timer_wakeup(60 * 60 * 1000000ULL);
    esp_deep_sleep_start();
    // (The device essentially reboots on wake anyway)
  }
}

// ============================================================
// SIGNAL GENERATION (High Res Timer)
// ============================================================

void startTransmission() {
  if (jjyTimerHandle) return;

  Serial.print("Starting TX... Aligning...");

  if (!ledcAttach(PIN_ANT, JJY_FREQ, PWM_RES)) { ESP.restart(); }
  ledcWrite(PIN_ANT, 0);

  // Microsecond Alignment
  struct timeval tv;
  gettimeofday(&tv, NULL);
  unsigned long usecToWait = 1000000UL - tv.tv_usec;
  unsigned long startWait = micros();
  while (micros() - startWait < usecToWait) { NOP(); }

  Serial.println(" NOW!");

  const esp_timer_create_args_t timer_args = { .callback = &onTimer, .name = "jjy" };
  esp_timer_create(&timer_args, &jjyTimerHandle);
  esp_timer_start_periodic(jjyTimerHandle, 100000);  // 100ms
}

void stopTransmission() {
  if (jjyTimerHandle) {
    esp_timer_stop(jjyTimerHandle);
    esp_timer_delete(jjyTimerHandle);
    jjyTimerHandle = nullptr;
    ledcDetach(PIN_ANT);
    digitalWrite(PIN_ANT, LOW);
    digitalWrite(PIN_LED, HIGH);
    Serial.println("TX Stopped.");
  }
}

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

int int3bcd(int a) {
  return (a % 10) + (a / 10 % 10 * 16) + (a / 100 % 10 * 256);
}
int parity8(int a) {
  int pa = a;
  for (int i = 1; i < 8; i++) pa += a >> i;
  return pa % 2;
}

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
    int mask = (sec == 1) ? 0x40 : (sec == 2) ? 0x20
                                 : (sec == 3) ? 0x10
                                 : (sec == 5) ? 0x08
                                 : (sec == 6) ? 0x04
                                 : (sec == 7) ? 0x02
                                              : 0x01;
    return (m & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec >= 12 && sec <= 18) {
    if (sec == 14) return CODE_LOW;
    int mask = (sec == 12) ? 0x20 : (sec == 13) ? 0x10
                                  : (sec == 15) ? 0x08
                                  : (sec == 16) ? 0x04
                                  : (sec == 17) ? 0x02
                                                : 0x01;
    return (h & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec >= 22 && sec <= 33) {
    if (sec == 24) return CODE_LOW;
    int mask = (sec == 22) ? 0x200 : (sec == 23) ? 0x100
                                   : (sec == 25) ? 0x080
                                   : (sec == 26) ? 0x040
                                   : (sec == 27) ? 0x020
                                   : (sec == 28) ? 0x010
                                   : (sec == 30) ? 0x008
                                   : (sec == 31) ? 0x004
                                   : (sec == 32) ? 0x002
                                                 : 0x001;
    return (d & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec == 36) return parity8(h) ? CODE_HIGH : CODE_LOW;
  if (sec == 37) return parity8(m) ? CODE_HIGH : CODE_LOW;
  if (sec >= 41 && sec <= 48) {
    int mask = (sec == 41) ? 0x80 : (sec == 42) ? 0x40
                                  : (sec == 43) ? 0x20
                                  : (sec == 44) ? 0x10
                                  : (sec == 45) ? 0x08
                                  : (sec == 46) ? 0x04
                                  : (sec == 47) ? 0x02
                                                : 0x01;
    return (y & mask) ? CODE_HIGH : CODE_LOW;
  }
  if (sec >= 50 && sec <= 52) {
    int mask = (sec == 50) ? 0x04 : (sec == 51) ? 0x02
                                                : 0x01;
    return (w & mask) ? CODE_HIGH : CODE_LOW;
  }
  return CODE_LOW;
}
