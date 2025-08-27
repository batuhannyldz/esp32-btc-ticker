/*
  ESP32 + ILI9488 (TFT_eSPI) — BTC/USDT (Binance 24hr)
  - Tek çağrı: /api/v3/ticker/24hr?symbol=BTCUSDT  → lastPrice + priceChangePercent
  - WiFi yeniden bağlanma, üstel backoff, NTP saat, sprite UI
  - Cloudflare'a takılmamak için: desktop User-Agent + HTTP/1.0 + fallback hostlar

  TFT_eSPI User_Setup.h:
    #define ILI9488_DRIVER
    // MISO=19, MOSI=23, SCLK=18, CS=15, DC=27, RST=33
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

// ====== WiFi ======
const char* WIFI_SSID = "x";
const char* WIFI_PASS = "y";

// ====== Ekran ======
TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);
const int ROT = 1;      // 480x320 yatay
const int BL  = 27;     

// ====== Zaman (GMT+3) ======
WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", 3 * 3600, 60'000);

// ====== Binance API ======
const char* SYMBOL = "BTCUSDT";
const char* HOSTS[] = {
  "https://api.binance.com",
  "https://api1.binance.com",
  "https://api2.binance.com",
  "https://api3.binance.com"
};
const size_t HOSTS_COUNT = sizeof(HOSTS) / sizeof(HOSTS[0]);

// ====== Süreler ======
const uint32_t POLL_INTERVAL_MS = 15'000;
uint32_t backoffMs = POLL_INTERVAL_MS;
const uint32_t BACKOFF_MIN = 15'000;
const uint32_t BACKOFF_MAX = 5 * 60'000;

// ====== Durum ======
float g_price = NAN;
float g_chg24 = NAN;      // %
char  g_timeStr[16] = "--:--:--";
bool  g_connected = false;
bool  g_lastCallOk = false;
size_t g_hostIdx = 0;

// ====== Yardımcılar ======
String formatFloat2(float v) {
  if (isnan(v)) return String("--");
  char b[32]; dtostrf(v, 0, 2, b); return String(b);
}

String formatUsd(float v) {
  if (isnan(v)) return String("$--");
  long whole = (long)fabs(v);
  int  cents = (int)round((fabs(v) - whole) * 100.0f);
  if (cents == 100) { whole += 1; cents = 0; }

  char tmp[32]; ltoa(whole, tmp, 10);
  char buf[48]; int idx = 0; int len = strlen(tmp);
  for (int i = 0; i < len; ++i) {
    buf[idx++] = tmp[i];
    int left = len - i - 1;
    if (left > 0 && left % 3 == 0) buf[idx++] = ',';
  }
  buf[idx] = 0;

  char out[64]; snprintf(out, sizeof(out), "$%s.%02d", buf, cents);
  return v < 0 ? String("-") + out : String(out);
}

void drawCenterText(TFT_eSprite& s, const String& txt, int y, int size, uint16_t fg, uint16_t bg = TFT_BLACK) {
  s.setTextFont(1);
  s.setTextSize(size);
  s.setTextColor(fg, bg);
  int w = s.textWidth(txt);
  int x = (s.width() - w) / 2;
  s.setCursor(x, y);
  s.print(txt);
}
void drawRightText(TFT_eSprite& s, const String& txt, int xRight, int y, int size, uint16_t fg, uint16_t bg = TFT_BLACK) {
  s.setTextFont(1);
  s.setTextSize(size);
  s.setTextColor(fg, bg);
  int w = s.textWidth(txt);
  s.setCursor(xRight - w, y);
  s.print(txt);
}

// ---- HTTP GET (desktop UA, HTTP/1.0, TLS insecure) + fallback
struct HttpResult { bool ok; int code; String body; String url; };
HttpResult httpGET_with_fallback(const String& pathAndQuery) {
  for (size_t n = 0; n < HOSTS_COUNT; ++n) {
    size_t idx = (g_hostIdx + n) % HOSTS_COUNT;
    String url = String(HOSTS[idx]) + pathAndQuery;

    HTTPClient http;
    http.setTimeout(9000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("User-Agent",
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/119.0 Safari/537.36");
    http.addHeader("Accept", "application/json");
    http.addHeader("Connection", "close");
    http.useHTTP10(true); 

    WiFiClientSecure c; c.setInsecure();
    bool began = http.begin(c, url);
    if (!began) { http.end(); continue; }

    int code = http.GET();
    String body = (code > 0) ? http.getString() : "";
    http.end();

    Serial.printf("[HTTP] %s -> %d (%d bytes)\n", url.c_str(), code, body.length());
    if (code == 200) { g_hostIdx = idx; return {true, code, body, url}; }
    else Serial.println(body);
  }
  return {false, -1, "", ""};
}

// ---- JSON Parse (Binance 24hr)
bool parseBinance24hr(const String& js, float& lastPrice, float& chg24pct) {
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, js);
  if (err) { Serial.printf("[JSON] %s\n", err.c_str()); return false; }

  // Örnek alanlar: "lastPrice":"59484.56000000","priceChangePercent":"-2.35"
  const char* sLast = doc["lastPrice"];
  const char* sPct  = doc["priceChangePercent"];
  if (!sLast || !sPct) return false;

  lastPrice = String(sLast).toFloat();
  chg24pct  = String(sPct ).toFloat();
  return true;
}

// Saat
void updateTimeString() {
  if (!ntp.update()) ntp.forceUpdate();
  unsigned long epoch = ntp.getEpochTime();
  if (epoch == 0) { strncpy(g_timeStr, "--:--:--", sizeof(g_timeStr)); return; }
  int h = (epoch % 86400L) / 3600;
  int m = (epoch % 3600) / 60;
  int s = epoch % 60;
  snprintf(g_timeStr, sizeof(g_timeStr), "%02d:%02d:%02d", h, m, s);
}

// Çizim
void drawScreen() {
  spr.fillSprite(TFT_BLACK);

  drawCenterText(spr, "BTC / USDT (Binance)", 8, 2, TFT_CYAN);

  // Fiyat
  {
    String p = formatUsd(g_price);
    int sz = 6; spr.setTextFont(1);
    while (sz > 2) { spr.setTextSize(sz); if (spr.textWidth(p) <= spr.width() - 20) break; sz--; }
    drawCenterText(spr, p, 78, sz, TFT_WHITE);
  }

  // 24h değişim
  {
    bool up = (!isnan(g_chg24) && g_chg24 >= 0.0f);
    String arrow = up ? " \x18" : " \x19"; // fonta bağlı ↑/↓
    String line = String("24h: ") + formatFloat2(g_chg24) + "%" + arrow;
    uint16_t col = isnan(g_chg24) ? TFT_LIGHTGREY : (up ? TFT_GREEN : TFT_RED);
    drawCenterText(spr, line, 178, 3, col);
  }

  // Alt bar
  {
    uint16_t bar = TFT_DARKGREY;
    spr.fillRect(0, 280, spr.width(), 40, bar);

    String left = g_connected ? "WiFi: OK" : "WiFi: Yok";
    spr.setTextFont(1); spr.setTextSize(2); spr.setTextColor(TFT_WHITE, bar);
    spr.setCursor(10, 290); spr.print(left);

    drawRightText(spr, String("Son: ") + g_timeStr, spr.width() - 10, 290, 2, TFT_WHITE, bar);

    String mid = g_lastCallOk ? "API OK" : "API Hata / Beklemede";
    drawCenterText(spr, mid, 292, 2, g_lastCallOk ? TFT_WHITE : TFT_YELLOW, bar);
  }

  spr.pushSprite(0, 0);
}

void showMessage(const char* msgTop, const char* msgBottom = nullptr) {
  spr.fillSprite(TFT_BLACK);
  drawCenterText(spr, msgTop, 120, 2, TFT_YELLOW);
  if (msgBottom) drawCenterText(spr, msgBottom, 150, 2, TFT_LIGHTGREY);
  spr.pushSprite(0, 0);
}

// Veri çekimi (tek çağrı, fallback'li)
bool fetchOnce() {
  String path = String("/api/v3/ticker/24hr?symbol=") + SYMBOL;
  auto r = httpGET_with_fallback(path);
  if (!r.ok) { g_lastCallOk = false; return false; }

  float price=NAN, pct=NAN;
  if (!parseBinance24hr(r.body, price, pct)) {
    Serial.println("[JSON] parse error"); Serial.println(r.body);
    g_lastCallOk = false; return false;
  }
  g_price = price;
  g_chg24 = pct;
  g_lastCallOk = true;
  return true;
}

// Wi-Fi
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) { g_connected = true; return; }
  g_connected = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15'000) delay(250);
  g_connected = (WiFi.status() == WL_CONNECTED);
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(ROT);
  if (BL >= 0) { pinMode(BL, OUTPUT); digitalWrite(BL, HIGH); }

  spr.createSprite(480, 320);
  spr.setTextFont(1);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);

  showMessage("WiFi'ye baglaniyor...");
  ensureWifi();

  ntp.begin();
  updateTimeString();

  if (!g_connected) showMessage("WiFi baglanamadi", "Tekrar denenecek...");

  if (g_connected && fetchOnce()) { updateTimeString(); backoffMs = BACKOFF_MIN; }
  else { g_lastCallOk = false; }

  drawScreen();
}

void loop() {
  static uint32_t lastPoll = 0;
  static uint32_t lastUi   = 0;
  uint32_t now = millis();

  // Her 1 sn saat tazele
  if (now - lastUi >= 1000) { lastUi = now; updateTimeString(); drawScreen(); }

  // Periyodik veri çek (backoff)
  if (now - lastPoll >= backoffMs) {
    lastPoll = now;

    ensureWifi();
    bool ok = false;
    if (g_connected) ok = fetchOnce();

    if (!ok) {
      g_lastCallOk = false;
      // farklı hostlarda da hata aldıysak backoff büyüsün
      backoffMs = min(backoffMs * 2, BACKOFF_MAX);
      Serial.printf("[BACKOFF] Next try in %lu ms\n", (unsigned long)backoffMs);
      showMessage("Veri cekilemedi", "Tekrar denenecek...");
    } else {
      backoffMs = BACKOFF_MIN;
    }
    drawScreen();
  }
}
