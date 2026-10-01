// irkbd-firmware — IR-bus USB keyboard for Seeed XIAO RP2040 (no radio), XIAO ESP32S3, and
// ATmega32U4 boards (DFRobot Beetle, Arduino Micro/Leonardo, Pro Micro).
// A CH0038K (38 kHz IR receiver, active-low OUT) on pin IR_PIN; the board enumerates as a
// USB HID keyboard on the machine it is plugged into and types what the control node
// blasts at it over IR (see irkbd.py in the repo root for the frame format).
//
// Frame: NEC timing, bytes LSB-first: dst, type<<4|seq, payload..., crc8(poly 0x07) over the rest.
//   dst 0 = broadcast, else must equal this node's id.
//   type 0 chord, 1 press/hold, 2 release all: payload = mods, keycode  (5-byte frame)
//   type 3 string: payload = n, then n bytes of (HID key | shift<<7)  (n+4 bytes)
// Node id is stored in flash; set it over the USB serial port:  "id 3"  (also "id?", "stat", "help").
// Id 255 = blaster-only node (plugged into the control machine): it never types, it only transmits.
// RP2040 and 32U4 boards can also TRANSMIT through an IR LED on D9 (38 kHz PWM carrier), so a node
// on the control machine replaces the Tiqiaa dongle. Serial commands, each answered with "ok":
//   irkb <hex bytes>      send an IRKB frame (fast timing); the host supplies dst..crc
//   necb <hex bytes>      same bytes with classic NEC timing (what the HDMI switch wants)
//   nec <addr> <cmd>      send a classic NEC frame (e.g. the HDMI switch: nec 0x80 0x05)
//   raw +mark -space ...  send arbitrary pulse/space microseconds
//   ping                  -> "irkvm <board> id=<n> tx=<0|1>"
// Build (see README):
//   RP2040:  arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2040:usbstack=tinyusb
//   ESP32S3: arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default
//   Beetle:  arduino-cli compile --fqbn arduino:avr:micro
//
// ---- platform layer: USB HID keyboard + persistent node id + optional IR LED ----
#if defined(ARDUINO_ARCH_RP2040)
  #include <Adafruit_TinyUSB.h>
  #include <EEPROM.h>
  #define IRAM_ATTR
  #define BOARD_NAME "rp2040"
  static const int IR_PIN = D10;          // GPIO3 on XIAO RP2040, the pad right below 3V3
  static const int LED_PIN = LED_BUILTIN; // active low
  #define LED_ON LOW
  #define LED_OFF HIGH
  static const size_t MAX_EDGES = 1024;   // ring of edge durations; a 44-byte frame is ~700 edges
  static const size_t CMD_LINE_MAX = 512;
  static const size_t RAW_MAX = 2 + 8 * 2 * 48 + 2;
  #define SPRINTF(...) Serial.printf(__VA_ARGS__)
  static uint8_t const hidDesc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
  Adafruit_USBD_HID usbHid(hidDesc, sizeof(hidDesc), HID_ITF_PROTOCOL_KEYBOARD, 2, false);
  static uint8_t kbMods = 0, kbKeys[6] = {0};
  static void kbReport() { while (!usbHid.ready()) delay(1); usbHid.keyboardReport(0, kbMods, kbKeys); }
  static void kbPressRaw(uint8_t k) {
    if (k >= 0xE0 && k < 0xE8) { kbMods |= 1 << (k - 0xE0); kbReport(); return; }
    for (int i = 0; i < 6; i++) if (kbKeys[i] == k) return;
    for (int i = 0; i < 6; i++) if (!kbKeys[i]) { kbKeys[i] = k; break; }
    kbReport();
  }
  static void kbReleaseAll() { kbMods = 0; memset(kbKeys, 0, sizeof kbKeys); kbReport(); }
  static void kbBegin() { usbHid.begin(); }
  static void kbReady() { while (!TinyUSBDevice.mounted()) delay(1); }
  static uint8_t loadNodeId() { EEPROM.begin(256); return EEPROM.read(1) == 0xA5 ? EEPROM.read(0) : 0; }
  static void saveNodeId(uint8_t id) { EEPROM.write(0, id); EEPROM.write(1, 0xA5); EEPROM.commit(); }
  // ---- IR transmit: 38 kHz carrier on IR_TX_PIN via a PWM slice, gated per mark/space ----
  #include "hardware/pwm.h"
  #include "hardware/clocks.h"
  #define HAS_IR_TX 1
  static const int IR_TX_PIN = D9;        // GPIO4 on XIAO RP2040, the pad right below D10
  static uint16_t txWrap = 0, txLevel = 0;
  static void txBegin() {
    gpio_set_function(IR_TX_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(IR_TX_PIN);
    txWrap = clock_get_hz(clk_sys) / 38000 - 1;   // 125 MHz -> 3288 : 38.0 kHz
    txLevel = txWrap / 3;                          // ~33 % duty
    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, txWrap);
    pwm_init(slice, &c, true);
    pwm_set_gpio_level(IR_TX_PIN, 0);
  }
  static inline void txMark(bool on) { pwm_set_gpio_level(IR_TX_PIN, on ? txLevel : 0); }
  static void rxAttach(void (*isr)()) { attachInterrupt(digitalPinToInterrupt(IR_PIN), isr, CHANGE); }
