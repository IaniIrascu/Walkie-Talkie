#include <Arduino.h>
#include <SoftwareSerial.h>
#include <avr/interrupt.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define UART_BAUD 57600UL
#define BOOT_MODE 0
#define RX_AUDIO_GAIN 5
#define SERIAL_AUDIO_DIVIDER 2
#define BT_AT_BAUD 38400UL

namespace {

constexpr uint8_t kPwmPin = 9;      // pin PWM (PB1/OC1A)
constexpr uint8_t kAdcChannel = 0;  // ADC0 (A0)
constexpr uint8_t kBtAtRxPin = 2;   // pin rx pentru HC-05 AT (PD2/D2)
constexpr uint8_t kBtAtTxPin = 3;   // pin tx pentru HC-05 AT (PD3/D3)
constexpr uint8_t kPttPin = 7;      // buton PTT pe PB7 (MCU)
constexpr uint8_t kSilence = 128;
constexpr size_t kLineBufSize = 64;
constexpr uint16_t kDefaultMicWindowMs = 1000;
constexpr uint16_t kMaxMicWindowMs = 10000;
constexpr uint16_t kDuplexRxFlushMs = 35;
constexpr uint8_t kClipLow = 2;
constexpr uint8_t kClipHigh = 253;
constexpr uint8_t kTxNoiseGate = 3;
constexpr uint8_t kSerialAudioDivider = SERIAL_AUDIO_DIVIDER;
constexpr uint8_t kRxAudioGain = RX_AUDIO_GAIN;

constexpr uint8_t kTxBufSize = 128;
constexpr uint8_t kTxBufMask = kTxBufSize - 1U;
constexpr uint8_t kRxBufSize = 128;
constexpr uint8_t kRxBufMask = kRxBufSize - 1U;

enum class Mode : uint8_t { Idle, Tx, Rx, Duplex };

volatile Mode gMode = Mode::Idle;
volatile uint8_t gLastAdc = kSilence;
volatile bool gFixedPwmEnabled = false;
volatile uint8_t gFixedPwmValue = kSilence;
volatile bool gPttPressed = false;

volatile uint8_t gTxBuf[kTxBufSize];
volatile uint8_t gTxHead = 0;
volatile uint8_t gTxTail = 0;
volatile uint8_t gRxBuf[kRxBufSize];
volatile uint8_t gRxHead = 0;
volatile uint8_t gRxTail = 0;
volatile uint16_t gTxDrops = 0;
volatile uint16_t gRxDrops = 0;
volatile uint8_t gSerialAudioTick = 0;
volatile uint8_t gRxPlaybackSample = kSilence;
volatile uint32_t gRxSamplesPlayed = 0;
volatile uint32_t gTxSamplesSent = 0;
volatile int16_t gMicDcEstimate = 0;
uint32_t gDuplexRxDropUntilMs = 0;

// timestamp de la ultimul byte rx
volatile uint32_t gRxActivityMs = 0;
// gain rx reglabil la runtime
volatile uint8_t gRxAudioGainVar = kRxAudioGain;

// statistici microfon
volatile bool gMicStatsEnabled = false;
volatile uint32_t gMicStatsCount = 0;
volatile uint32_t gMicStatsSum = 0;
volatile uint32_t gMicStatsAbsSum = 0;
volatile uint16_t gMicStatsClipsLow = 0;
volatile uint16_t gMicStatsClipsHigh = 0;
volatile uint16_t gMicStatsCrossings = 0;
volatile uint8_t gMicStatsMin = 0xFF;
volatile uint8_t gMicStatsMax = 0;
volatile bool gMicStatsPrevAboveMid = false;

char gLineBuf[kLineBufSize];
size_t gLineLen = 0;

SoftwareSerial gBtAtSerial(kBtAtRxPin, kBtAtTxPin);

Mode bootMode() {
#if BOOT_MODE == 1
  return Mode::Idle;
#elif BOOT_MODE == 2
  return Mode::Tx;
#elif BOOT_MODE == 3
  return Mode::Rx;
#elif BOOT_MODE == 4
  return Mode::Duplex;
#else
  return Mode::Idle;
#endif
}

// returneaza urmatorul index circular
inline uint8_t nextIndex(uint8_t idx, uint8_t mask) {
  return static_cast<uint8_t>((idx + 1U) & mask);
}

inline bool txPush(uint8_t value) {
  const uint8_t next = nextIndex(gTxHead, kTxBufMask);
  if (next == gTxTail) {
    if (gTxDrops < 0xFFFFU) {
      ++gTxDrops;
    }
    return false;
  }
  gTxBuf[gTxHead] = value;
  gTxHead = next;
  return true;
}

inline bool txPop(uint8_t &value) {
  if (gTxHead == gTxTail) {
    return false;
  }
  value = gTxBuf[gTxTail];
  gTxTail = nextIndex(gTxTail, kTxBufMask);
  return true;
}

inline bool rxPush(uint8_t value) {
  const uint8_t next = nextIndex(gRxHead, kRxBufMask);
  if (next == gRxTail) {
    if (gRxDrops < 0xFFFFU) {
      ++gRxDrops;
    }
    return false;
  }
  gRxBuf[gRxHead] = value;
  gRxHead = next;
  return true;
}

inline bool rxPop(uint8_t &value) {
  if (gRxHead == gRxTail) {
    return false;
  }
  value = gRxBuf[gRxTail];
  gRxTail = nextIndex(gRxTail, kRxBufMask);
  return true;
}

uint8_t applyAudioGain(uint8_t sample, uint8_t gain) {
  int16_t centered = static_cast<int16_t>(sample) - static_cast<int16_t>(kSilence);
  centered *= gain;
  centered += kSilence;
  if (centered < 0) {
    return 0;
  }
  if (centered > 255) {
    return 255;
  }
  return static_cast<uint8_t>(centered);
}

uint8_t cleanMicSample(uint8_t sample) {
  // filtru simplu anti bazait: scot DC + noise gate mic
  const int16_t centered = static_cast<int16_t>(sample) - static_cast<int16_t>(kSilence);
  gMicDcEstimate += (centered - gMicDcEstimate) >> 5;
  int16_t hp = centered - gMicDcEstimate;

  if (hp < static_cast<int16_t>(kTxNoiseGate) && hp > -static_cast<int16_t>(kTxNoiseGate)) {
    hp = 0;
  }

  int16_t out = hp + static_cast<int16_t>(kSilence);
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return static_cast<uint8_t>(out);
}

void resetBuffers() {
  noInterrupts();
  gTxHead = 0;
  gTxTail = 0;
  gRxHead = 0;
  gRxTail = 0;
  gTxDrops = 0;
  gRxDrops = 0;
  gSerialAudioTick = 0;
  gRxPlaybackSample = kSilence;
  gRxSamplesPlayed = 0;
  gTxSamplesSent = 0;
  interrupts();
}

void setupPwm() {
  pinMode(kPwmPin, OUTPUT);
  // setez fast PWM pe 8 biti
  TCCR1A = _BV(COM1A1) | _BV(WGM10);
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A = kSilence;
}

void setupAdc() {
  ADMUX = _BV(REFS0) | _BV(ADLAR) | (kAdcChannel & 0x07);
  ADCSRA = _BV(ADEN) | _BV(ADATE) | _BV(ADIE) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
  ADCSRB = 0;
  DIDR0 |= _BV(ADC0D);
  ADCSRA |= _BV(ADSC);
}

void setupPtt() {
  // configurare PB7 ca intrare si pull-up intern
  DDRB &= ~_BV(PB7);
  PORTB |= _BV(PB7);
}

bool readPttButton() {
  // butonul e activ pe low
  return (PINB & _BV(PB7)) == 0;
}

const __FlashStringHelper *modeName(Mode mode) {
  switch (mode) {
    case Mode::Idle:
      return F("idle");
    case Mode::Tx:
      return F("tx");
    case Mode::Rx:
      return F("rx");
    case Mode::Duplex:
      return F("duplex");
  }
  return F("idle");
}

bool parseU16(const char *text, uint16_t &value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const unsigned long raw = strtoul(text, &end, 10);
  if (*end != '\0' || raw > 0xFFFFUL) {
    return false;
  }
  value = static_cast<uint16_t>(raw);
  return true;
}

bool parseU8(const char *text, uint8_t &value) {
  uint16_t raw = 0;
  if (!parseU16(text, raw) || raw > 0xFFU) {
    return false;
  }
  value = static_cast<uint8_t>(raw);
  return true;
}

void printFixedTenths(uint32_t tenths) {
  Serial.print(tenths / 10UL);
  Serial.print('.');
  Serial.print(tenths % 10UL);
}

uint8_t calcLevelFromPeakToPeak(uint8_t minValue, uint8_t maxValue) {
  const uint16_t p2p = static_cast<uint16_t>(maxValue) - minValue;
  const uint16_t level = (p2p + 1U) / 2U;
  return level > 127U ? 127U : static_cast<uint8_t>(level);
}

void printLevelBar(uint8_t level) {
  const uint8_t width = 32;
  uint8_t filled = static_cast<uint8_t>((static_cast<uint16_t>(level) * width + 63U) / 127U);
  if (filled > width) {
    filled = width;
  }
  Serial.print('[');
  for (uint8_t i = 0; i < width; ++i) {
    Serial.print(i < filled ? '#' : '.');
  }
  Serial.print(']');
}

size_t splitArgs(char *text, char *argv[], size_t maxArgs) {
  size_t argc = 0;
  char *p = text;
  while (*p != '\0' && argc < maxArgs) {
    while (*p == ' ') {
      ++p;
    }
    if (*p == '\0') {
      break;
    }
    argv[argc++] = p;
    while (*p != '\0' && *p != ' ') {
      ++p;
    }
    if (*p == ' ') {
      *p = '\0';
      ++p;
    }
  }
  return argc;
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  help"));
  Serial.println(F("  status"));
  Serial.println(F("  mode <idle|tx|rx|duplex>"));
  Serial.println(F("  bt <AT command>      - send command to HC-05 AT mode"));
}

void printStatus() {
  Serial.println(F("\n=== Walkie Status ==="));
  Serial.print(F("UART baud: "));
  Serial.println(UART_BAUD);
  Serial.print(F("Mode: "));
  Serial.println(modeName(gMode));
  Serial.print(F("ADC rate approx: 9615 samples/s"));
  Serial.println();
  Serial.print(F("Serial PCM rate approx: "));
  Serial.println(9615UL / kSerialAudioDivider);
  Serial.print(F("RX audio gain: "));
  Serial.println(gRxAudioGainVar);
  if (gFixedPwmEnabled) {
    Serial.println(gFixedPwmValue);
  } else {
    Serial.println(F("off"));
  }
  Serial.println();
}

void handleCommand(char *line) {
  char *argv[4] = {};
  const size_t argc = splitArgs(line, argv, 4);
  if (argc == 0) {
    return;
  }

  if (strcmp(argv[0], "help") == 0) {
    printHelp();
    return;
  }
  
  if (strcmp(argv[0], "status") == 0) {
    printStatus();
    return;
  }
  
  if (strcmp(argv[0], "mode") == 0) {
    if (argc == 1) {
      Serial.print(F("Mode: "));
      Serial.println(modeName(gMode));
      return;
    }
    Mode next = gMode;
    if (strcmp(argv[1], "idle") == 0) {
      next = Mode::Idle;
    } else if (strcmp(argv[1], "tx") == 0) {
      next = Mode::Tx;
    } else if (strcmp(argv[1], "rx") == 0) {
      next = Mode::Rx;
    } else if (strcmp(argv[1], "duplex") == 0) {
      next = Mode::Duplex;
    } else {
      Serial.println(F("Unknown mode."));
      return;
    }
    gFixedPwmEnabled = false;
    resetBuffers();
    gMode = next;
    Serial.print(F("Mode set to "));
    Serial.println(modeName(gMode));
      if (gMode == Mode::Tx) {
        Serial.println(F("TX mode. Press button to transmit."));
      }
    if (gMode == Mode::Rx) {
      Serial.println(F("RX mode. Ready to receive audio."));
    }
    if (gMode == Mode::Tx || gMode == Mode::Rx) {
      Serial.println(F("Use PC bridge with matching --baud and --serial-rate."));
    }
    return;
  }

  if (strcmp(argv[0], "bt") == 0) {
    if (argc < 2) {
      Serial.println(F("Usage: bt AT or bt AT+UART?"));
      return;
    }
    while (gBtAtSerial.available() > 0) {
      gBtAtSerial.read();
    }
    Serial.println(F("BT command begin"));
    gBtAtSerial.print(argv[1]);
    gBtAtSerial.print("\r\n");
    const uint32_t deadline = millis() + 1000UL;
    bool gotResponse = false;
    while (millis() < deadline) {
      while (gBtAtSerial.available() > 0) {
        const int value = gBtAtSerial.read();
        if (value < 0) {
          break;
        }
        gotResponse = true;
        Serial.write(static_cast<uint8_t>(value));
      }
    }
    if (!gotResponse) {
      Serial.println(F("(no response)"));
    }
    Serial.println(F("\nBT command end"));
    return;
  }

  if (strcmp(argv[0], "rxgain") == 0) {
    if (argc == 1) {
      Serial.print(F("RX gain: "));
      Serial.println(gRxAudioGainVar);
      return;
    }
    uint8_t value = 0;
    if (!parseU8(argv[1], value) || value == 0) {
      Serial.println(F("Usage: rxgain <1-255>"));
      return;
    }
    gRxAudioGainVar = value;
    Serial.print(F("RX gain set to "));
    Serial.println(value);
    return;
  }
  
  Serial.println(F("Unknown command. Type 'help'."));
}

void handleCommandInput(char c) {
  if (c == '\r' || c == '\n') {
    Serial.println();
    gLineBuf[gLineLen] = '\0';
    handleCommand(gLineBuf);
    gLineLen = 0;
    Serial.print(F("walkie> "));
    return;
  }
  if (c == '\b' || c == 127) {
    if (gLineLen > 0) {
      --gLineLen;
      Serial.print(F("\b \b"));
    }
    return;
  }
  if (isprint(static_cast<unsigned char>(c)) && gLineLen < (kLineBufSize - 1U)) {
    gLineBuf[gLineLen++] = c;
    Serial.write(c);
  }
}

void pollSerial() {
  const uint32_t now = millis();
  bool dropDuplexRx = false;
  if (gMode == Mode::Duplex) {
    dropDuplexRx = gPttPressed || (static_cast<int32_t>(gDuplexRxDropUntilMs - now) > 0);
  }

  if (gMode == Mode::Rx || gMode == Mode::Duplex) {
    while (Serial.available() > 0) {
      const int value = Serial.read();
      if (value < 0) {
        break;
      }
      // in duplex, in fereastra de tx arunc datele RX ca sa nu existe replay
      if (gMode == Mode::Duplex && dropDuplexRx) {
        continue;
      }
      if (rxPush(static_cast<uint8_t>(value))) {
        gRxActivityMs = now;
      }
    }
  } else if (gMode == Mode::Tx) {
    while (Serial.available() > 0) {
      Serial.read();
    }
  } else {
    while (Serial.available() > 0) {
      const int value = Serial.read();
      if (value < 0) {
        break;
      }
      handleCommandInput(static_cast<char>(value));
    }
  }

  // tx doar cand butonul e apasat
  if (gMode == Mode::Tx || (gMode == Mode::Duplex && gPttPressed)) {
    while (Serial.availableForWrite() > 0) {
      uint8_t value = 0;
      if (!txPop(value)) {
        break;
      }
      Serial.write(value);
    }
  }
}

void pollPtt() {
  static bool lastPressed = false;
  bool isPressed = false;
  if (gMode == Mode::Tx || gMode == Mode::Duplex) {
    isPressed = readPttButton();
  }

  // In Duplex, cat timp butonul e apasat, tin RX gol ca sa nu existe backlog.
  // La prima apasare opresc instant redarea.
  if (gMode == Mode::Duplex) {
    if (isPressed) {
      noInterrupts();
      gRxHead = gRxTail;
      gRxPlaybackSample = kSilence;
      interrupts();
    } else if (lastPressed) {
      // dupa release mai arunc un pic RX ca sa scap de backlog din BT/UART
      gDuplexRxDropUntilMs = millis() + kDuplexRxFlushMs;
      noInterrupts();
      gRxHead = gRxTail;
      gRxPlaybackSample = kSilence;
      interrupts();
    }
  }

  gPttPressed = isPressed;
  lastPressed = isPressed;
}

}

