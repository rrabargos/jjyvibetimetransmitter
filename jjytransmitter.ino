/*
 * ---------------------------------------------------------------------------
 *  ESP32-C3 JJY Vibe Time Signal Transmitter
 *  Author: Rigor Abargos
 *  Description:
 *      Generates a 40 kHz JJY-compatible time signal using PWM on an ESP32-C3.
 *      Synchronizes time via NTP (UTC+9), encodes the full JJY minute frame,
 *      and emits amplitude-modulated carrier pulses aligned to the start of
 *      each second. Includes night-schedule transmission, drift correction,
 *      and low-power idle mode.
 *
 *  Hardware:
 *      - ESP32-C3 Dev Board
 *      - PIN_TRX (default: 8) used for antenna output + status LED
 *
 *  Main Components:
 *      - NTP sync and JST conversion
 *      - Microsecond-aligned TX start
 *      - 100 ms JJY frame interrupt via Ticker
 *      - PWM carrier generation using LEDC
 *      - Automatic Wi-Fi shutdown after sync
 *      - Light-sleep idle loop outside TX schedule
 *
 *  Safety:
 *      For near-field, low-power laboratory use only.
 *      Unauthorized long-range transmission may be illegal.
 *
 *  License:
 *      MIT License
 * ---------------------------------------------------------------------------
 */

#include <WiFi.h>
#include <Ticker.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>      
#include <esp32-hal-ledc.h> 

// ================= CONFIGURATION =================
const char* WIFI_SSID = "your_wifi";
const char* WIFI_PASS = "your_password";

// SHARED PIN (LED + ANTENNA)
const int PIN_TRX = 8; 

// TIME CORRECTION
// If your clock is consistently 1 second slow, change this to 1.
// If your clock is 1 second fast, change this to -1.
const int TIME_SHIFT_SEC = 0; 

// Settings
const int JJY_FREQ = 40000; 
const long GMT_OFFSET_SEC = 9 * 3600; // JST (UTC+9)
const int FORCE_TX_MINUTES = 5;       // Boot TX duration

// =================================================

enum SystemState { STATE_BOOT, STATE_SYNCING, STATE_TRANSMITTING, STATE_LOCKED_IDLE, STATE_ERROR };
volatile SystemState currentState = STATE_BOOT;

struct tm timeinfo;
struct timeval tv_now;
Ticker tickJjy; 

unsigned long bootTime = 0;
bool wifiOn = false;

// PWM 8-bit
const int PWM_RES = 8;
const int DUTY_50 = 127;
const int DUTY_OFF = 0;
const int CODE_MARKER = 0xFF;

// Prototypes
void jjyHandler();
void startTransmission();
void stopTransmission();
void carrierOn();
void carrierOff();
int getJJYCode();
int int3bcd(int a);
int parity8(int a);

void setup() {
  Serial.begin(115200);
  delay(2000); 
  Serial.println("\n\n=== JJY Signal Transmitter by Rigor Abargos ===");

  pinMode(PIN_TRX, OUTPUT);
  digitalWrite(PIN_TRX, HIGH); 

  bootTime = millis();
  currentState = STATE_SYNCING; 
}

void loop() {
  // ============================================================
  // STATE 1: SYNCING
  // ============================================================
  if (currentState == STATE_SYNCING) {
    if (!wifiOn) {
      Serial.print("Connecting to Wi-Fi: "); Serial.println(WIFI_SSID);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifiOn = true;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected. NTP...");
      configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org");
      
      if (getLocalTime(&timeinfo)) {
        Serial.println(" Locked!");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiOn = false;
        startTransmission(); // Start aligned TX
      }
    }
    
    if (millis() - bootTime > 20000 && !wifiOn) {
      Serial.println("Sync Failed. Rebooting.");
      ESP.restart();
    }
    delay(100); 
    return;
  }

  // ============================================================
  // STATE 2: TRANSMITTING
  // ============================================================
  if (currentState == STATE_TRANSMITTING) {
    getLocalTime(&timeinfo);

    bool bootRule  = (millis() - bootTime < (FORCE_TX_MINUTES * 60 * 1000UL));
    bool nightRule = (timeinfo.tm_hour >= 0 && timeinfo.tm_hour < 5);

    if (!bootRule && !nightRule) {
      Serial.println("Schedule Ended. Sleep Mode.");
      stopTransmission(); 
    }
    delay(100); 
    return;
  }

  // ============================================================
  // STATE 3: IDLE (Light Sleep)
  // ============================================================
  if (currentState == STATE_LOCKED_IDLE) {
    // Heartbeat
    digitalWrite(PIN_TRX, LOW);  
    delay(50); 
    digitalWrite(PIN_TRX, HIGH); 

    // Check Schedule
    getLocalTime(&timeinfo);
    if (timeinfo.tm_hour >= 0 && timeinfo.tm_hour < 5) {
       startTransmission();
       return; 
    }

    // Sleep 15s
    esp_sleep_enable_timer_wakeup(15 * 1000000ULL); 
    esp_light_sleep_start();
  }
}