#elif defined(ARDUINO_ARCH_ESP32)
  #include <Preferences.h>
  #include "USB.h"
  #include "USBHIDKeyboard.h"
  #define BOARD_NAME "esp32s3"
  static const int IR_PIN = D10;          // GPIO9 on XIAO ESP32S3, the pad right below 3V3
  static const int LED_PIN = LED_BUILTIN; // GPIO21, active low
  #define LED_ON LOW
  #define LED_OFF HIGH
  static const size_t MAX_EDGES = 1024;
  static const size_t CMD_LINE_MAX = 512;
  static const size_t RAW_MAX = 2 + 8 * 2 * 48 + 2;
  #define SPRINTF(...) Serial.printf(__VA_ARGS__)
  USBHIDKeyboard Keyboard;
  Preferences prefs;
  static void kbPressRaw(uint8_t k) { Keyboard.pressRaw(k); }
  static void kbReleaseAll() { Keyboard.releaseAll(); }
  static void kbBegin() { Keyboard.begin(); USB.begin(); }
  static void kbReady() {}
  static uint8_t loadNodeId() { prefs.begin("irkbd", false); return prefs.getUChar("id", 0); }
  static void saveNodeId(uint8_t id) { prefs.putUChar("id", id); }
  #define HAS_IR_TX 0
  static void txBegin() {}
  static inline void txMark(bool) {}
  static void rxAttach(void (*isr)()) { attachInterrupt(digitalPinToInterrupt(IR_PIN), isr, CHANGE); }
