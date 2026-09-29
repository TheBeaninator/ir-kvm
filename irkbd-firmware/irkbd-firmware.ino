// irkbd-firmware — IR-bus USB keyboard for Seeed XIAO ESP32S3.
// A CH0038K (38 kHz IR receiver, active-low OUT) on pin IR_PIN; the board enumerates as a
// USB HID keyboard on the machine it is plugged into and types what the control node
// blasts at it over IR (see ~/ir-blaster/irkbd.py for the frame format).
//
// Frame: NEC timing, bytes LSB-first: dst, type<<4|seq, payload..., crc8(poly 0x07) over the rest.
//   dst 0 = broadcast, else must equal this node's id.
//   type 0 chord, 1 press/hold, 2 release all: payload = mods, keycode  (5-byte frame)
//   type 3 string: payload = n, then n bytes of (HID key | shift<<7)  (n+4 bytes)
// Node id is stored in NVS; set it over the USB serial port:  "id 3"  (also "id?" and "help").
// Build: arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default  (USB-OTG/TinyUSB, CDC on)
#include <Preferences.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

static const int IR_PIN = D10;         // GPIO9 on XIAO ESP32S3, the pad right below 3V3
static const int LED_PIN = LED_BUILTIN; // GPIO21, active low
static const size_t MAX_FRAME = 48;    // dst, type|seq, n, up to 44 payload, crc
static const size_t MAX_EDGES = 1024;  // ring of edge durations; a 44-byte frame is ~700 edges

USBHIDKeyboard Keyboard;
Preferences prefs;
static uint8_t nodeId = 0;

// ---- edge capture (ISR fills a ring of edge durations) ----
volatile uint32_t edgeUs[MAX_EDGES];
volatile uint8_t edgeLevel[MAX_EDGES];   // level that *ended* with this edge (0 = mark, since OUT is active low)
volatile uint16_t edgeHead = 0, edgeTail = 0;
volatile uint32_t lastEdge = 0;
volatile uint32_t edgeCount = 0;
static uint32_t framesOk = 0, crcErrors = 0;
static bool rawDump = false;

void IRAM_ATTR onEdge() {
  uint32_t now = micros();
  uint32_t dur = now - lastEdge;
  lastEdge = now;
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
  for (int i = 0; i < 8; i++) if (mods & (1 << i)) Keyboard.pressRaw(0xE0 + i);
  if (key) Keyboard.pressRaw(key);
  delay(4);
  Keyboard.releaseAll();
  delay(2);
}

static void handleFrame() {
  if (crc8(frame, frameLen - 1) != frame[frameLen - 1]) { crcErrors++; Serial.println("rx: bad crc"); return; }
  framesOk++;
  uint8_t dst = frame[0], type = frame[1] >> 4, seq = frame[1] & 0xF;
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
      case 1: for (int i = 0; i < 8; i++) if (mods & (1 << i)) Keyboard.pressRaw(0xE0 + i); if (key) Keyboard.pressRaw(key); break;
      case 2: Keyboard.releaseAll(); break;
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

static void serialCommand(String line) {
  line.trim();
  if (line.startsWith("id ")) {
    int v = line.substring(3).toInt();
    if (v < 1 || v > 254) { Serial.println("id must be 1..254"); return; }
    nodeId = v; prefs.putUChar("id", nodeId);
    Serial.printf("node id = %u (saved)\n", nodeId);
  } else if (line == "id?" || line == "id") {
    Serial.printf("node id = %u\n", nodeId);
  } else if (line == "stat") {
    Serial.printf("pin=%d (idle should be 1) edges=%lu frames=%lu crcerr=%lu state=%d\n", digitalRead(IR_PIN), (unsigned long)edgeCount, (unsigned long)framesOk, (unsigned long)crcErrors, (int)state);
  } else if (line == "raw on" || line == "raw off") {
    rawDump = line.endsWith("on"); Serial.printf("raw dump %s\n", rawDump ? "on" : "off");
  } else if (line == "help") {
    Serial.println("irkbd: id N | id? | stat | raw on|off | help");
  } else if (line.length()) {
    Serial.println("? (try help)");
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(IR_PIN, INPUT_PULLUP);
  prefs.begin("irkbd", false);
  nodeId = prefs.getUChar("id", 0);
  Keyboard.begin();
  USB.begin();
  Serial.begin(115200);
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
