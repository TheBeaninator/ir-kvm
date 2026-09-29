// irkbd-firmware — IR-bus USB keyboard for Seeed XIAO RP2040 (no radio) and XIAO ESP32S3.
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
// RP2040 boards can also TRANSMIT through an IR LED on D9 (38 kHz PWM carrier), so a node on the
// control machine replaces the Tiqiaa dongle. Serial commands, each answered with "ok":
//   irkb <hex bytes>      send an IRKB frame (fast timing); the host supplies dst..crc
//   nec <addr> <cmd>      send a classic NEC frame (e.g. the HDMI switch: nec 0x80 0x05)
//   raw +mark -space ...  send arbitrary pulse/space microseconds
//   ping                  -> "irkvm <board> id=<n> tx=<0|1>"
// Build (see README):
//   RP2040: arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2040:usbstack=tinyusb
//   ESP32S3: arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default
//
// ---- platform layer: USB HID keyboard + persistent node id ----
#if defined(ARDUINO_ARCH_RP2040)
  #include <Adafruit_TinyUSB.h>
  #include <EEPROM.h>
  #define IRAM_ATTR
  static const int IR_PIN = D10;          // GPIO3 on XIAO RP2040, the pad right below 3V3
  static const int LED_PIN = LED_BUILTIN; // active low
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
#elif defined(ARDUINO_ARCH_ESP32)
  #include <Preferences.h>
  #include "USB.h"
  #include "USBHIDKeyboard.h"
  static const int IR_PIN = D10;          // GPIO9 on XIAO ESP32S3, the pad right below 3V3
  static const int LED_PIN = LED_BUILTIN; // GPIO21, active low
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
#else
  #error "unsupported board: build for seeed_xiao_rp2040 or XIAO_ESP32S3"
#endif
static const size_t MAX_FRAME = 48;    // dst, type|seq, n, up to 44 payload, crc
static const size_t MAX_EDGES = 1024;  // ring of edge durations; a 44-byte frame is ~700 edges

static uint8_t nodeId = 0;

// ---- edge capture (ISR fills a ring of edge durations) ----
volatile uint32_t edgeUs[MAX_EDGES];
volatile uint8_t edgeLevel[MAX_EDGES];   // level that *ended* with this edge (0 = mark, since OUT is active low)
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
  edgeUs[edgeHead] = dur;
  edgeLevel[edgeHead] = digitalRead(IR_PIN) ? 1 : 0;  // pin now high => a mark (low period) just ended
  edgeHead = next;
}

static bool popEdge(uint32_t &dur, bool &wasMark) {
  if (edgeTail == edgeHead) return false;
  dur = edgeUs[edgeTail]; wasMark = edgeLevel[edgeTail];
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
  if (crc8(frame, frameLen - 1) != frame[frameLen - 1]) { crcErrors++; Serial.println("rx: bad crc"); return; }
  framesOk++;
  uint8_t dst = frame[0], type = frame[1] >> 4, seq = frame[1] & 0xF;
  if (nodeId == 255) return;                             // blaster-only node: never type on the host
  if (dst != 0 && dst != nodeId) return;               // not for us
  if (seq == lastSeq) return;                            // duplicate (sender may transmit a frame twice)
  lastSeq = seq;
  digitalWrite(LED_PIN, LOW);
  if (type == 3) {
    uint8_t n = frame[2];
    Serial.printf("rx: dst=%u string seq=%u n=%u\n", dst, seq, n);
    for (uint8_t i = 0; i < n; i++) tapKey((frame[3 + i] & 0x80) ? 0x02 : 0, frame[3 + i] & 0x7F);
  } else {
    uint8_t mods = frame[2], key = frame[3];
    Serial.printf("rx: dst=%u type=%u seq=%u mods=%02X key=%02X\n", dst, type, seq, mods, key);
    switch (type) {
      case 0: tapKey(mods, key); break;
      case 1: for (int i = 0; i < 8; i++) if (mods & (1 << i)) kbPressRaw(0xE0 + i); if (key) kbPressRaw(key); break;
      case 2: kbReleaseAll(); break;
      default: break;
    }
  }
  digitalWrite(LED_PIN, HIGH);
}