#elif defined(ARDUINO_ARCH_AVR) && defined(__AVR_ATmega32U4__)
  // DFRobot Beetle / Arduino Micro / Leonardo / Pro Micro: 16 MHz, 2.5 KB RAM, native USB.
  // Same wiring as the XIAOs: receiver OUT -> D10, IR LED driver -> D9. Both are broken out on the Beetle.
  #include <EEPROM.h>
  #include <HID.h>
  #define IRAM_ATTR
  #define BOARD_NAME "avr32u4"
  static const int IR_PIN = 10;           // PB6 / PCINT6 (no INTx on this pin: we use a pin-change interrupt)
  static const int LED_PIN = LED_BUILTIN; // D13, active HIGH on the Beetle / Micro
  #define LED_ON HIGH
  #define LED_OFF LOW
  static const size_t MAX_EDGES = 256;    // 512 bytes; loop() drains it continuously, a 20-byte frame is ~330 edges
  static const size_t CMD_LINE_MAX = 112;     // irkb/necb of a 20-byte string frame is 45 chars; raw lines don't fit here
  static const size_t RAW_MAX = 40;       // 'raw' on this board only fits short bursts: use nec/necb instead
  #define SPRINTF(...) do { char _b[100]; snprintf(_b, sizeof _b, __VA_ARGS__); Serial.print(_b); } while (0)
  // Raw keyboard report (report id 2, the same layout the Arduino Keyboard library uses).
  static const uint8_t hidDesc[] PROGMEM = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x73, 0x05, 0x07, 0x19, 0x00, 0x29, 0x73, 0x81, 0x00,
    0xC0 };
  static uint8_t kbMods = 0, kbKeys[6] = {0};
  static void kbReport() { uint8_t r[8] = { kbMods, 0, kbKeys[0], kbKeys[1], kbKeys[2], kbKeys[3], kbKeys[4], kbKeys[5] }; HID().SendReport(2, r, sizeof r); }
  static void kbPressRaw(uint8_t k) {
    if (k >= 0xE0 && k < 0xE8) { kbMods |= 1 << (k - 0xE0); kbReport(); return; }
    for (int i = 0; i < 6; i++) if (kbKeys[i] == k) return;
    for (int i = 0; i < 6; i++) if (!kbKeys[i]) { kbKeys[i] = k; break; }
    kbReport();
  }
  static void kbReleaseAll() { kbMods = 0; memset(kbKeys, 0, sizeof kbKeys); kbReport(); }
  static void kbBegin() { static HIDSubDescriptor node(hidDesc, sizeof hidDesc); HID().AppendDescriptor(&node); }
  static void kbReady() {}
  static uint8_t loadNodeId() { return EEPROM.read(1) == 0xA5 ? EEPROM.read(0) : 0; }
  static void saveNodeId(uint8_t id) { EEPROM.update(0, id); EEPROM.update(1, 0xA5); }
  // ---- IR transmit: Timer1 fast PWM on OC1A (= D9), 38 kHz, ~33 % duty; gated by connecting/disconnecting OC1A ----
  #define HAS_IR_TX 1
  static const int IR_TX_PIN = 9;
  static void txBegin() {
    pinMode(IR_TX_PIN, OUTPUT); digitalWrite(IR_TX_PIN, LOW);
    ICR1 = F_CPU / 38000 - 1;            // 420 -> 38.0 kHz
    OCR1A = ICR1 / 3;
    TCCR1A = _BV(WGM11);                 // fast PWM, TOP = ICR1, OC1A disconnected (pin low)
    TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);
  }
  static inline void txMark(bool on) { if (on) TCCR1A |= _BV(COM1A1); else { TCCR1A &= ~_BV(COM1A1); PORTB &= ~_BV(PB5); } }
  static void (*rxIsr)();
  ISR(PCINT0_vect) { rxIsr(); }
  static void rxAttach(void (*isr)()) { rxIsr = isr; PCMSK0 = _BV(PCINT6); PCIFR = _BV(PCIF0); PCICR |= _BV(PCIE0); }
#else
  #error "unsupported board: build for seeed_xiao_rp2040, XIAO_ESP32S3 or an ATmega32U4 (arduino:avr:micro)"
#endif
static const size_t MAX_FRAME = 48;    // dst, type|seq, n, up to 44 payload, crc

static uint8_t nodeId = 0;

// ---- edge capture (ISR fills a ring of edge durations) ----
// each entry: duration in us (clamped to 32 ms) with bit 15 = level that *ended* with this edge
// (1 = pin went high, i.e. a mark just ended, since the receiver's OUT is active low)
volatile uint16_t edges[MAX_EDGES];
volatile uint16_t edgeHead = 0, edgeTail = 0;
volatile uint32_t lastEdge = 0;
volatile uint32_t edgeCount = 0;
static volatile bool txBusy = false;
static uint32_t framesOk = 0, crcErrors = 0;
static bool rawDump = false;

