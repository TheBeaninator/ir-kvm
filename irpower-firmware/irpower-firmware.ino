// irpower-firmware — IR-bus power node for the Seeed XIAO ESP32S3.
// Same CH0038K receiver on D10 and the same IRKB frame format as irkbd-firmware, but this node
// joins Wi-Fi and, on a frame of type 4 addressed to it, switches outlets on the user's Matter
// power strip through python-matter-server's WebSocket API (tokenless, LAN only). No cloud.
//
// Frame (NEC timing, bytes LSB-first): dst, 4<<4|seq, outlet, action, crc8(poly 0x07)
//   dst     this node's id (0 = broadcast)
//   outlet  Matter endpoint of the outlet, 1..N; 0 = every outlet 1..N
//   action  0 = off, 1 = on, 2 = toggle
// Every other frame type (keyboard traffic for other nodes) is ignored. Duplicate seq is dropped,
// so a sender may transmit each frame twice without toggling twice.
//
// Setup over the USB serial port (kept in flash, nothing baked into the binary):
//   ssid <name>            Wi-Fi network          pass <secret>        Wi-Fi password
//   server <host> [port]   matter-server (default port 5580)
//   matter <node> [N]      strip's Matter node id and outlet count (default 6)
//   id <n>                 this node's IR id       net                  Wi-Fi / server status
//   power <outlet> on|off|toggle   drive the strip now (local test)
//   ping | stat | raw on|off | help   as in irkbd-firmware
// Build: arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default

#include <WiFi.h>
#include <Preferences.h>
#include "mbedtls/base64.h"

struct Ws;                               // forward decl: Arduino hoists function prototypes above this point
#define BOARD_NAME "esp32s3-power"
static const int IR_PIN = D10;           // GPIO9 on XIAO ESP32S3, the pad right below 3V3
static const int LED_PIN = LED_BUILTIN;  // GPIO21, active low
static const size_t MAX_FRAME = 48;
static const size_t MAX_EDGES = 1024;
static const size_t CMD_LINE_MAX = 200;

Preferences prefs;
static uint8_t nodeId = 0;
static String cfgSsid, cfgPass, cfgHost;
static uint16_t cfgPort = 5580;
static uint16_t cfgMatterNode = 8;
static uint8_t cfgOutlets = 6;

// ---- edge capture (ISR fills a ring of edge durations) ----
volatile uint16_t edges[MAX_EDGES];      // duration us (clamped to 32 ms); bit 15 = a mark just ended
volatile uint16_t edgeHead = 0, edgeTail = 0;
volatile uint32_t lastEdge = 0, edgeCount = 0;
static uint32_t framesOk = 0, crcErrors = 0, powerCmds = 0, powerFails = 0;
static bool rawDump = false;

void IRAM_ATTR onEdge() {
  uint32_t now = micros(), dur = now - lastEdge;
  lastEdge = now; edgeCount++;
  uint16_t next = (edgeHead + 1) % MAX_EDGES;
  if (next == edgeTail) return;
  if (dur > 0x7FFF) dur = 0x7FFF;
  edges[edgeHead] = (uint16_t)dur | (digitalRead(IR_PIN) ? 0x8000 : 0);
  edgeHead = next;
}
static bool popEdge(uint32_t &dur, bool &wasMark) {
  if (edgeTail == edgeHead) return false;
  uint16_t e = edges[edgeTail]; dur = e & 0x7FFF; wasMark = e & 0x8000;
  edgeTail = (edgeTail + 1) % MAX_EDGES;
  return true;
}
static inline bool near(uint32_t v, uint32_t t, uint32_t tol) { return v + tol >= t && v <= t + tol; }

// ---- NEC-style decoder (identical to irkbd-firmware) ----
enum { S_IDLE, S_LEAD_SPACE, S_BIT_MARK, S_BIT_SPACE } state = S_IDLE;
static uint8_t frame[MAX_FRAME];
static uint16_t bitCount = 0, frameLen = 5;
static uint16_t tMark = 560, tMarkTol = 300, tSpace0 = 560, tSpace1 = 1690, tSpaceTol = 300;
static uint8_t lastSeq = 0xFF;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x07 : c << 1; }
  return c;
}

static bool matterPower(uint8_t outlet, uint8_t action, String &err);

