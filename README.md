# Hermes M5Stick Firmware

Firmware for **M5StickC Plus 2** — a physical companion device that connects to **Hermes Agent** over Wi-Fi, displaying session status, token usage, and allowing voice input via the built-in microphone.

This is a fork of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy), adapted to work with the Hermes Agent API instead of the original Claude Bluetooth API. The original project is MIT-licensed by Anthropic, PBC.

---

## Features

- **Real-time dashboard** — displays active sessions, token counts, connection status
- **Voice input** — double-press button A to record (up to 4s), transcribed via **Groq STT** (whisper-large-v3-turbo) and sent to Hermes for response
- **Session browser** — view and scroll through active Hermes sessions
- **18 ASCII pets** — switchable companion characters (robot, capybara, cat, duck, etc.)
- **GIF character support** — upload custom animated characters via SPIFFS (e.g. `bufo`)
- **Animated states** — sleep, idle, busy, celebrate, dizzy, heart — driven by mood and activity
- **Pet system** — mood (0-4 hearts) decays with inactivity; energy (0-5) drains over time; levels up every 50K tokens
- **NTP-synced clock** — auto-orienting portrait/landscape display with RTC
- **Settings menu** — brightness, sound, LED, Wi-Fi info, Hermes info, clock rotation

## Architecture

```
                  +--------------------------------+
                  |      M5StickC PLUS2            |
                  |  (Hermes Desktop Buddy)        |
                  +---------------+----------------+
                                  |
                                  | Wi-Fi (HTTP GET/POST)
                                  v
                  +---------------+----------------+
                  |     Hermes API Gateway         |
                  |       (Port 8642)              |
                  +--------------------------------+
```

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/runs/current` | GET | Poll current run status |
| `/v1/runs/current/stop` | POST | Stop a running Hermes session |
| `/v1/chat/completions` | POST | Send voice transcriptions for chat |
| `/api/sessions` | GET | List active Hermes sessions |

### Voice Pipeline

```
M5Stick Mic -> Groq STT (whisper-large-v3-turbo) -> Hermes Chat -> Response on display
```

## Controls

| Input | Context | Action |
|-------|---------|--------|
| **A** (front) | Normal / Info | Next screen |
| **Double A** | Normal | **Start voice recording** |
| **Hold A** | Any | Open settings menu |
| **B** (side) | Normal / Info | Next page / Back |
| **Power** (short) | Any | Toggle screen on/off |
| **Power** (long) | Any | Power off device |
| **Shake** | Normal | Trigger "dizzy" animation |
| **Face-down** | Normal | Enter power-saving "nap" mode |

## States

| State | Trigger | Description |
|-------|---------|-------------|
| `sleep` | Wi-Fi/API disconnected | Eyes closed, low power |
| `idle` | Connected, no active run | Relaxed animation (varies by mood) |
| `busy` | Run in progress | Working animation |
| `celebrate` | Level up (50K tokens) or high mood | Celebration |
| `dizzy` | Device shaken | Spiral eyes |
| `heart` | High mood | Floating hearts |

## Requirements

- **Hardware**: M5StickC Plus 2 (with PDM microphone and speaker)
- **Software**: [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) (`pip install platformio`)
- **Backend**: Hermes Agent with API server enabled + **Groq API key** for voice STT

## Setup & Flashing

1. Clone the repo:
   ```bash
   git clone https://github.com/Syax89/hermes-m5stick-firmware.git
   cd hermes-m5stick-firmware
   ```

2. Install dependencies and compile:
   ```bash
   pio pkg install
   pio run --environment m5stickc-plus
   ```

3. Upload firmware via USB:
   ```bash
   pio run --environment m5stickc-plus -t upload
   ```

4. To erase NVS and do a clean flash:
   ```bash
   pio run --environment m5stickc-plus -t erase && pio run -t upload
   ```

## Project Structure

```
hermes-m5stick-firmware/
├── src/
│   ├── main.cpp               # Main loop, state machine, UI screens
│   ├── buddy.cpp/h            # ASCII pet dispatch and rendering
│   ├── buddy_common.h         # Shared data structures for buddies
│   ├── buddies/               # 18 ASCII pet definitions
│   ├── character.cpp/h        # GIF decode and rendering
│   ├── data.h                 # Wi-Fi client, API polling, audio pipeline
│   ├── stats.h                # NVS-backed stats, settings, pet levels
│   ├── ble_bridge.cpp/h       # Bluetooth LE (present but unused)
│   ├── xfer.h                 # GIF character transfer over serial
│   ├── setup_wizard.h         # First-run Wi-Fi configuration wizard
│   ├── M5StickCPlus.cpp/h     # M5 wrapper static init fixes
├── characters/                # GIF character packs (e.g. bufo)
│   └── bufo/                  # 13 frames + manifest.json + README.md
├── scripts/
│   └── patch_dfr.py           # Auto-patch for ESP32 Arduino Core v3+ compat
├── platformio.ini             # PlatformIO configuration
├── LICENSE                    # MIT License (see below)
├── CONTRIBUTING.md            # Contribution guidelines (upstream)
└── README.md
```

## Troubleshooting

### Compilation — `analogWriteResolution` error

If you encounter:
```
error: too many arguments to function 'void analogWriteResolution(uint8_t)'
```
The `scripts/patch_dfr.py` script auto-fixes this during build. If it fails, apply manually:
```bash
sed -i 's/analogWriteResolution(_pin0,10);/analogWriteResolution(10);/' \
  .pio/libdeps/m5stickc-plus/DFRobot_GP8XXX/DFRobot_GP8XXX.cpp
```

### First Boot

The device will show a **Wi-Fi Setup Wizard** prompting for SSID, password, Hermes IP, and API key. Configuration is done over USB serial.

## Upstream

This project is a fork of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) (MIT licensed). Key differences from upstream:

- Replaced Claude BLE API with Hermes Agent HTTP API
- Added voice recording pipeline (Groq STT -> Hermes Chat)
- Added session browser, NTP sync, settings menu
- Custom pet mood/energy system
- Various UI improvements and M5StickC Plus 2 compatibility fixes

## License

MIT — see [LICENSE](LICENSE). The GIF assets in `characters/bufo/` are third-party artwork and remain under their original license.