void IRAM_ATTR onEdge() {
  uint32_t now = micros();
  uint32_t dur = now - lastEdge;
  lastEdge = now;
  if (txBusy) return;                   // our own LED, not the bus
  edgeCount++;
  uint16_t next = (edgeHead + 1) % MAX_EDGES;
  if (next == edgeTail) return;          // overflow: drop
  if (dur > 0x7FFF) dur = 0x7FFF;
  edges[edgeHead] = (uint16_t)dur | (digitalRead(IR_PIN) ? 0x8000 : 0);
  edgeHead = next;
}

static bool popEdge(uint32_t &dur, bool &wasMark) {
  if (edgeTail == edgeHead) return false;
  uint16_t e = edges[edgeTail];
  dur = e & 0x7FFF; wasMark = e & 0x8000;
  edgeTail = (edgeTail + 1) % MAX_EDGES;
  return true;
}

static inline bool near(uint32_t v, uint32_t target, uint32_t tol) { return v + tol >= target && v <= target + tol; }

// ---- NEC-style decoder state machine ----
enum { S_IDLE, S_LEAD_SPACE, S_BIT_MARK, S_BIT_SPACE } state = S_IDLE;
static uint8_t frame[MAX_FRAME];
static uint16_t bitCount = 0;
static uint16_t frameLen = 5;          // bytes expected for the current frame (known once header is in)
// bit timing chosen per frame from the lead space: 4500 us = NEC (560/560/1690), 2250 us = fast (350/350/1000)
static uint16_t tMark = 560, tMarkTol = 300, tSpace0 = 560, tSpace1 = 1690, tSpaceTol = 300;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x07 : c << 1; }
  return c;
}

static uint8_t lastSeq = 0xFF;

static void tapKey(uint8_t mods, uint8_t key) {
  for (int i = 0; i < 8; i++) if (mods & (1 << i)) kbPressRaw(0xE0 + i);
  if (key) kbPressRaw(key);
  delay(4);
  kbReleaseAll();
  delay(2);
}

static void handleFrame() {
  if (crc8(frame, frameLen - 1) != frame[frameLen - 1]) { crcErrors++; Serial.println(F("rx: bad crc")); return; }
  framesOk++;
  uint8_t dst = frame[0], type = frame[1] >> 4, seq = frame[1] & 0xF;
  if (nodeId == 255) return;                             // blaster-only node: never type on the host
  if (dst != 0 && dst != nodeId) return;               // not for us
  if (seq == lastSeq) return;                            // duplicate (sender may transmit a frame twice)
  lastSeq = seq;
  digitalWrite(LED_PIN, LED_ON);
  if (type == 3) {
    uint8_t n = frame[2];
    SPRINTF("rx: dst=%u string seq=%u n=%u\n", dst, seq, n);
    for (uint8_t i = 0; i < n; i++) tapKey((frame[3 + i] & 0x80) ? 0x02 : 0, frame[3 + i] & 0x7F);
  } else {
    uint8_t mods = frame[2], key = frame[3];
    SPRINTF("rx: dst=%u type=%u seq=%u mods=%02X key=%02X\n", dst, type, seq, mods, key);
    switch (type) {
      case 0: tapKey(mods, key); break;
      case 1: for (int i = 0; i < 8; i++) if (mods & (1 << i)) kbPressRaw(0xE0 + i); if (key) kbPressRaw(key); break;
      case 2: kbReleaseAll(); break;
      default: break;
    }
  }
  digitalWrite(LED_PIN, LED_OFF);
}

static void feed2(uint32_t dur, bool wasMark);
static void feed(uint32_t dur, bool wasMark) {
  if (rawDump && dur < 50000) SPRINTF("%c%lu ", wasMark ? '+' : '-', (unsigned long)dur);
  int before = state;
  feed2(dur, wasMark);
  if (rawDump && before != state) SPRINTF("[%d->%d bits=%u]", before, (int)state, (unsigned)bitCount);
}

