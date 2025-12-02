# ESP32-C3 JJY Vibe Time Signal Transmitter

Vibe coded with Gemini 3 Pro Preview

A compact JJY (Japan Standard Time) 40 kHz time-signal emulator for the ESP32-C3.  
It synchronizes via NTP (UTC+9), generates accurate JJY minute frames, aligns transmission edges to the next UTC-aligned second, and uses PWM to broadcast a 40 kHz carrier suitable for watch-synchronization experiments.

Can transmit when placed directly over or under the watch.
![Transmitter working on Casio Lineage LCW-M100TSE-1A](https://raw.githubusercontent.com/rrabargos/jjyvibetimetransmitter/main/image.jpg "Transmitter working on Casio Lineage LCW-M100TSE-1A")

Transmitter code is based on BotanicFields' JJY transmitter code: [Generate Standard Radio Wave JJY with Ticker of M5StickC](https://www.hackster.io/botanicfields/generate-standard-radio-wave-jjy-with-ticker-of-m5stickc-0330e0)

> This project is for educational and hobby RF experimentation only.  
> JJY is a real government-operated time service. Only low-power, near-field lab testing is recommended.

---

## Features

- Full JJY minute-frame encoding (seconds 0–59)
- Microsecond-aligned transmission start for stable timing
- Automatic NTP sync (with Wi-Fi disabled afterward)
- Night-schedule transmission window (00:00–05:00 JST)
- Forced TX window on boot
- Low-power idle mode between broadcasts
- Shared pin for LED + antenna output
- Uses ESP32-C3 LEDC PWM (`ledcAttach`)

---

## Hardware

- ESP32-C3 development board  
- Output pin: **PIN_TRX = 8**  
  - Drives a small wire antenna  
  - Also used for “heartbeat” idle indication  

---

## Wiring

```
ESP32-C3 Pin 8  →  Antenna (short wire)
ESP32-C3 GND    →  Ground
```

For near-field testing, 5–20 cm of wire is usually enough.

---

## Installation

1. Install **Arduino IDE** and **ESP32 Board Manager 3.0+**  
2. Open the source file
3. Adjust configuration:

```cpp
const char* WIFI_SSID = "your_wifi";
const char* WIFI_PASS = "your_password";

const int TIME_SHIFT_SEC = 0;
const int JJY_FREQ = 40000;  // 40kHz
```

4. Arduino IDE settings:
   - Board: **ESP32-C3 Dev Module**
   - Upload Speed: **921600**
   - Select the board’s COM port

5. Upload the firmware.

---

## How It Works

### Boot Phase
- Connects to Wi-Fi  
- Synchronizes via NTP (UTC+9)  
- Shuts Wi-Fi down for RF stability  

### Transmission Phase
A 100 ms ticker interrupt generates the amplitude-modulated pattern used by JJY:

- “1” → carrier on 0–200 ms  
- “0” → carrier on 0–100 ms  
- “Marker” → carrier on 0–200 ms, off 200–900 ms  

The transmitter aligns the start of transmission to the next second boundary for low jitter.

### Idle Phase
After scheduled hours:
- PWM stops  
- Light sleep mode activates  
- Wakes periodically to check if TX window has opened  

---

## Configuration Summary

| Setting | Description |
|--------|-------------|
| `TIME_SHIFT_SEC` | Manual drift correction |
| `FORCE_TX_MINUTES` | Guaranteed transmission after boot |
| `JJY_FREQ` | Carrier frequency (default: 40 kHz) |
| `PIN_TRX` | Shared antenna + LED pin |
| `GMT_OFFSET_SEC` | JST offset for JJY (UTC+9) |

---

## Legal Notes

- Do not transmit strong RF signals on 40 kHz.  
- Use only for shielded or near-field testing.  
- Long-range broadcasting may be illegal depending on region.  

---

## License

MIT License  
Copyright © 2025  
Rigor Abargos
