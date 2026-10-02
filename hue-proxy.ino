/*
 * Emulated Hue proxy + SSDP + OTA for ESP32-C3 SuperMini
 *
 * Echo (LAN) -> ESP :80 + UDP 1900 -> Home Assistant Emulated Hue
 *
 * GitHub: https://github.com/daosin/esp32-hue-proxy
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// ---------- Secrets ----------
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "secrets.h not found – using placeholders. Copy secrets.h.example to secrets.h!"
  #define WIFI_SSID     "YOUR_WIFI_SSID"
  #define WIFI_PASS     "YOUR_WIFI_PASSWORD"
  #define OTA_PASSWORD  "YOUR_OTA_PASSWORD"
#endif

// ---------- OTA ----------
const char *OTA_HOSTNAME = "hue-proxy";

// ---------- ESP network (change to your values) ----------
const IPAddress ESP_IP(172, 20, 1, 180);
const IPAddress GATEWAY(172, 20, 1, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress DNS1(172, 20, 1, 1);
const IPAddress DNS2(8, 8, 8, 8);

// ---------- Home Assistant (change to your values) ----------
const char *HA_HOST = "10.10.10.112";
const uint16_t HA_PORT = 80;

// IMPORTANT: these two strings MUST have the same length
const char *HA_IP_STR  = "10.10.10.112";
const char *ESP_IP_STR = "172.20.1.180";

// ---------- SSDP ----------
const IPAddress SSDP_IP(239, 255, 255, 250);
const uint16_t SSDP_PORT = 1900;
const char *BRIDGE_UUID = "2f402f80-da50-11e1-9b23-00178809ea66";

WebServer server(80);
WiFiUDP udp;

// ---------- Buffers ----------
static const size_t RESP_MAX = 12288;
static char respBuf[RESP_MAX];
static char ssdpBuf[1024];

// ---------- Upstream health ----------
static uint8_t upstreamFailStreak = 0;
static const uint8_t UPSTREAM_FAIL_RESTART_THRESHOLD = 8;

// ---------- Cache ----------
struct CacheEntry {
  const char *path;
  char       *buf;
  size_t      capacity;
  size_t      len;
  uint32_t    ttl_ms;
  uint32_t    updated_ms;
  const char *contentType;
};

static char descrBuf[1024];
static char nouserBuf[1024];
static char lightsBuf[4096];

static CacheEntry cacheDescr  = { "/description.xml",   descrBuf,  sizeof(descrBuf),  0, 60000, 0, "text/xml" };
static CacheEntry cacheNouser = { "/api/nouser/config", nouserBuf, sizeof(nouserBuf), 0, 60000, 0, "application/json" };
static CacheEntry cacheLights = { "/api/v2/lights",     lightsBuf, sizeof(lightsBuf), 0, 3000,  0, "application/json" };

// ---------- Helpers ----------

static void markUpstreamSuccess() {
  upstreamFailStreak = 0;
}

static void markUpstreamFailure() {
  if (upstreamFailStreak < 255) upstreamFailStreak++;
  if (upstreamFailStreak >= UPSTREAM_FAIL_RESTART_THRESHOLD) {
    ESP.restart();
  }
}

static void replaceIpInPlace(char *s) {
  const size_t ipLen = strlen(HA_IP_STR);
  char *p = s;
  while ((p = strstr(p, HA_IP_STR)) != nullptr) {
    memcpy(p, ESP_IP_STR, ipLen);
    p += ipLen;
  }
}

static bool cacheValid(const CacheEntry &c) {
  return c.len > 0 && (millis() - c.updated_ms) < c.ttl_ms;
}

static CacheEntry* findCacheByUri(const String &uri) {
  if (uri == cacheDescr.path)  return &cacheDescr;
  if (uri == cacheNouser.path) return &cacheNouser;
  if (uri == cacheLights.path) return &cacheLights;
  return nullptr;
}

static bool tryServeCached(const CacheEntry &c) {
  if (!cacheValid(c) || c.len >= RESP_MAX) return false;
  memcpy(respBuf, c.buf, c.len);
  respBuf[c.len] = '\0';
  replaceIpInPlace(respBuf);
  server.send(200, c.contentType, respBuf);
  return true;
}

static bool updateCache(CacheEntry &c, const String &payload) {
  const size_t len = payload.length();
  if (len >= c.capacity) return false;
  memcpy(c.buf, payload.c_str(), len);
  c.buf[len] = '\0';
  c.len = len;
  c.updated_ms = millis();
  return true;
}

// ---------- HTTP proxy ----------

static bool proxyRequest() {
  const String uri = server.uri();

  CacheEntry *cache = findCacheByUri(uri);
  if (cache && tryServeCached(*cache)) return true;

  String path = uri;
  bool firstQuery = true;
  for (int i = 0; i < server.args(); i++) {
    const String name = server.argName(i);
    if (name == "plain") continue;
    path += firstQuery ? "?" : "&";
    firstQuery = false;
    path += name + "=" + server.arg(i);
  }

  String url = String("http://") + HA_HOST + ":" + HA_PORT + path;
  const String body = server.arg("plain");

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    markUpstreamFailure();
    return false;
  }

  if (body.length()) {
    if (server.hasHeader("Content-Type")) {
      http.addHeader("Content-Type", server.header("Content-Type"));
    } else {
      http.addHeader("Content-Type", "application/json");
    }
  }

  int code = -1;
  switch (server.method()) {
    case HTTP_GET:    code = http.GET(); break;
    case HTTP_PUT:    code = http.PUT(body); break;
    case HTTP_POST:   code = http.POST(body); break;
    case HTTP_DELETE: code = http.sendRequest("DELETE", body); break;
    default:
      http.end();
      server.send(405, "text/plain", "Method Not Allowed");
      return true;
  }

  if (code < 0) {
    http.end();
    markUpstreamFailure();
    return false;
  }

  if (http.getSize() >= static_cast<int>(RESP_MAX)) {
    http.end();
    markUpstreamFailure();
    server.send(502, "text/plain", "Upstream response too large");
    return true;
  }

  String payload = http.getString();
  String contentType = http.header("Content-Type");
  http.end();

  if (payload.length() >= static_cast<int>(RESP_MAX)) {
    markUpstreamFailure();
    server.send(502, "text/plain", "Upstream response too large");
    return true;
  }

  if (!contentType.length()) contentType = "application/json";

  const size_t len = payload.length();
  memcpy(respBuf, payload.c_str(), len);
  respBuf[len] = '\0';
  replaceIpInPlace(respBuf);

  if (cache) updateCache(*cache, payload);

  markUpstreamSuccess();
  server.send(code, contentType, respBuf);
  return true;
}

static void handleNotFound() {
  if (!proxyRequest()) {
    server.send(502, "text/plain", "Bad Gateway");
  }
}

// ---------- SSDP ----------

static bool ssdpMatches(const char *request) {
  if (strcasestr(request, "M-SEARCH") == nullptr) return false;
  return
    strcasestr(request, "ssdp:all") != nullptr ||
    strcasestr(request, "upnp:rootdevice") != nullptr ||
    strcasestr(request, "basic:1") != nullptr ||
    strcasestr(request, "WirelessPower") != nullptr ||
    strcasestr(request, "urn:schemas-upnp-org:device:basic:1") != nullptr ||
    strcasestr(request, "hue") != nullptr ||
    strcasestr(request, "IpBridge") != nullptr;
}

static void sendSsdpPacket(const IPAddress &remoteIP, uint16_t remotePort,
                           const char *searchTarget, const char *usn) {
  const int len = snprintf(ssdpBuf, sizeof(ssdpBuf),
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=100\r\n"
    "EXT:\r\n"
    "LOCATION: http://%s:80/description.xml\r\n"
    "SERVER: FreeRTOS/6.0.5, UPnP/1.0, IpBridge/1.17.0\r\n"
    "hue-bridgeid: 001788FFFE09EA66\r\n"
    "ST: %s\r\n"
    "USN: %s\r\n"
    "\r\n",
    ESP_IP_STR, searchTarget, usn);

  if (len > 0 && len < static_cast<int>(sizeof(ssdpBuf))) {
    udp.beginPacket(remoteIP, remotePort);
    udp.write(reinterpret_cast<const uint8_t *>(ssdpBuf), len);
    udp.endPacket();
  }
}

static void sendSsdpResponse(const IPAddress &remoteIP, uint16_t remotePort) {
  String usnBasic = String("uuid:") + BRIDGE_UUID + "::urn:schemas-upnp-org:device:basic:1";
  sendSsdpPacket(remoteIP, remotePort, "urn:schemas-upnp-org:device:basic:1", usnBasic.c_str());

  String usnRoot = String("uuid:") + BRIDGE_UUID;
  sendSsdpPacket(remoteIP, remotePort, "upnp:rootdevice", usnRoot.c_str());
}

static void handleSsdp() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;
  if (packetSize >= static_cast<int>(sizeof(ssdpBuf))) packetSize = sizeof(ssdpBuf) - 1;

  const int bytesRead = udp.read(ssdpBuf, packetSize);
  if (bytesRead <= 0) return;
  ssdpBuf[bytesRead] = '\0';

  if (ssdpMatches(ssdpBuf)) {
    sendSsdpResponse(udp.remoteIP(), udp.remotePort());
  }
}

static void ssdpNotify() {
  static uint32_t lastNotify = 0;
  if (millis() - lastNotify < 60000) return;
  lastNotify = millis();

  const int len = snprintf(ssdpBuf, sizeof(ssdpBuf),
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "CACHE-CONTROL: max-age=100\r\n"
    "LOCATION: http://%s:80/description.xml\r\n"
    "SERVER: FreeRTOS/6.0.5, UPnP/1.0, IpBridge/1.17.0\r\n"
    "NTS: ssdp:alive\r\n"
    "hue-bridgeid: 001788FFFE09EA66\r\n"
    "NT: uuid:%s\r\n"
    "USN: uuid:%s\r\n"
    "\r\n",
    ESP_IP_STR, BRIDGE_UUID, BRIDGE_UUID);

  if (len > 0 && len < static_cast<int>(sizeof(ssdpBuf))) {
    udp.beginPacket(SSDP_IP, SSDP_PORT);
    udp.write(reinterpret_cast<const uint8_t *>(ssdpBuf), len);
    udp.endPacket();
  }
}

// ---------- Wi-Fi + OTA ----------

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.config(ESP_IP, GATEWAY, SUBNET, DNS1, DNS2);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started > 30000) ESP.restart();
    delay(250);
  }
}

static void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
}

void setup() {
  if (strlen(HA_IP_STR) != strlen(ESP_IP_STR)) {
    while (true) delay(1000);   // IP strings length mismatch – fix config
  }

  connectWiFi();

  if (!udp.beginMulticast(SSDP_IP, SSDP_PORT)) {
    udp.begin(SSDP_PORT);
  }

  const char *headers[] = {"Content-Type"};
  server.collectHeaders(headers, 1);
  server.onNotFound(handleNotFound);
  server.begin();

  setupOta();
}

void loop() {
  server.handleClient();
  handleSsdp();
  ssdpNotify();
  ArduinoOTA.handle();

  static uint32_t lastWiFiCheck = 0;
  static uint8_t reconnectAttempts = 0;

  if (millis() - lastWiFiCheck >= 10000) {
    lastWiFiCheck = millis();
    if (WiFi.status() == WL_CONNECTED) {
      reconnectAttempts = 0;
    } else {
      WiFi.reconnect();
      if (++reconnectAttempts >= 12) ESP.restart();
    }
  }
}