static void feed2(uint32_t dur, bool wasMark) {
  switch (state) {
    case S_IDLE:
      if (wasMark && near(dur, 9000, 2000)) state = S_LEAD_SPACE;
      break;
    case S_LEAD_SPACE:
      if (wasMark) { state = S_IDLE; break; }
      if (near(dur, 4500, 700))      { tMark = 560; tMarkTol = 300; tSpace0 = 560; tSpace1 = 1690; tSpaceTol = 300; }
      else if (near(dur, 2250, 500)) { tMark = 350; tMarkTol = 200; tSpace0 = 350; tSpace1 = 1000; tSpaceTol = 200; }
      else { state = S_IDLE; break; }
      state = S_BIT_MARK; bitCount = 0; frameLen = 5; memset(frame, 0, sizeof frame);
      break;
    case S_BIT_MARK:
      if (wasMark && near(dur, tMark, tMarkTol)) state = S_BIT_SPACE; else state = S_IDLE;
      break;
    case S_BIT_SPACE:
      if (wasMark) { state = S_IDLE; break; }
      if (near(dur, tSpace0, tSpaceTol)) { /* 0 */ }
      else if (near(dur, tSpace1, tSpaceTol + 200)) frame[bitCount / 8] |= 1 << (bitCount % 8);
      else { state = S_IDLE; break; }
      bitCount++;
      if (bitCount == 24 && (frame[1] >> 4) == 3) {          // string frame: length now known
        uint16_t n = frame[2];
        if (n == 0 || n > MAX_FRAME - 4) { state = S_IDLE; break; }
        frameLen = 4 + n;
      }
      if (bitCount == frameLen * 8) { state = S_IDLE; handleFrame(); }
      else state = S_BIT_MARK;
      break;
  }
}