static void handleFrame() {
  if (crc8(frame, frameLen - 1) != frame[frameLen - 1]) { crcErrors++; Serial.println("rx: bad crc"); return; }
  framesOk++;
  uint8_t dst = frame[0], type = frame[1] >> 4, seq = frame[1] & 0xF;
  if (dst != 0 && dst != nodeId) return;
  if (type != 4) return;                                  // keyboard traffic etc.: not ours
  if (seq == lastSeq) return;                             // duplicate transmission
  lastSeq = seq;
  uint8_t outlet = frame[2], action = frame[3];
  Serial.printf("rx: dst=%u power seq=%u outlet=%u action=%u\n", dst, seq, outlet, action);
  if (action > 2 || outlet > cfgOutlets) { Serial.println("rx: bad outlet/action"); return; }
  digitalWrite(LED_PIN, LOW);
  String err;
  bool ok = matterPower(outlet, action, err);
  digitalWrite(LED_PIN, HIGH);
  if (ok) { powerCmds++; Serial.println("power: ok"); } else { powerFails++; Serial.println("power: FAILED " + err); }
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
      if (near(dur, tSpace0, tSpaceTol)) { }
      else if (near(dur, tSpace1, tSpaceTol + 200)) frame[bitCount / 8] |= 1 << (bitCount % 8);
      else { state = S_IDLE; break; }
      bitCount++;
      if (bitCount == 24 && (frame[1] >> 4) == 3) {
        uint16_t n = frame[2];
        if (n == 0 || n > MAX_FRAME - 4) { state = S_IDLE; break; }
        frameLen = 4 + n;
      }
      if (bitCount == frameLen * 8) { state = S_IDLE; handleFrame(); }
      else state = S_BIT_MARK;
      break;
  }
}