ISR(ADC_vect) {
  const uint8_t sample = ADCH;
  gLastAdc = sample;

  if (gMicStatsEnabled) {
    ++gMicStatsCount;
    gMicStatsSum += sample;
    gMicStatsAbsSum += (sample >= kSilence) ? (sample - kSilence) : (kSilence - sample);
    if (sample < gMicStatsMin) {
      gMicStatsMin = sample;
    }
    if (sample > gMicStatsMax) {
      gMicStatsMax = sample;
    }
    if (sample <= kClipLow && gMicStatsClipsLow < 0xFFFFU) {
      ++gMicStatsClipsLow;
    }
    if (sample >= kClipHigh && gMicStatsClipsHigh < 0xFFFFU) {
      ++gMicStatsClipsHigh;
    }
    const bool aboveMid = (sample >= kSilence);
    if (aboveMid != gMicStatsPrevAboveMid && gMicStatsCrossings < 0xFFFFU) {
      ++gMicStatsCrossings;
    }
    gMicStatsPrevAboveMid = aboveMid;
  }

  const Mode mode = gMode;
  uint8_t out = kSilence;
  bool serialAudioSlot = false;

  ++gSerialAudioTick;
  if (gSerialAudioTick >= kSerialAudioDivider) {
    gSerialAudioTick = 0;
    serialAudioSlot = true;
  }

  // tx trimit doar cand butonul e apasat
  if (serialAudioSlot && (mode == Mode::Tx || mode == Mode::Duplex)) {
    if (gPttPressed) {
      txPush(cleanMicSample(sample));
      ++gTxSamplesSent;
    }
  }

  // in rx mode mereu playback
  // duplex playback doar cand nu e butonul apasat
  if (serialAudioSlot && (mode == Mode::Rx || mode == Mode::Duplex)) {
    if (!(mode == Mode::Duplex && gPttPressed)) {
      uint8_t value = kSilence;
      if (rxPop(value)) {
        gRxPlaybackSample = applyAudioGain(value, static_cast<uint8_t>(gRxAudioGainVar));
        ++gRxSamplesPlayed;
      }
    } else {
      // cat timp transmit in duplex, redarea ramane muta
      gRxPlaybackSample = kSilence;
    }
  }

  // in duplex + buton apasat: speaker mut
  if (mode == Mode::Duplex && gPttPressed) {
    out = kSilence;
  } else if (mode == Mode::Rx || (mode == Mode::Duplex && !gPttPressed)) {
    // playback cand suntem in rx sau duplex fara buton
    out = gRxPlaybackSample;
  } else if (gFixedPwmEnabled) {
    out = gFixedPwmValue;
  }

  OCR1A = out;
}

void setup() {
  Serial.begin(UART_BAUD);
  gBtAtSerial.begin(BT_AT_BAUD);
  setupPtt();
  delay(50);
  setupPwm();
  setupAdc();
  // led pe D13 la activitate rx
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  gMode = bootMode();
  resetBuffers();
  // intra in shell daca nu sunt in tx sau rx imediat
  if (gMode != Mode::Tx && gMode != Mode::Rx) {
    Serial.println(F("\nWalkie Talkie firmware"));
    Serial.println(F("Type 'help' for commands."));
    Serial.print(F("walkie> "));
  }
}

void loop() {
  pollPtt();
  pollSerial();
  // arata activitate rx pe led D13
  {
    const uint32_t now = millis();
    if (gMode == Mode::Rx && (now - gRxActivityMs) < 200UL) {
      digitalWrite(13, HIGH);
    } else {
      digitalWrite(13, LOW);
    }
  }
}