// ---- transmit (pulses are generated on the fly; nothing is buffered) ----
static inline void txPulse(bool mark, uint32_t us) {
  txMark(mark);
  while (us > 10000) { delayMicroseconds(10000); us -= 10000; }   // AVR delayMicroseconds() tops out at 16383
  delayMicroseconds(us);
}
static void txStart() { txBusy = true; }
static void txEnd() {
  txMark(false);
  delay(2);
  edgeHead = edgeTail = 0; state = S_IDLE;  // discard whatever our own LED bounced into the receiver
  txBusy = false;
}
static void txCodes(const int16_t *codes, size_t n) {   // +mark / -space microseconds
  txStart();
  for (size_t i = 0; i < n; i++) txPulse(codes[i] > 0, codes[i] > 0 ? codes[i] : -codes[i]);
  txEnd();
}
static void txBytes(const uint8_t *b, size_t n, bool fast) {
  const uint16_t mark = fast ? 350 : 560, sp0 = fast ? 350 : 560, sp1 = fast ? 1000 : 1690;
  txStart();
  txPulse(true, 9000); txPulse(false, fast ? 2250 : 4500);
  for (size_t i = 0; i < n && i < MAX_FRAME; i++)
    for (int bit = 0; bit < 8; bit++) { txPulse(true, mark); txPulse(false, (b[i] >> bit) & 1 ? sp1 : sp0); }
  txPulse(true, mark); txPulse(false, 5000);
  txEnd();
}
static int hexNibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static bool startsWith(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static void serialCommand(char *line) {
  while (*line == ' ') line++;
  for (size_t n = strlen(line); n && line[n - 1] == ' '; n--) line[n - 1] = 0;
  if (!strcmp(line, "ping")) { SPRINTF("irkvm " BOARD_NAME " id=%u tx=%d\n", nodeId, HAS_IR_TX); return; }
  if (!strcmp(line, "raw on") || !strcmp(line, "raw off")) {   // before the "raw <pulses>" transmit branch
    rawDump = line[4] == 'o' && line[5] == 'n'; SPRINTF("raw dump %s\n", rawDump ? "on" : "off"); return;
  }
  if (startsWith(line, "irkb ") || startsWith(line, "necb ") || startsWith(line, "nec ") || startsWith(line, "raw ")) {
    if (!HAS_IR_TX) { Serial.println(F("err no IR LED on this board")); return; }
    if (startsWith(line, "irkb ") || startsWith(line, "necb ")) {
      uint8_t b[MAX_FRAME]; size_t n = 0; int hi = -1;
      for (const char *p = line + 5; *p && n < MAX_FRAME; p++) {
        int v = hexNibble(*p); if (v < 0) continue;
        if (hi < 0) hi = v; else { b[n++] = (hi << 4) | v; hi = -1; }
      }
      if (n < 3) { Serial.println(F("err short frame")); return; }
      txBytes(b, n, line[0] == 'i');
    } else if (startsWith(line, "nec ")) {
      long addr = strtol(line + 4, nullptr, 0);
      const char *sp = strchr(line + 4, ' ');
      if (!sp) { Serial.println(F("err usage: nec <addr> <cmd>")); return; }
      long cmd = strtol(sp + 1, nullptr, 0);
      uint8_t b[4] = { (uint8_t)addr, (uint8_t)~addr, (uint8_t)cmd, (uint8_t)~cmd };
      txBytes(b, 4, false);
    } else {
      static int16_t codes[RAW_MAX]; size_t k = 0;
      const char *p = line + 4;
      while (*p && k < RAW_MAX) {
        char *end; long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v) codes[k++] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
        p = end;
      }
      if (!k) { Serial.println(F("err usage: raw +mark -space ...")); return; }
      txCodes(codes, k);
    }
    Serial.println(F("ok"));
    return;
  }
  if (startsWith(line, "id ")) {
    int v = atoi(line + 3);
    if (v < 1 || v > 255) { Serial.println(F("id must be 1..254, or 255 = blaster only (never types)")); return; }
    nodeId = v; saveNodeId(nodeId);
    SPRINTF("node id = %u (saved)\n", nodeId);
  } else if (!strcmp(line, "id?") || !strcmp(line, "id")) {
    SPRINTF("node id = %u\n", nodeId);
  } else if (!strcmp(line, "stat")) {
    SPRINTF("pin=%d (idle should be 1) edges=%lu frames=%lu crcerr=%lu state=%d\n", digitalRead(IR_PIN), (unsigned long)edgeCount, (unsigned long)framesOk, (unsigned long)crcErrors, (int)state);
  } else if (!strcmp(line, "help")) {
    Serial.println(F("irkbd: id N | id? | stat | raw on|off | ping | irkb <hex> | necb <hex> | nec <addr> <cmd> | raw <+us -us ...> | help"));
  } else if (*line) {
    Serial.println(F("? (try help)"));
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LED_OFF);
  pinMode(IR_PIN, INPUT_PULLUP);
  nodeId = loadNodeId();
  kbBegin();
  Serial.begin(115200);
  kbReady();
  txBegin();
  lastEdge = micros();
  rxAttach(onEdge);
}

void loop() {
  uint32_t dur; bool wasMark;
  while (popEdge(dur, wasMark)) feed(dur, wasMark);
  // a long quiet period resets a half-decoded frame
  // signed compare: the ISR can bump lastEdge between the two reads, which would wrap an unsigned difference
  if (state != S_IDLE && (int32_t)(micros() - lastEdge) > 20000) { if (rawDump) SPRINTF("[timeout st=%d bits=%u]\n", (int)state, (unsigned)bitCount); state = S_IDLE; }
  static char line[CMD_LINE_MAX + 1]; static size_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { line[len] = 0; serialCommand(line); len = 0; } else if (len < CMD_LINE_MAX) line[len++] = c;
  }
}
