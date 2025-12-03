/*
 * ---------------------------------------------------------------------------
 *  ESP32-C3 JJY Vibe Time Signal Transmitter
 *  Author: Rigor Abargos
 *
 *  Description:
 *      Generates a 40 kHz JJY-compatible time signal using PWM on an ESP32-C3.
 *      Synchronizes time via NTP (UTC+9), encodes the full JJY minute frame,
 *      and emits amplitude-modulated carrier pulses aligned to the start of
 *      each second. Implements drift correction and microsecond timing.
 *
 *      Power-optimized transmission modes:
 *          - 10-minute transmission on boot
 *          - 15-minute transmission every hour between 00:00–05:00
 *          - Light-sleep idle behavior outside transmission windows
 *
 *      Configuration via WiFiManager captive portal with automatic Wi-Fi shutdown
 *      after synchronization.
 *
 *  Hardware:
 *      - ESP32-C3 Dev Board
 *      - Separate pins for antenna output and status LED
 *
 *  Main Components:
 *      - WiFiManager captive setup
 *      - NTP synchronization and JST conversion
 *      - Microsecond-aligned TX start
 *      - 100 ms JJY frame generation (Ticker)
 *      - PWM carrier generation via LEDC (40 kHz AM)
 *      - Low-power operation modes
 *
 *  Safety:
 *      Intended for short-range, low-power laboratory use only.
 *      Long-range or unauthorized transmission may violate radio laws.
 *
 *  License:
 *      MIT License
 * ---------------------------------------------------------------------------
 */

#include <WiFi.h>
#include <WiFiManager.h> 
#include <Ticker.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>      
#include <esp32-hal-ledc.h> 
#include <Preferences.h>

// ================= CONFIGURATION =================

// Default/Fallback Credentials
const char* WIFI_SSID_DEFAULT = "your_wifi";
const char* WIFI_PASS_DEFAULT = "your_password";

// === HARDWARE PINS ===
const int PIN_LED = 8;    // Onboard LED (Status Indicator)
const int PIN_ANT = 1;    // [CHANGE THIS] New Antenna Output Pin

// TIME CORRECTION
const int TIME_SHIFT_SEC = 0; 

// Settings
const int JJY_FREQ = 40000; 
const long GMT_OFFSET_SEC = 9 * 3600; // JST (UTC+9)

// == SCHEDULE SETTINGS ==
const int BOOT_TX_MINUTES = 10;      // Transmit for 10 mins after power on
const int NIGHT_TX_MINUTES = 15;     // Transmit for first 15 mins of the hour...
const int NIGHT_START_HOUR = 0;      // ...starting at 00:00
const int NIGHT_END_HOUR = 5;        // ...ending at 05:00

// =================================================

enum SystemState { STATE_BOOT, STATE_SYNCING, STATE_TRANSMITTING, STATE_LOCKED_IDLE };
volatile SystemState currentState = STATE_BOOT;

struct tm timeinfo;
struct timeval tv_now;
Ticker tickJjy; 
Preferences preferences;

unsigned long bootTime = 0;

// PWM 8-bit
const int PWM_RES = 8;
const int DUTY_50 = 127;
const int DUTY_OFF = 0;
const int CODE_MARKER = 0xFF;

// Prototypes
void setupWiFi();
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

  // Initialize Split Pins
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_ANT, OUTPUT);
  
  digitalWrite(PIN_LED, HIGH); // LED ON during setup
  digitalWrite(PIN_ANT, LOW);  // ANT OFF during setup

  bootTime = millis();
  
  // Handle WiFi Connection (3 Retries -> Captive Portal)
  setupWiFi();

  // If we reach here, WiFi is connected
  currentState = STATE_SYNCING; 
}

void loop() {
  // ============================================================
  // STATE 1: SYNCING (Fetch Time)
  // ============================================================
  if (currentState == STATE_SYNCING) {
    Serial.print("Connected. Fetching NTP...");
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org");
    
    // Wait for time to set
    if (getLocalTime(&timeinfo)) {
      Serial.println(" Locked!");
      
      // DISCONNECT WIFI TO REDUCE NOISE
      WiFi.mode(WIFI_OFF);
      
      startTransmission(); // Start aligned TX
    } else {
      Serial.println("NTP Failed. Retrying...");
      delay(1000);
    }
    return;
  }

  // ============================================================
  // STATE 2: TRANSMITTING
  // ============================================================
  if (currentState == STATE_TRANSMITTING) {
    getLocalTime(&timeinfo);

    // --- PRIORITY 1: BOOT/MANUAL SYNC PERIOD ---
    // If the device was just reset (manual sync), we transmit for BOOT_TX_MINUTES
    // regardless of what the clock says (Midnight rules are ignored here).
    if (millis() - bootTime < (BOOT_TX_MINUTES * 60 * 1000UL)) {
        // Keep transmitting, don't check schedule yet.
        delay(1000);
        return;
    }

    // --- PRIORITY 2: SCHEDULED NIGHT TRANSMISSION ---
    // Only reachable if Boot Period has expired.
    bool isNightWindow = (timeinfo.tm_hour >= NIGHT_START_HOUR && timeinfo.tm_hour < NIGHT_END_HOUR);
    bool isNightTxTime = isNightWindow && (timeinfo.tm_min < NIGHT_TX_MINUTES);

    if (!isNightTxTime) {
      // If we are not in boot period AND not in night schedule, stop.
      Serial.println("Transmission Window Closed. Entering Sleep.");
      stopTransmission(); 
    }
    
    // Check every 1s so we don't block the Ticker
    delay(1000); 
    return;
  }

  // ============================================================
  // STATE 3: IDLE (Light Sleep)
  // ============================================================
  if (currentState == STATE_LOCKED_IDLE) {
    // Heartbeat (Short blink every wake cycle to show it's alive)
    digitalWrite(PIN_LED, LOW);  
    delay(20); 
    digitalWrite(PIN_LED, HIGH); 

    // Check Schedule
    getLocalTime(&timeinfo);
    
    bool isNightWindow = (timeinfo.tm_hour >= NIGHT_START_HOUR && timeinfo.tm_hour < NIGHT_END_HOUR);
    bool isNightTxTime = isNightWindow && (timeinfo.tm_min < NIGHT_TX_MINUTES);

    if (isNightTxTime) {
       Serial.println("Night Schedule Active. Waking up...");
       startTransmission();
       return; 
    }

    // Light Sleep for 15 seconds to save power while maintaining time
    // We wake up periodically to check if the new hour has started
    esp_sleep_enable_timer_wakeup(15 * 1000000ULL); 
    esp_light_sleep_start();
  }
}

