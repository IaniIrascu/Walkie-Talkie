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


## Explicatii pe scurt

### De ce am ales bibliotecile

- `Arduino.h` e baza normala pentru proiect, fiindca imi da acces direct la `Serial`, `pinMode`, `millis` si functiile de baza.
- `SoftwareSerial.h` am folosit-o pentru partea de AT cu HC-05, ca sa nu blochez UART-ul principal folosit pentru audio si consola.
- `avr/interrupt.h` e importanta pentru ca tot fluxul audio se bazeaza pe intreruperea de ADC, deci prelucrarea e facuta stabil si repetabil.
- Pe partea de PC, `pyserial` e alegerea simpla pentru comunicatia seriala, iar `sounddevice` se potriveste bine pentru captare si redare audio in timp real.
- `array` si `math` in scriptul Python ajuta la conversie audio si la calcule simple pentru nivel, fara sa complic codul inutil.

### Elementul de noutate

Partea mai interesanta la proiect nu e doar ca trimite audio pe serial, ci faptul ca am facut o solutie half-duplex controlata cu PTT, cu modul `duplex`, cu curatare de buffer la eliberarea butonului si cu bridge pe PC care opreste microfonul cat timp intra audio din placa. Asta reduce mult efectul de replay intarziat si face sistemul mai aproape de un walkie-talkie real.

### Ce functionalitati din laborator am folosit

- ADC-ul a fost folosit pentru citirea microfonului.
- PWM-ul a fost folosit pentru redarea sunetului pe speaker.
- UART-ul a fost folosit pentru legatura dintre placa si PC / HC-05.
- Intreruperile au fost folosite ca sa lucrez pe esantioane fara sa depind de `loop()`.
- Timer-ele si configurarea registrilor AVR sunt folosite pentru PWM si pentru ritmul audio.
- `SoftwareSerial` m-a ajutat sa separ partea de AT de fluxul principal de date.

### Scheletul proiectului

Proiectul are doua parti mari:

1. **Firmware-ul de pe placa**
   - citeste microfonul pe ADC;
   - decide daca transmite sau reda audio;
   - foloseste PTT-ul ca sa stie cand are voie sa trimita;
   - expune comenzi de shell pentru testare si calibrare.

2. **Bridge-ul de pe PC**
   - citeste si scrie date pe serial;
   - capteaza audio din microfonul laptopului;
   - reda sunetul primit din placa;
   - opreste transmiterea din microfon cand primeste audio, ca sa nu bage inapoi in sistem ce tocmai se aude.

Interactiunea e simpla: placa transforma audio in bytes, PC-ul ii trimite mai departe sau ii reda, iar PTT-ul decide cine are prioritate. In modul `duplex`, ramane intotdeauna o regula clara: cand apas butonul, transmit; cand il eliberez, ascult.

### Cum am validat ca functioneaza

Am validat in mai multe etape:

- am compilat proiectul cu PlatformIO;
- am incarcat firmware-ul pe placa;
- am verificat shell-ul cu `status` si `help`;
- am testat separat `mode tx` si `mode rx`;
- am verificat ca in `duplex` nu mai ramane audio blocat in buffer dupa eliberarea PTT-ului;
- am testat si bridge-ul Python, unde am verificat ca nu mai apare replay-ul intarziat;
- am urmarit si nivelul semnalului si buffer-ele, ca sa vad daca apar pierderi sau intarzieri mari.

### Cum am facut calibrarea elementelor de senzoristica

La partea de microfon nu am facut o calibrare hardware complicata, ci una practica, din software:

- am pornit de la semnalul centrat la 128, ca sa lucrez in PCM pe 8 biti fara semn;
- am observat valorile brute din ADC si am verificat daca exista offset mare de DC;
- am folosit filtrul simplu cu estimare de DC si noise gate pentru a reduce bazaitul;
- am pastrat gain-ul de receptie ca parametru reglabil din comanda `rxgain`;
- am verificat si clipping-ul, ca sa nu imping semnalul prea tare nici pe TX, nici pe RX.

Pe scurt, calibrarea a fost facuta prin masurare, ascultare si ajustare, pana cand semnalul a devenit stabil si usor de folosit.

### Unde si de ce am facut optimizari

Optimizarea am facut-o acolo unde conteaza cel mai mult, adica in zona de timp real:

- in ISR-ul de ADC, ca sa procesez esantioanele imediat si sa nu pierd ritmul;
- in buffere circulare, ca sa evit alocarile dinamice si copiile inutile;
- in downsampling, ca sa trimit doar cat trebuie pe UART si sa nu aglomerez legatura;
- in filtrarea audio, unde am folosit calcule simple pe intregi, nu formule complicate;
- in bridge-ul de PC, unde am limitat cat de mult se poate incarca buffer-ul de receptie, ca sa ramana intarzierea mica.

De ce am facut asta: fiindca proiectul lucreaza cu audio in timp real, iar orice copie inutila, buffer prea mare sau procesare grea se simte imediat in intarziere si calitate.