static void feed2(uint32_t dur, bool wasMark);
static void feed(uint32_t dur, bool wasMark) {
  if (rawDump && dur < 50000) Serial.printf("%c%lu ", wasMark ? '+' : '-', (unsigned long)dur);
  int before = state;
  feed2(dur, wasMark);
  if (rawDump && before != state) Serial.printf("[%d->%d bits=%u]", before, (int)state, (unsigned)bitCount);
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

// ---- transmit ----
static void txCodes(const int32_t *codes, size_t n) {   // +mark / -space microseconds
  txBusy = true;
  for (size_t i = 0; i < n; i++) {
    txMark(codes[i] > 0);
    delayMicroseconds(codes[i] > 0 ? codes[i] : -codes[i]);
  }
  txMark(false);
  delay(2);
  edgeHead = edgeTail = 0; state = S_IDLE;  // discard whatever our own LED bounced into the receiver
  txBusy = false;
}
static void txBytes(const uint8_t *b, size_t n, bool fast) {
  static int32_t codes[2 + 8 * 2 * MAX_FRAME + 2];
  size_t k = 0;
  codes[k++] = 9000; codes[k++] = fast ? -2250 : -4500;
  const int32_t mark = fast ? 350 : 560, sp0 = fast ? -350 : -560, sp1 = fast ? -1000 : -1690;
  for (size_t i = 0; i < n && i < MAX_FRAME; i++)
    for (int bit = 0; bit < 8; bit++) { codes[k++] = mark; codes[k++] = (b[i] >> bit) & 1 ? sp1 : sp0; }
  codes[k++] = mark; codes[k++] = -5000;
  txCodes(codes, k);
}
static int hexNibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void serialCommand(String line) {
  line.trim();
  if (line == "ping") {
#if defined(ARDUINO_ARCH_RP2040)
    Serial.printf("irkvm rp2040 id=%u tx=%d\n", nodeId, HAS_IR_TX);
#else
    Serial.printf("irkvm esp32s3 id=%u tx=%d\n", nodeId, HAS_IR_TX);
#endif
    return;
  }
  if (line.startsWith("irkb ") || line.startsWith("nec ") || line.startsWith("raw ")) {
    if (!HAS_IR_TX) { Serial.println("err no IR LED on this board"); return; }
    if (line.startsWith("irkb ")) {
      uint8_t b[MAX_FRAME]; size_t n = 0; int hi = -1;
      for (unsigned i = 5; i < line.length() && n < MAX_FRAME; i++) {
        int v = hexNibble(line[i]); if (v < 0) continue;
        if (hi < 0) hi = v; else { b[n++] = (hi << 4) | v; hi = -1; }
      }
      if (n < 3) { Serial.println("err short frame"); return; }
      txBytes(b, n, true);
    } else if (line.startsWith("nec ")) {
      long addr = strtol(line.c_str() + 4, nullptr, 0);
      const char *sp = strchr(line.c_str() + 4, ' ');
      if (!sp) { Serial.println("err usage: nec <addr> <cmd>"); return; }
      long cmd = strtol(sp + 1, nullptr, 0);
      uint8_t b[4] = { (uint8_t)addr, (uint8_t)~addr, (uint8_t)cmd, (uint8_t)~cmd };
      txBytes(b, 4, false);
    } else {
      static int32_t codes[2 + 8 * 2 * MAX_FRAME + 2]; size_t k = 0;
      const char *p = line.c_str() + 4;
      while (*p && k < sizeof(codes) / sizeof(codes[0])) {
        char *end; long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v) codes[k++] = v;
        p = end;
      }
      if (!k) { Serial.println("err usage: raw +mark -space ..."); return; }
      txCodes(codes, k);
    }
    Serial.println("ok");
    return;
  }
  if (line.startsWith("id ")) {
    int v = line.substring(3).toInt();
    if (v < 1 || v > 255) { Serial.println("id must be 1..254, or 255 = blaster only (never types)"); return; }
    nodeId = v; saveNodeId(nodeId);
    Serial.printf("node id = %u (saved)\n", nodeId);
  } else if (line == "id?" || line == "id") {
    Serial.printf("node id = %u\n", nodeId);
  } else if (line == "stat") {
    Serial.printf("pin=%d (idle should be 1) edges=%lu frames=%lu crcerr=%lu state=%d\n", digitalRead(IR_PIN), (unsigned long)edgeCount, (unsigned long)framesOk, (unsigned long)crcErrors, (int)state);
  } else if (line == "raw on" || line == "raw off") {
    rawDump = line.endsWith("on"); Serial.printf("raw dump %s\n", rawDump ? "on" : "off");
  } else if (line == "help") {
    Serial.println("irkbd: id N | id? | stat | raw on|off | ping | irkb <hex> | nec <addr> <cmd> | raw <+us -us ...> | help");
  } else if (line.length()) {
    Serial.println("? (try help)");
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(IR_PIN, INPUT_PULLUP);
  nodeId = loadNodeId();
  kbBegin();
  Serial.begin(115200);
  kbReady();
  txBegin();
  lastEdge = micros();
  attachInterrupt(digitalPinToInterrupt(IR_PIN), onEdge, CHANGE);
}

void loop() {
  uint32_t dur; bool wasMark;
  while (popEdge(dur, wasMark)) feed(dur, wasMark);
  // a long quiet period resets a half-decoded frame
  // signed compare: the ISR can bump lastEdge between the two reads, which would wrap an unsigned difference
  if (state != S_IDLE && (int32_t)(micros() - lastEdge) > 20000) { if (rawDump) Serial.printf("[timeout st=%d bits=%u]\n", (int)state, (unsigned)bitCount); state = S_IDLE; }
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { serialCommand(line); line = ""; } else if (line.length() < 40) line += c;
  }
}
