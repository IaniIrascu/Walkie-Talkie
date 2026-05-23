# Walkie Talkie Firmware (ATmega328P)

Minimal firmware for a DIY walkie-talkie using:

- **MAX9814** microphone module (ADC input)
- **HC-05** Bluetooth (UART data link)
- **Speaker on PWM** (audio output)

Target board: **ATmega328P Xplained Mini** (or Arduino Uno).

Reference: https://ocw.cs.pub.ro/courses/pm/proiect/xplainedmini

## Wiring

### MAX9814

- VDD -> 5V
- GND -> GND
- OUT -> A0 / PC0 / ADC0
- GAIN -> not connected (default gain)
- AR -> not connected (default AGC)

### HC-05

- VCC -> 5V
- GND -> GND
- TX -> PD0 / RX (MCU RX)
- RX -> PD1 / TX (MCU TX) via voltage divider
- STATE -> not connected
- EN -> not connected (AT mode off)

### Speaker

- Speaker + -> PB1 / OC1A / D9 (PWM)
- Speaker - -> GND

## Build / Upload / Monitor

```bash
platformio run --environment avr
platformio run --target upload --environment avr
platformio device monitor --environment avr
```

Default UART baud is **57600** (see `platformio.ini`).
If your HC-05 data mode is still **9600**, change `-DUART_BAUD` and `monitor_speed`.

## Commands

Type `help` to list commands.

- `status` - show current mode and last ADC sample
- `mode <idle|loop|tx|rx>`
  - `loop`: mic -> speaker (local test)
  - `tx`: mic -> UART (send)
  - `rx`: UART -> speaker (receive)
- `adc [count]` - print raw ADC samples
- `pwm <0-255|off>` - fixed PWM output
- `stats` - show TX/RX drop counters

Note: In `tx`/`rx` mode, the UART carries **raw audio bytes**. Reset the board to exit.

## Test Plan

1) **UART / Console**
  - `status` should respond with `walkie>` prompt.

2) **ADC (mic)**
  - `adc 20`
  - Speak or tap the mic and watch values change.

3) **PWM (speaker)**
  - `pwm 128` (silence)
  - `pwm 255` (max)
  - `pwm 0`

4) **Local loopback**
  - `mode loop`
  - You should hear your voice on the speaker.
  - Reset board to exit.

5) **Walkie test (two boards)**
  - Board A: `mode tx`
  - Board B: `mode rx`
  - Talk on A, listen on B. Swap roles by resetting and changing modes.

## Notes

- UART speed must be high enough for audio. **57600** is recommended for this setup.
- If audio is distorted, check power, wiring, and baud settings.
- HC-05 RX must be level-shifted (voltage divider).

## License

This project is licensed under the **MIT License** (see `LICENSE`).