// ---- minimal RFC 6455 client over WiFiClient: one request/response per connection ----
struct Ws {
  WiFiClient c;
  bool open(const char *host, uint16_t port, const char *path, String &err) {
    c.setTimeout(8);
    if (!c.connect(host, port, 5000)) { err = "connect failed"; return false; }
    uint8_t rnd[16]; for (auto &b : rnd) b = esp_random();
    unsigned char key[32]; size_t klen = 0;
    mbedtls_base64_encode(key, sizeof key, &klen, rnd, sizeof rnd); key[klen] = 0;
    c.printf("GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host, port, (char *)key);
    String status = c.readStringUntil('\n');
    if (status.indexOf(" 101") < 0) { err = "handshake: " + status; c.stop(); return false; }
    while (c.connected()) { String l = c.readStringUntil('\n'); if (l == "\r" || l.length() == 0) break; }
    return true;
  }
  bool readExact(uint8_t *buf, size_t n) {
    uint32_t t0 = millis();
    size_t got = 0;
    while (got < n) {
      int r = c.read(buf + got, n - got);
      if (r > 0) { got += r; t0 = millis(); }
      else if (!c.connected() || millis() - t0 > 8000) return false;
      else delay(1);
    }
    return true;
  }
  void sendText(const String &s) {
    uint8_t mask[4]; for (auto &b : mask) b = esp_random();
    uint8_t hdr[4]; size_t h = 0; size_t n = s.length();
    hdr[h++] = 0x81;
    if (n < 126) hdr[h++] = 0x80 | n; else { hdr[h++] = 0x80 | 126; hdr[h++] = n >> 8; hdr[h++] = n & 0xFF; }
    c.write(hdr, h); c.write(mask, 4);
    uint8_t out[128];
    for (size_t i = 0; i < n; i += sizeof out) {
      size_t k = min(sizeof out, n - i);
      for (size_t j = 0; j < k; j++) out[j] = s[i + j] ^ mask[(i + j) & 3];
      c.write(out, k);
    }
  }
  // next text message, or "" on timeout/close; oversized messages (node dumps, events) are skipped
  String recvText() {
    for (;;) {
      uint8_t h[2]; if (!readExact(h, 2)) return "";
      uint8_t op = h[0] & 0x0F; uint32_t len = h[1] & 0x7F;
      if (len == 126) { uint8_t e[2]; if (!readExact(e, 2)) return ""; len = (e[0] << 8) | e[1]; }
      else if (len == 127) { uint8_t e[8]; if (!readExact(e, 8)) return ""; len = (e[4] << 24) | (e[5] << 16) | (e[6] << 8) | e[7]; }
      if (op == 8) return "";
      if (len > 4000 || op != 1) {                   // skip: drain
        uint8_t junk[256];
        while (len) { size_t k = min(len, (uint32_t)sizeof junk); if (!readExact(junk, k)) return ""; len -= k; }
        if (op == 9) { uint8_t pong[2] = {0x8A, 0x80}; uint8_t m[4] = {0,0,0,0}; c.write(pong, 2); c.write(m, 4); }
        continue;
      }
      String s; s.reserve(len + 1);
      uint8_t buf[256];
      while (len) { size_t k = min(len, (uint32_t)sizeof buf); if (!readExact(buf, k)) return ""; s.concat((const char *)buf, k); len -= k; }
      return s;
    }
  }
};

// device_command on the strip's OnOff cluster (6) for one endpoint
static bool matterOne(Ws &ws, uint8_t ep, const char *cmd, uint32_t &msgId, String &err) {
  char body[256];
  snprintf(body, sizeof body,
    "{\"message_id\":\"ir%lu\",\"command\":\"device_command\",\"args\":{\"node_id\":%u,\"endpoint_id\":%u,"
    "\"cluster_id\":6,\"command_name\":\"%s\",\"payload\":{}}}", (unsigned long)msgId, cfgMatterNode, ep, cmd);
  String want = "\"message_id\":\"ir" + String(msgId) + "\""; msgId++;
  ws.sendText(body);
  for (int i = 0; i < 20; i++) {                      // events may interleave; wait for our reply
    String r = ws.recvText();
    if (!r.length()) { err = "no reply"; return false; }
    if (r.indexOf(want) < 0 && r.indexOf("\"message_id\": \"ir") < 0) continue;
    if (r.indexOf("error_code") >= 0) {
      int d = r.indexOf("\"details\""); err = d >= 0 ? r.substring(d, min((int)r.length(), d + 120)) : r.substring(0, 120);
      return false;
    }
    return true;
  }
  err = "reply not seen"; return false;
}

static bool matterPower(uint8_t outlet, uint8_t action, String &err) {
  if (WiFi.status() != WL_CONNECTED) { err = "wifi down"; return false; }
  if (!cfgHost.length()) { err = "no server configured"; return false; }
  static const char *names[] = { "Off", "On", "Toggle" };
  Ws ws;
  if (!ws.open(cfgHost.c_str(), cfgPort, "/ws", err)) return false;
  ws.recvText();                                      // server info banner
  static uint32_t msgId = 1;
  bool ok = true;
  uint8_t lo = outlet ? outlet : 1, hi = outlet ? outlet : cfgOutlets;
  for (uint8_t ep = lo; ep <= hi && ok; ep++) ok = matterOne(ws, ep, names[action], msgId, err);
  ws.c.stop();
  return ok;
}

// ---- config + serial ----
static void loadCfg() {
  prefs.begin("irpower", false);
  nodeId = prefs.getUChar("id", 0);
  cfgSsid = prefs.getString("ssid", ""); cfgPass = prefs.getString("pass", "");
  cfgHost = prefs.getString("host", ""); cfgPort = prefs.getUShort("port", 5580);
  cfgMatterNode = prefs.getUShort("mnode", 8); cfgOutlets = prefs.getUChar("outlets", 6);
}
static void onWifiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) Serial.printf("wifi: disconnected reason=%d\n", (int)info.wifi_sta_disconnected.reason);
  else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) Serial.println("wifi: associated, waiting for DHCP");
}
static void wifiStart() {
  if (!cfgSsid.length() || !cfgPass.length()) return;
  static bool once = false;
  if (!once) { once = true; WiFi.onEvent(onWifiEvent); WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.persistent(false); }
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
}
static bool startsWith(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static void serialCommand(char *line) {
  while (*line == ' ') line++;
  for (size_t n = strlen(line); n && line[n - 1] == ' '; n--) line[n - 1] = 0;
  if (!strcmp(line, "ping")) { Serial.printf("irkvm " BOARD_NAME " id=%u tx=0\n", nodeId); return; }
  if (!strcmp(line, "raw on") || !strcmp(line, "raw off")) { rawDump = line[5] == 'n'; Serial.printf("raw dump %s\n", rawDump ? "on" : "off"); return; }
  if (startsWith(line, "id ")) {
    int v = atoi(line + 3);
    if (v < 1 || v > 254) { Serial.println("id must be 1..254"); return; }
    nodeId = v; prefs.putUChar("id", nodeId); Serial.printf("node id = %u (saved)\n", nodeId);
  } else if (!strcmp(line, "id?") || !strcmp(line, "id")) {
    Serial.printf("node id = %u\n", nodeId);
  } else if (startsWith(line, "ssid ")) {
    cfgSsid = line + 5; prefs.putString("ssid", cfgSsid); Serial.println("ssid saved; set pass then reconnecting"); wifiStart();
  } else if (startsWith(line, "pass ")) {
    cfgPass = line + 5; prefs.putString("pass", cfgPass); Serial.println("pass saved; reconnecting"); wifiStart();
  } else if (startsWith(line, "server ")) {
    char *sp = strchr(line + 7, ' ');
    if (sp) { *sp = 0; cfgPort = atoi(sp + 1); }
    cfgHost = line + 7; prefs.putString("host", cfgHost); prefs.putUShort("port", cfgPort);
    Serial.printf("server = %s:%u (saved)\n", cfgHost.c_str(), cfgPort);
  } else if (startsWith(line, "matter ")) {
    char *sp = strchr(line + 7, ' ');
    if (sp) { cfgOutlets = atoi(sp + 1); prefs.putUChar("outlets", cfgOutlets); }
    cfgMatterNode = atoi(line + 7); prefs.putUShort("mnode", cfgMatterNode);
    Serial.printf("matter node = %u, outlets 1..%u (saved)\n", cfgMatterNode, cfgOutlets);
  } else if (!strcmp(line, "wifi")) {
    Serial.println("wifi: reconnecting"); wifiStart();
  } else if (!strcmp(line, "net")) {
    Serial.printf("wifi ssid=%s status=%d ip=%s rssi=%d  server=%s:%u matter node %u outlets %u\n", cfgSsid.c_str(), (int)WiFi.status(),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), cfgHost.c_str(), cfgPort, cfgMatterNode, cfgOutlets);
  } else if (startsWith(line, "power ")) {
    int outlet = atoi(line + 6); const char *sp = strchr(line + 6, ' ');
    int action = !sp ? -1 : !strcmp(sp + 1, "off") ? 0 : !strcmp(sp + 1, "on") ? 1 : !strcmp(sp + 1, "toggle") ? 2 : -1;
    if (action < 0 || outlet < 0 || outlet > cfgOutlets) { Serial.println("usage: power <outlet|0=all> on|off|toggle"); return; }
    String err; bool ok = matterPower(outlet, action, err);
    Serial.println(ok ? "power: ok" : "power: FAILED " + err);
  } else if (!strcmp(line, "stat")) {
    Serial.printf("pin=%d (idle should be 1) edges=%lu frames=%lu crcerr=%lu power ok=%lu fail=%lu state=%d\n", digitalRead(IR_PIN),
                  (unsigned long)edgeCount, (unsigned long)framesOk, (unsigned long)crcErrors, (unsigned long)powerCmds, (unsigned long)powerFails, (int)state);
  } else if (!strcmp(line, "help")) {
    Serial.println("irpower: id N | ssid S | pass P | server HOST [PORT] | matter NODE [OUTLETS] | net | power OUTLET on|off|toggle | ping | stat | raw on|off");
  } else if (*line) {
    Serial.println("? (try help)");
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(IR_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  loadCfg();
  wifiStart();
  lastEdge = micros();
  attachInterrupt(digitalPinToInterrupt(IR_PIN), onEdge, CHANGE);
}

void loop() {
  uint32_t dur; bool wasMark;
  while (popEdge(dur, wasMark)) feed(dur, wasMark);
  if (state != S_IDLE && (int32_t)(micros() - lastEdge) > 20000) { if (rawDump) Serial.printf("[timeout st=%d bits=%u]\n", (int)state, (unsigned)bitCount); state = S_IDLE; }
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  wl_status_t w = WiFi.status();
  if (w != lastWifi) {
    if (w == WL_CONNECTED) Serial.printf("wifi: connected %s\n", WiFi.localIP().toString().c_str());
    else if (lastWifi == WL_CONNECTED) Serial.println("wifi: lost");
    lastWifi = w;
  }
  static uint32_t lastRetry = 0;
  if (w != WL_CONNECTED && cfgSsid.length() && millis() - lastRetry > 15000) { lastRetry = millis(); WiFi.reconnect(); }
  static char line[CMD_LINE_MAX + 1]; static size_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { line[len] = 0; serialCommand(line); len = 0; } else if (len < CMD_LINE_MAX) line[len++] = c;
  }
}
