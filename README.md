
# ESP32-C3 JJY Time Signal Transmitter (Precision & Low Power)

A high-precision, battery-optimized JJY (40kHz) time signal emulator designed for the **ESP32-C3**.

This project allows you to synchronize Japanese Radio Controlled Clocks (intended for reception of the Fukushima `JJY` signal) anywhere in the world. It is specifically engineered for regions with unstable internet connections (like the Philippines) and long-term battery operation.

![Transmitter working on Casio Lineage LCW-M100TSE-1A](https://raw.githubusercontent.com/rrabargos/jjyvibetimetransmitter/main/image.jpg "Transmitter working on Casio Lineage LCW-M100TSE-1A")

Transmitter code is based on BotanicFields' JJY transmitter code: [Generate Standard Radio Wave JJY with Ticker of M5StickC](https://www.hackster.io/botanicfields/generate-standard-radio-wave-jjy-with-ticker-of-m5stickc-0330e0)

## 🌟 Key Features

*   **Atomic Precision:** Uses the ESP32's High-Resolution Hardware Timer (`esp_timer`) running in IRAM for microsecond-perfect pulse timing.
*   **Zero-Drift Sleep:** Forces the 40MHz Crystal (XTAL) to remain active during Light Sleep, ensuring the clock stays perfectly aligned between the 23:50 sync and the 00:00 broadcast.
*   **Robust NTP Strategy:**
    1.  Prioritizes Google NTP (Anycast/Asia nodes) for low latency.
    2.  Falls back to NIST and Pool.org.
    3.  **Hardcoded IP Fallback:** Bypasses DNS entirely if local ISP DNS servers fail (common in PH).
    4.  **Safety Valve:** Sleeps for 1 hour if internet is totally down to prevent battery drain from infinite reboot loops.
*   **Smart Power Management:**
    *   **Daytime (05:00 - 23:50):** Deep Sleep (~5µA).
    *   **Nighttime (00:00 - 05:00):** Light Sleep between hourly broadcasts.
    *   **WiFi:** Only active for ~20 seconds per day.
*   **Remote Logging:** Batches system logs and uploads them to **Google Sheets** via Google Apps Script (Boot status, Sleep durations, Sync success).
*   **Drift Fix:** intelligently detects if the device woke up from Deep Sleep to skip the "Power-On Test Mode," preserving exact midnight alignment.
*   **WiFi Manager:** No hardcoded credentials. Connect to the Captive Portal (`JJY-SETUP`) to configure WiFi.

## 🛠 Hardware Requirements

1.  **Microcontroller:** ESP32-C3 (e.g., SuperMini, DevKitM-1).
    *   *Note: Code is optimized for C3 RISC-V architecture.*
2.  **Antenna:**
    *   **Simple:** A long wire (1-2 meters) connected to GPIO 1.
    *   **Better:** A 40kHz tuned Ferrite Rod antenna (requires a transistor/MOSFET driver circuit for range > 10cm).
3.  **Power:** 3.7V LiPo Battery or USB Power.

### Pinout
| Function | Pin (ESP32-C3) | Description |
| :--- | :--- | :--- |
| **Antenna Output** | **GPIO 1** | Generates the 40kHz PWM signal |
| **Status LED** | **GPIO 8** | Onboard LED (Cold boot / Error status) |

## 📊 Google Sheets Logging Setup

To receive logs from the device, you must deploy a Google Apps Script.

1.  Create a new **Google Sheet**.
2.  Go to **Extensions** > **Apps Script**.
3.  Paste the following code:
    ```javascript
    function doPost(e) {
      var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
      var data = JSON.parse(e.postData.contents);
      sheet.appendRow([new Date(), data.deviceId, data.log]);
      return ContentService.createTextOutput("Success");
    }
    ```
4.  Click **Deploy** > **New Deployment**.
    *   **Select type:** Web app.
    *   **Description:** JJY Logger.
    *   **Execute as:** Me (your email).
    *   **Who has access:** **Anyone** (Critical: allows ESP32 to write without login).
5.  Copy the **Web App URL**.
6.  Paste this URL into the `G_SCRIPT_URL` variable in the Arduino sketch.

## 📥 Installation

1.  **Install Arduino IDE** and the **ESP32 Board Manager** (v2.0.4 or later recommended).
2.  Install Required Libraries:
    *   `WiFiManager` (by tzpu).
3.  **Board Settings (Tools Menu):**
    *   **Board:** ESP32C3 Dev Module.
    *   **USB CDC On Boot:** **Enabled** (Required to see Serial Logs).
    *   **Flash Mode:** DIO / QIO.
4.  **Configure Code:**
    *   Update `G_SCRIPT_URL` with your Google Script URL.
    *   (Optional) Adjust `GMT_OFFSET_SEC` if you are not in UTC+9 (JST).
5.  **Upload** the code.

## ⚙️ How It Works (The Daily Cycle)

1.  **First Power On (Manual Reset):**
    *   Connects to WiFi (launches Captive Portal if fails 3x).
    *   Syncs NTP (Google -> IP Fallback).
    *   **Test Mode:** Transmits immediately for **10 minutes** so you can test reception.
    *   Enters Deep Sleep.

2.  **Daytime (Idle):**
    *   Device is in **Deep Sleep**. Power consumption is negligible.

3.  **The 23:50 Wake-Up:**
    *   Device wakes up automatically at 23:50.
    *   Detects `ESP_RST_DEEPSLEEP` -> **Skips Test Mode**.
    *   Syncs NTP to get atomic precision.
    *   Enters **Light Sleep (XTAL ON)** until exactly 00:00:00.

4.  **Night Schedule (00:00 - 05:00):**
    *   **Minutes 00-15:** Transmits JJY signal.
    *   **Minutes 15-59:** Light Sleep (XTAL ON) to save battery.
    *   Repeats every hour until 05:00.

5.  **Morning (05:00):**
    *   Uploads all batched logs to Google Sheets.
    *   Goes back to Deep Sleep until 23:50.

## 🐛 Troubleshooting

*   **No Serial Logs?**
    *   Ensure **"USB CDC On Boot"** is ENABLED in Arduino IDE Tools.
    *   The device sleeps often. You may need to press the `BOOT` button or reset it manually to wake it up for debugging.
*   **WiFi Won't Connect?**
    *   If the saved credentials fail 3 times, look for a WiFi Hotspot named **`JJY-SETUP`**. Connect to it (IP `192.168.4.1`) to configure WiFi.
*   **Clock sets to wrong time?**
    *   The device emulates **JST (UTC+9)** regardless of your local time. Your clock will show Japan time.
    *   Check `TIME_SHIFT_SEC` in the code if you want to fake a different timezone.

## ⚠️ Legal Note
This device generates radio frequency signals. While the output power from a GPIO pin is very low, please ensure you comply with your local radio regulations regarding experimental transmissions in the 40kHz - 60kHz band.

---
*Author: Rigor Abargos*