// ============================================================
// HARDWARE CONTROL (With Alignment Fix)
// ============================================================

void startTransmission() {
  Serial.print("Aligning signal to next second...");
  
  // 1. PHASE ALIGNMENT
  // We wait until the microseconds roll over to 0 (Start of next second)
  // This ensures our 100ms interrupts hit exactly at .000, .100, .200...
  // preventing "jitter" which confuses clocks.
  struct timeval tv;
  gettimeofday(&tv, NULL);
  
  // Calculate how many ms to wait until the next full second
  unsigned long waitMs = 1000 - (tv.tv_usec / 1000);
  
  // Wait (blocking is fine here, we are just starting)
  if (waitMs > 0 && waitMs < 1000) delay(waitMs);
  
  Serial.println(" NOW!");
  currentState = STATE_TRANSMITTING;
  
  // 2. Start Hardware
  if (!ledcAttach(PIN_TRX, JJY_FREQ, PWM_RES)) {
    Serial.println("PWM Fail! Rebooting.");
    ESP.restart();
  }
  
  // 3. Start Ticker immediately (now aligned to ~0ms)
  tickJjy.attach_ms(100, jjyHandler);
}

void stopTransmission() {
  Serial.println("STOPPING SIGNAL.");
  tickJjy.detach();
  ledcDetach(PIN_TRX);
  pinMode(PIN_TRX, OUTPUT);
  digitalWrite(PIN_TRX, HIGH); 
  currentState = STATE_LOCKED_IDLE;
}

// ============================================================
// SIGNAL LOGIC
// ============================================================

void jjyHandler() {
  gettimeofday(&tv_now, NULL);
  // Slot 0-9 (representing 0ms to 900ms)
  long slot = tv_now.tv_usec / 100000L;

  switch(slot) {
    case 0: carrierOn(); break;
    case 2: if (getJJYCode() == CODE_MARKER) carrierOff(); break;
    case 5: {
      int c = getJJYCode();
      if (c != 0 && c != CODE_MARKER) carrierOff();
    } break;
    case 8: carrierOff(); break;
  }
}

void carrierOn() { ledcWrite(PIN_TRX, DUTY_50); }
void carrierOff() { ledcWrite(PIN_TRX, DUTY_OFF); }

// ============================================================
// JJY ENCODING 
// ============================================================

int int3bcd(int a) { return (a % 10) + (a / 10 % 10 * 16) + (a / 100 % 10 * 256); }
int parity8(int a) { int pa = a; for(int i = 1; i < 8; i++) pa += a >> i; return pa % 2; }

int getJJYCode() {
  // 1. Get Current Time
  time_t now;
  time(&now);
  
  // 2. APPLY USER OFFSET
  now += TIME_SHIFT_SEC;

  // 3. Convert back to struct tm
  struct tm t_enc;
  localtime_r(&now, &t_enc);

  int h = int3bcd(t_enc.tm_hour);
  int m = int3bcd(t_enc.tm_min);
  int y = int3bcd((t_enc.tm_year + 1900) % 100);
  int d = int3bcd(t_enc.tm_yday + 1);
  int w = t_enc.tm_wday;
  int ph = parity8(h);
  int pm = parity8(m);
  int sec = t_enc.tm_sec;

  if (sec == 0 || sec == 9 || sec == 19 || sec == 29 || sec == 39 || sec == 49 || sec == 59) return CODE_MARKER;

  if (sec >= 1 && sec <= 8) {
    if (sec == 4) return 0;
    int mask = (sec==1)?0x40:(sec==2)?0x20:(sec==3)?0x10:(sec==5)?0x08:(sec==6)?0x04:(sec==7)?0x02:0x01;
    return (m & mask) ? 1 : 0;
  }
  if (sec >= 12 && sec <= 18) {
    if (sec == 14) return 0;
    int mask = (sec==12)?0x20:(sec==13)?0x10:(sec==15)?0x08:(sec==16)?0x04:(sec==17)?0x02:0x01;
    return (h & mask) ? 1 : 0;
  }
  if (sec >= 22 && sec <= 33) {
    if (sec == 24) return 0;
    int mask = (sec==22)?0x200:(sec==23)?0x100:(sec==25)?0x080:(sec==26)?0x040:(sec==27)?0x020:(sec==28)?0x010:(sec==30)?0x008:(sec==31)?0x004:(sec==32)?0x002:0x001;
    return (d & mask) ? 1 : 0;
  }
  if (sec == 36) return ph;
  if (sec == 37) return pm;
  if (sec >= 41 && sec <= 48) {
    int mask = (sec==41)?0x80:(sec==42)?0x40:(sec==43)?0x20:(sec==44)?0x10:(sec==45)?0x08:(sec==46)?0x04:(sec==47)?0x02:0x01;
    return (y & mask) ? 1 : 0;
  }
  if (sec >= 50 && sec <= 52) {
    int mask = (sec==50)?0x04:(sec==51)?0x02:0x01;
    return (w & mask) ? 1 : 0;
  }
  return 0;
}
