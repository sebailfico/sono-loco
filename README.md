# AudioMultiRoom

DIY multiroom audio system using ESP32 microcontrollers. No WiFi router or central server required.

Connect your phone via Bluetooth to any ESP32 speaker — the others sync automatically via ESP-NOW mesh.

## How It Works

```
  📱 Phone
      │ Bluetooth A2DP
      ▼
  ┌──────────┐
  │  ESP32   │─── ESP-NOW broadcast ──►  ┌──────────┐
  │ SERVER   │                           │  ESP32   │
  │  🔊      │─── ESP-NOW broadcast ──►  │ CLIENT   │
  └──────────┘                           │  🔊      │
       │                                 └──────────┘
       └──── ESP-NOW broadcast ──────►  ┌──────────┐
                                        │  ESP32   │
                                        │ CLIENT   │
                                        │  🔊      │
                                        └──────────┘
```

Each ESP32 acts as a standalone Bluetooth speaker. When a phone connects:
- That device becomes the **SERVER** and broadcasts audio via ESP-NOW
- All other ESP32s automatically become **CLIENTs** and play the same audio
- When you disconnect, everyone returns to discoverable mode

**No configuration needed** — roles are negotiated automatically at runtime.

## Current Status

🚧 **Work in Progress** — Currently debugging the Bluetooth audio path. ESP-NOW mesh functionality is temporarily disabled while we stabilize the core BT → I2S pipeline.

### What Works
- [x] Bluetooth A2DP sink (phone → ESP32)
- [x] I2S output to PCM5102 DAC
- [x] Notification sounds (startup, connect, disconnect)
- [x] Basic volume control

### Known Issues
- **EMI Interference**: Crackling noise when moving speakers/ESP32/nearby objects. Likely caused by long I2S wires (~20cm) near the ESP32 antenna. See [Hardware Tips](#hardware-tips).
- **Volume too high**: TPA3116 amp has high gain — even at software volume 1/127 it can be loud with small speakers. Hardware attenuation recommended.

### Roadmap
- [ ] Fix EMI/interference issues
- [ ] Re-enable ESP-NOW mesh broadcasting
- [ ] SBC codec for compressed audio over ESP-NOW
- [ ] ESPectre presence detection integration
- [ ] Dynamic volume based on presence
- [ ] Home Assistant integration
- [ ] OTA updates

## Hardware

### Bill of Materials (per speaker unit)

| Component | Model | Cost |
|-----------|-------|------|
| Microcontroller | ESP32 DevKit V1 | ~€5 |
| DAC | PCM5102 I2S module | ~€3 |
| Amplifier | TPA3116 Class D 2x50W | ~€10 |
| Power Supply | 12V DC adapter | ~€10 |
| **Total** | | **~€28** |

### Wiring Diagram

```
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│     ESP32       │      │    PCM5102      │      │    TPA3116      │
│                 │      │     (DAC)       │      │     (AMP)       │
│  GPIO26 (BCK)  ─┼──────┼─► BCK           │      │                 │
│  GPIO25 (DATA) ─┼──────┼─► DIN           │      │                 │
│  GPIO22 (WS)   ─┼──────┼─► LCK           │      │                 │
│  3.3V ──────────┼──────┼─► VCC           │      │                 │
│  GND ───────────┼──────┼─► GND           │      │                 │
│                 │      │  LOUT ──────────┼──────┼─► L IN          │
│                 │      │  ROUT ──────────┼──────┼─► R IN          │
│                 │      │  GND ───────────┼──────┼─► GND           │
└─────────────────┘      └─────────────────┘      │  L+/L- ──► 🔊 L │
                                                  │  R+/R- ──► 🔊 R │
                                                  │  12V DC ◄── PSU │
                                                  └─────────────────┘
```

### PCM5102 Configuration

| Pin | Connection | Function |
|-----|------------|----------|
| FLT | GND | Normal latency |
| DEMP | GND | De-emphasis off |
| XSMT | 3.3V | Soft mute OFF (audio active) |
| FMT | GND | I2S format (standard) |
| SCK | GND | System clock (generated internally) |

### Hardware Tips

**To reduce EMI/interference:**
- Keep I2S wires **short** (<10cm) and twisted together
- Route wires **away** from the ESP32 antenna (the corner with the metal shield)
- Add ferrite beads on I2S lines
- Use separate power supplies for ESP32 and amplifier
- Add decoupling capacitors (100nF + 10µF) near ESP32 and DAC

**To reduce volume with TPA3116:**
- Check for gain jumpers on your TPA3116 module (20dB/26dB/32dB)
- Add a resistor voltage divider between DAC and amp:
  ```
  PCM5102 LOUT ──[10kΩ]──┬── TPA3116 L IN
                         │
                       [1kΩ]
                         │
                        GND
  ```

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)

### Configuration

Edit `AudioMultiRoomClient/include/config.h`:

```cpp
// Room name (visible as Bluetooth speaker name)
#define ROOM_NAME "LivingRoom"

// ESP-NOW channel (0 = auto, or fixed 1-13)
#define ESPNOW_CHANNEL 0
```

### Build & Upload

**Using VS Code:** Use PlatformIO toolbar buttons (Build, Upload, Monitor)

**Using CLI:**
```bash
cd AudioMultiRoomClient

# Build
pio run

# Upload to ESP32
pio run --target upload

# Serial monitor
pio device monitor
```

## Usage

1. Power on the ESP32
2. You'll hear a "bip-bip" startup sound
3. Connect via Bluetooth from your phone (look for the room name)
4. You'll hear a "ta-da" connection sound
5. Play audio — it comes out of the speaker!
6. On disconnect, you'll hear a descending "da-ta" sound

## Libraries

- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) — Bluetooth A2DP Sink
- [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) — Audio framework (for ESP-NOW, currently disabled)
- [arduino-libsbc](https://github.com/pschatzmann/arduino-libsbc) — SBC codec (for ESP-NOW, currently disabled)

## License

MIT License
