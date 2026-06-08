# hermes-desktop-buddy

Hermes Desktop Buddy is a physical companion device built on the **M5StickC PLUS2** (ESP32) platform that connects directly to your **Hermes API Daemon** over Wi-Fi. It allows you to monitor agent runs, receive real-time notifications (including chirps and LED blinks), and approve or deny tool execution prompts right from your desk.

## Architecture

Unlike the original Bluetooth BLE implementation, this version connects directly to the Hermes API Gateway over Wi-Fi. It polls `/v1/runs/current` and interacts with the `/v1/runs/current/approval` endpoint.

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

### Configuration Details
* **Server Target**: `http://192.168.1.100:8642/v1/runs/current`
* **Authorization**: `Bearer <YOUR_HERMES_API_KEY>`
* **Wi-Fi**: Configured directly in the device memory (or NVS preferences).

## Controls

| Button | Screen Context | Action |
| --- | --- | --- |
| **A** (Front) | Normal / Info | Switch to the next screen |
| **A** (Front) | Approval Prompt | **Approve tool execution** |
| **B** (Side) | Normal / Info | Switch to the next page |
| **B** (Side) | Approval Prompt | **Deny tool execution** |
| **Hold A** | Any | Open Settings Menu |
| **Power** (Left, short) | Any | Toggle screen backlight on/off |
| **Power** (Left, long) | Any | Power off device |
| **Shake** | Normal | Put buddy in "Dizzy" animation state |
| **Face-down** | Normal | Put buddy in "Nap" (power-saving) state |

## The Seven States

| State | Trigger | Animation Description |
| --- | --- | --- |
| `sleep` | Wi-Fi / Gateway disconnected | Eyes closed, low-power mode |
| `idle` | Connected, no active run | Robot blinking, looking around |
| `busy` | Hermes agent run is actively executing | Sweating, running cycles |
| `attention` | Run is waiting for tool approval | Alert state, LED blinks and buzzer chirps |
| `celebrate` | Level up (every 50K tokens processed) | Bouncing, celebrate screen |
| `dizzy` | Device was shaken | Spiral eyes, shaking |
| `heart` | Prompt approved in under 5 seconds | Floating hearts, happy |

## ASCII Species
The device retains support for all **18 ASCII species** (including `robot`, `capybara`, `duck`, etc.). You can switch between them through the Settings menu.

## GIF Characters
Custom GIF characters (like `bufo`) can be flashed into the device's SPIFFS filesystem partitions. The buddy will automatically switch to GIF rendering if a valid filesystem character pack is detected.

## Flashing the Device

Install Python and [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/), then compile and upload the firmware via USB:

```bash
# Compile and flash via USB
python3 -m platformio run -t upload
```

To erase the NVS cache and perform a clean flash:
```bash
python3 -m platformio run -t erase && python3 -m platformio run -t upload
```

## Project Layout

```
src/
  main.cpp       — loop, state machine, UI screens
  buddy.cpp      — ASCII species dispatch + render helpers
  buddies/       — active robot buddy animation helper (robot.cpp)
  character.cpp  — GIF decode + render
  data.h         — Wi-Fi client, GET status, POST approval
  stats.h        — NVS-backed stats, settings, owner choice
  M5StickCPlus.h/cpp — static init fixes & M5 wrapper
characters/      — example GIF character packs (e.g. bufo)
```
