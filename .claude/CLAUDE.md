# Claude Code Notes for AudioMultiRoom

## Known Issues / TODO

### Issue 1: Startup & Connection Sounds
**Problem**: Strange noise on power-on and BT connect/disconnect.

**Desired behavior**:
- Power on: Play "bip-bip" (short midi-like beeps)
- BT connect: Play "ta-da" (ascending tones)
- BT disconnect: Play reversed "ta-da" (descending tones)

**Implementation notes**: Need to generate simple tones via I2S. Options:
- Pre-generated PCM samples stored in PROGMEM
- Real-time tone generation (sine waves at specific frequencies)

---

### Issue 2: EMI/Interference - "Frying" Noise
**Problem**: Crackling/frying noise when moving:
- The speakers
- The ESP32
- Objects near the setup

**NOT caused by**: Short circuits or loose cables (confirmed by user).

**Likely causes to investigate**:
1. **EMI pickup on I2S lines** - Long unshielded wires act as antennas
2. **WiFi/BT antenna interference** - ESP32 radio interfering with audio path
3. **Ground loop** - Multiple ground paths between ESP32, DAC, and amplifier
4. **Power supply noise** - Insufficient filtering/decoupling
5. **Capacitive coupling** - Movement changes parasitic capacitance

**Potential fixes**:
- Keep I2S wires short (<10cm) and twisted together
- Add ferrite beads on I2S lines
- Ensure single-point grounding
- Add decoupling capacitors (100nF + 10uF) close to ESP32 and DAC
- Shield I2S wires or use shielded cable
- Separate power supplies for digital (ESP32) and analog (amp) sections

---

## PlatformIO Path

PIO executable location:
```
/c/Users/sebai/.platformio/penv/Scripts/pio.exe
```

Build and upload command:
```bash
cd AudioMultiRoomClient && /c/Users/sebai/.platformio/penv/Scripts/pio.exe run --target upload
```

Monitor serial output:
```bash
/c/Users/sebai/.platformio/penv/Scripts/pio.exe device monitor -p COM7 -b 115200
```

## Hardware

- ESP32 DevKit V1
- PCM5102 DAC (I2S)
- TPA3116 Class D Amplifier
- Upload port: COM7
