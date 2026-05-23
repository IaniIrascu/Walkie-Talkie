# PC Audio Bridge

This tool streams 8-bit unsigned PCM between your PC and the Arduino-Shell firmware audio mode.

## Install

```bash
py -3 -m venv .venv-win
.\.venv-win\Scripts\python.exe -m pip install -r tools\requirements.txt
```

## Run

1. Upload firmware.
2. Open the serial monitor at 9600 and run:
   - `mode tx`
3. Close the serial monitor.
4. Start the PC bridge on the Bluetooth COM port (or USB-Serial):

```bash
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM15 --baud 9600 --rate 8000 --serial-rate 801 --rx-only
```

Notes:
- Audio is raw bytes; garbled text in the monitor after `mode tx` is expected.
- The firmware keeps the serial audio sample rate below the 9600 baud UART capacity. The PC audio device still runs at a normal rate such as 8000 Hz.
- At 9600 baud this is a connectivity test, not high quality audio. For clearer voice, raise the HC-05 UART baud later and increase the serial PCM rate.
- If you use the HC-05 module, use the Bluetooth COM port on the PC.
- Avoid having both USB-serial and HC-05 drive UART at the same time.

## Options

```bash
.\.venv-win\Scripts\python.exe tools\pc_audio.py --list-devices
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM7 --rx-only
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM7 --tx-only
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM7 --in-device 1 --out-device 3
```