// ============================================================
// WIFI LOGIC (RETRIES + CAPTIVE PORTAL)
// ============================================================

void setupWiFi() {
  WiFi.mode(WIFI_STA);WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  
  // 1. Open Preferences with namespace "jjy-config"
  // false = read/write mode
  preferences.begin("jjy-config", false);

  // 2. Read manually saved credentials
  String prefSSID = preferences.getString("ssid", ""); 
  String prefPass = preferences.getString("pass", "");

  // 3. Decide which credentials to use
  if (prefSSID.length() > 0) {
    Serial.print("Reading from Preferences: ");
    Serial.println(prefSSID);
    WiFi.begin(prefSSID.c_str(), prefPass.c_str());
  } else {
    Serial.print("Preferences empty. Using Hardcoded Default: ");
    Serial.println(WIFI_SSID_DEFAULT);
    WiFi.begin(WIFI_SSID_DEFAULT, WIFI_PASS_DEFAULT);
  }

  // 4. Retry Loop (3 Attempts)
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 3) {
    Serial.print("Connecting... Attempt ");
    Serial.println(retries + 1);
    
    // Wait 5 seconds per attempt
    for(int i=0; i<50; i++) {
      if(WiFi.status() == WL_CONNECTED) break;
      delay(100);
    }
    retries++;
  }

  // 5. Success Check
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi Connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    preferences.end(); // Close preferences
    return;
  }

  // 6. Failure -> Captive Portal
  Serial.println("\nConnection Failed. Launching 'JJY-SETUP' Portal.");
  
  // Fast Blink on LED only
  for(int i=0; i<10; i++) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(50); }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // 3 minutes

  // If this returns true, the user connected and the ESP is now connected to WiFi
  if (!wm.startConfigPortal("JJY-SETUP")) {
    Serial.println("Portal Timeout. Rebooting...");
    ESP.restart();
  }

  // 7. CAPTURE AND SAVE NEW CREDENTIALS
  Serial.println("\nConnected via Portal! Saving to Preferences...");
  
  String newSSID = WiFi.SSID();
  String newPass = WiFi.psk();

  // Save to manual storage
  preferences.putString("ssid", newSSID);
  preferences.putString("pass", newPass);
  preferences.end();

  Serial.println("Saved! Rebooting to apply clean state...");
  delay(1000);
  ESP.restart();
}


// ============================================================
// HARDWARE CONTROL (Aligned)
// ============================================================

void startTransmission() {
  Serial.print("Aligning signal to next second...");
  
  struct timeval tv;
  gettimeofday(&tv, NULL);
  
  // Wait until microseconds roll over to 0 for precision
  unsigned long waitMs = 1000 - (tv.tv_usec / 1000);
  if (waitMs > 0 && waitMs < 1000) delay(waitMs);
  
  Serial.println(" NOW!");
  currentState = STATE_TRANSMITTING;
  
  // Attach PWM specifically to PIN_ANT
  if (!ledcAttach(PIN_ANT, JJY_FREQ, PWM_RES)) {
    Serial.println("PWM Fail! Rebooting.");
    ESP.restart();
  }
  
  tickJjy.attach_ms(100, jjyHandler);
}

void stopTransmission() {
  Serial.println("STOPPING SIGNAL.");
  tickJjy.detach();
  
  // Detach PWM from Antenna
  ledcDetach(PIN_ANT);
  pinMode(PIN_ANT, OUTPUT);
  digitalWrite(PIN_ANT, LOW); // Ensure Antenna is OFF

  // Return LED to idle state
  digitalWrite(PIN_LED, HIGH); 
  
  currentState = STATE_LOCKED_IDLE;
}

// ============================================================
// SIGNAL LOGIC
// ============================================================

void jjyHandler() {
  gettimeofday(&tv_now, NULL);
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

// Updated to control both pins:
// ANT = 40kHz PWM
// LED = Digital Visual Sync
void carrierOn() { 
  ledcWrite(PIN_ANT, DUTY_50); 
  digitalWrite(PIN_LED, HIGH); // Visual indicator ON
}

void carrierOff() { 
  ledcWrite(PIN_ANT, DUTY_OFF); 
  digitalWrite(PIN_LED, LOW);  // Visual indicator OFF
}

// ============================================================
// JJY ENCODING 
// ============================================================

int int3bcd(int a) { return (a % 10) + (a / 10 % 10 * 16) + (a / 100 % 10 * 256); }
int parity8(int a) { int pa = a; for(int i = 1; i < 8; i++) pa += a >> i; return pa % 2; }

int getJJYCode() {
  time_t now;
  time(&now);
  now += TIME_SHIFT_SEC;

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
