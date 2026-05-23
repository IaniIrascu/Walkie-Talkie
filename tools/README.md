# PC Audio Bridge

Tool-ul asta trimite PCM pe 8 biti fara semn intre PC si firmware-ul Arduino-Shell, pe link-ul serial de audio.

Bridge-ul opreste transmiterea din microfonul PC-ului cat timp intra audio din serial, ca sa nu se mai adune sunetul si sa fie redat cu intarziere dupa ce eliberezi PTT.

## Instalare

```bash
py -3 -m venv .venv-win
.\.venv-win\Scripts\python.exe -m pip install -r tools\requirements.txt
```

## Rulare

1. Uploadeaza firmware-ul.
2. Deschide shell-ul placii si alege modul dorit, de exemplu `mode rx`, `mode tx` sau `mode duplex`.
3. Inchide serial monitorul inainte sa pornesti bridge-ul, ca sa poata folosi portul.
4. Porneste bridge-ul pe COM-ul de Bluetooth sau pe USB-serial:

```bash
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM15 --baud 57600 --rate 8000 --serial-rate 801 --rx-only
```

- `--rx-only` doar asculta si reda audio din placa
- `--tx-only` trimite doar din microfonul PC-ului
- Fara niciun flag, bridge-ul merge duplex
- Bytii audio sunt PCM raw, fara semn, centrat la 128

## Optiuni

```bash
.\.venv-win\Scripts\python.exe tools\pc_audio.py --list-devices
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM7 --rx-only
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM7 --tx-only
.\.venv-win\Scripts\python.exe tools\pc_audio.py --port COM7 --in-device 1 --out-device 3
```
