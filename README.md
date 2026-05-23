# Walkie Talkie Firmware (ATmega328P)

Firmware pentru un walkie-talkie cu piesele astea:

- **MAX9814** pentru microfon, pe ADC0
- **HC-05** pentru legatura pe serial / Bluetooth
- **Speaker pe PWM**, iesire pe OC1A / D9

Placa **ATmega328P Xplained Mini**

Referinta: https://ocw.cs.pub.ro/courses/pm/prj2026/alexandru.jipa2803/iani.irascu

## Conectare

### MAX9814

- VDD -> 5V
- GND -> GND
- OUT -> A0 / PC0 / ADC0
- GAIN -> neconectat
- AR -> neconectat

### HC-05

- VCC -> 5V
- GND -> GND
- TX -> PD0 / RX (RX de pe MCU)
- RX -> PD1 / TX (TX de pe MCU) prin divizor de tensiune
- STATE -> neconectat
- EN -> neconectat

### Speaker

- Speaker + -> PB1 / OC1A / D9 (PWM)
- Speaker - -> GND

Baud-ul UART implicit este **57600**
Daca HC-05-ul tau inca merge pe **9600**, schimba `UART_BAUD` din cod si `monitor_speed` din platformio.ini

## Comenzi

Scrie `help` ca sa vezi lista.

- `status` - arata modul curent si setarile
- `mode <idle|tx|rx|duplex>`
  - `idle`: doar shell
  - `tx`: microfon -> UART, butonul PTT de pe D7 porneste transmisia
  - `rx`: audio din UART -> speaker
  - `duplex`: mod walkie half-duplex cu acelasi buton PTT
- `bt <AT command>` - trimite o comanda AT catre HC-05 pe pinii dedicati
- `rxgain <1-255>` - seteaza gain-ul pentru sunetul receptionat

pe UART merg bytes audio raw, PCM pe 8 biti fara semn, centrat la 128. In `tx` si `duplex`, placa transmite doar cat timp tineti apasat PTT.

## Test rapid

1. **Shell**
   - Da `status` si vezi daca raspunde cu modul curent.

2. **TX**
   - Pune `mode tx`.
   - Tine apasat PTT si verifica daca audio-ul pleaca doar cat timp apesi.

3. **RX**
   - Pune `mode rx`.
   - Trimite PCM raw pe UART si verifica daca se aude in speaker.

4. **Duplex**
   - Pune `mode duplex`.
   - Tine PTT apasat ca sa vorbesti, apoi da drumul ca sa auzi ce vine inapoi.

5. **Comenzi AT pe BT**
   - Foloseste `bt AT` sau `bt AT+UART?` cand HC-05 e in mod AT.
