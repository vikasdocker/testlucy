/*
 * ============================================================
 *  ESP32-C3 Super Mini  ->  WiFi Hotspot  +  SSD1306 OLED
 * ============================================================
 *
 *  Hardware (your wiring):
 *    OLED VCC      -> 3V3
 *    OLED GND      -> GND
 *    OLED SCL/SCK  -> GPIO 6
 *    OLED SDA      -> GPIO 7
 *
 *  On-board extras used:
 *    LED           -> GPIO 8  (active LOW on most C3 Super Mini boards)
 *    BOOT button   -> GPIO 9  (short press = next screen, hold 2s = restart AP)
 *
 *  What it does:
 *    - Starts a WPA2 access point (name/password stored in NVS)
 *    - Captive portal: any URL a phone opens lands on the dashboard
 *    - Dashboard: live clients, uptime, heap, change SSID/password, reboot
 *    - OLED shows SSID / password / IP / client count / uptime on 3 pages
 *    - mDNS: http://hotspot.local
 *
 *  Board setting in Arduino IDE:
 *    "Nologo ESP32C3 Super Mini"
 *    USB CDC On Boot: Enabled   (so Serial works over the native USB port)
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <esp_wifi.h>

/* ---------------- pins ---------------- */
#define I2C_SDA        6
#define I2C_SCL        7
#ifndef I2C_CLOCK
#define I2C_CLOCK      100000    // drop to 50000 if the screen is still noisy
#endif
#define LED_PIN        8
#define BTN_PIN        9
#define LED_ACTIVE_LOW true      // set to false if your LED is wired the other way

/* ---------------- hotspot settings ---------------- */
#define AP_PASS_DEFAULT  "hotspot1234"
#define AP_CHANNEL       6
#define AP_MAX_CLIENTS   4
#define AP_HIDDEN        false
#define FW_VERSION       "1.3"

/* ---------------- globals ---------------- */
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WebServer  server(80);
DNSServer  dns;
Preferences prefs;

String  apSsid;
String  apPass;
bool    oledOk      = false;
uint8_t oledOffset  = 2;          // 2 = SH1106 (1.3"), 0 = SSD1306
bool    i2cSlow     = false;
uint32_t i2cErrors  = 0;
void    flush();                  // implemented below

/* ---------------- station (home WiFi) ---------------- */
String   staSsid, staPass;
bool     staStarting = false;
uint32_t staStartMs  = 0;
uint8_t  staState    = 0;         // 0 idle, 1 connecting, 2 connected, 3 failed
bool     apOn        = true;
uint32_t apOffSince  = 0;         // safety timer while the AP is off

/* ---------------- BOOT-button menu ---------------- */
#define  MENU_ITEMS 7
bool     inMenu   = false;
uint8_t  menuSel  = 0;
uint8_t  menuTop  = 0;
uint8_t  menuView = 0;            // 0 list, 1 status, 2 scan, 3 web hint, 4 AP confirm
bool     apConfirm = false;
uint32_t apConfirmAt = 0;
String   scanSsid[8];
int32_t  scanRssi[8];
uint8_t  scanCount = 0;
uint8_t page        = 0;          // 0 = cat, 1 = connection, 2 = clients, 3 = system
const uint8_t PAGES = 4;

unsigned long lastDraw    = 0;
unsigned long lastBlink   = 0;
unsigned long bootMs      = 0;
unsigned long selfCheckAt = 0;
unsigned long restartAt   = 0;    // 0 = no restart pending
bool          ledOn       = false;
volatile bool dirty       = true; // redraw request
uint32_t      lastClients = 0xFF;

/* ============================================================
 *  helpers
 * ============================================================ */
String uptimeStr() {
  unsigned long s = (millis() - bootMs) / 1000;
  char b[16];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(b);
}

String shortUptime() {
  unsigned long s = (millis() - bootMs) / 1000;
  char b[12];
  snprintf(b, sizeof(b), "%02lu:%02lu", (s / 60) % 100, s % 60);
  return String(b);
}

String macToStr(const uint8_t *mac) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

int readStations(wifi_sta_info_t *out, int maxN) {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  int n = (int)list.num;
  if (n > maxN) n = maxN;
  for (int i = 0; i < n; i++) out[i] = list.sta[i];
  return n;
}

void ledWrite(bool on) {
  ledOn = on;
#if LED_ACTIVE_LOW
  digitalWrite(LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

/* ============================================================
 *  OLED drawing
 * ============================================================ */
void drawHeader(const char *title) {
  oled.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(3, 2);
  oled.print(title);
  oled.setCursor(107, 2);
  oled.print(WiFi.softAPgetStationNum());
  oled.print("c");
  oled.setTextColor(SSD1306_WHITE);
}

void drawPageInfo() {
  drawHeader("ESP32-C3  HOTSPOT");

  // SSID big
  oled.setTextSize(1);
  oled.setCursor(0, 13);
  oled.print(F("SSID"));
  oled.setTextSize(2);
  oled.setCursor(0, 22);
  oled.print(apSsid);

  oled.setTextSize(1);
  oled.setCursor(0, 39);
  oled.print(F("PASS "));
  oled.print(apPass);

  oled.setCursor(0, 49);
  oled.print(F("IP   "));
  oled.print(WiFi.softAPIP().toString());

  oled.setCursor(0, 57);
  oled.print(F("CLI "));
  oled.print(WiFi.softAPgetStationNum());
  oled.print('/');
  oled.print(AP_MAX_CLIENTS);

  String up = "UP " + shortUptime();
  oled.setCursor(128 - up.length() * 6, 57);
  oled.print(up);

  flush();
}

void drawPageClients() {
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "CLIENTS  %d/%d", WiFi.softAPgetStationNum(), AP_MAX_CLIENTS);
  drawHeader(hdr);

  wifi_sta_info_t sta[AP_MAX_CLIENTS];
  int n = readStations(sta, AP_MAX_CLIENTS);

  oled.setTextSize(1);
  if (n == 0) {
    oled.setCursor(10, 26);
    oled.print(F("No devices"));
    oled.setCursor(10, 38);
    oled.print(F("connected yet"));
  } else {
    for (int i = 0; i < n; i++) {
      oled.setCursor(0, 14 + i * 11);
      oled.print(macToStr(sta[i].mac).substring(9));   // last 3 bytes
      oled.setCursor(58, 14 + i * 11);
      oled.print(macToStr(sta[i].mac).substring(0, 8));
      oled.setCursor(112, 14 + i * 11);
      oled.print(sta[i].rssi);
    }
    oled.setCursor(0, 57);
    oled.print(F("SSID  "));
    oled.print(apSsid);
  }
  flush();
}

void drawPageSystem() {
  drawHeader("SYSTEM");

  char b[26];
  oled.setTextSize(1);

  oled.setCursor(0, 14);
  oled.print(F("Uptime : "));
  oled.print(uptimeStr());

  oled.setCursor(0, 24);
  oled.print(F("Free   : "));
  oled.print(ESP.getFreeHeap() / 1024);
  oled.print(F(" kB"));

  oled.setCursor(0, 34);
  if (staState == 2) {
    snprintf(b, sizeof(b), "WAN %s", WiFi.localIP().toString().c_str());
  } else if (staSsid.length()) {
    snprintf(b, sizeof(b), "WAN %s", staState == 1 ? "joining..." : "offline");
  } else {
    snprintf(b, sizeof(b), "WAN not set");
  }
  oled.print(b);

  oled.setCursor(0, 44);
  snprintf(b, sizeof(b), "Ch %d   %d client%s", AP_CHANNEL,
           (int)WiFi.softAPgetStationNum(),
           WiFi.softAPgetStationNum() == 1 ? "" : "s");
  oled.print(b);

  oled.setCursor(0, 54);
  snprintf(b, sizeof(b), "IP %s", WiFi.softAPIP().toString().c_str());
  oled.print(b);

  flush();
}

/* ============================================================
 *  BOOT-button menu (WiFi + AP control)
 * ============================================================ */
void drawMenuHeader(const char *t) {
  oled.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setCursor(3, 2);
  oled.print(t);
  oled.setTextColor(SSD1306_WHITE);
}

String menuItemLabel(uint8_t i) {
  switch (i) {
    case 0: return String(F("WiFi status"));
    case 1: return String(F("Scan networks"));
    case 2: return String(F("Home WiFi"));
    case 3: return apOn ? String(F("AP: ON  (turn off)")) : String(F("AP: OFF (turn on)"));
    case 4: return String(F("Web settings"));
    case 5: return String(F("Info screens"));
    default: return String(F("Exit to cat"));
  }
}

void drawMenuList() {
  drawMenuHeader("MENU    hold=select");
  const uint8_t rows = 3;
  if (menuSel < menuTop) menuTop = menuSel;
  if (menuSel >= menuTop + rows) menuTop = menuSel - rows + 1;

  for (uint8_t r = 0; r < rows; r++) {
    uint8_t idx = menuTop + r;
    if (idx >= MENU_ITEMS) break;
    int y = 14 + r * 16;
    bool sel = (idx == menuSel);
    if (sel) oled.fillRect(0, y - 2, 118, 15, SSD1306_WHITE);
    oled.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(4, y + 1);
    oled.print(menuItemLabel(idx));
  }
  oled.setTextColor(SSD1306_WHITE);
  if (menuTop > 0)               { oled.setCursor(121, 12); oled.print(F("^")); }
  if (menuTop + rows < MENU_ITEMS) { oled.setCursor(121, 55); oled.print(F("v")); }
  flush();
}

void drawMenuStatus() {
  drawMenuHeader("WiFi   hold=back");
  oled.setTextSize(1);
  oled.setCursor(0, 14);
  oled.print(F("Home : "));
  oled.print(staSsid.length() ? staSsid.substring(0, 14) : String(F("(not set)")));
  oled.setCursor(0, 25);
  oled.print(F("State: "));
  oled.print(staState == 2 ? "connected" :
             staState == 1 ? "connecting" :
             staState == 3 ? "failed" : "idle");
  oled.setCursor(0, 36);
  oled.print(F("IP   : "));
  oled.print(staState == 2 ? WiFi.localIP().toString() : String(F("-")));
  oled.setCursor(0, 47);
  if (staSsid.length() == 0) {
    oled.print(F("Type it at /wifi"));
  } else {
    oled.print(F("RSSI : "));
    if (staState == 2) { oled.print(WiFi.RSSI()); oled.print(F(" dBm")); }
    else oled.print('-');
  }
  flush();
}

void drawMenuScan() {
  char hdr[26];
  snprintf(hdr, sizeof(hdr), "WIFI   %u found", (unsigned)scanCount);
  drawMenuHeader(hdr);
  oled.setTextSize(1);
  if (scanCount == 0) {
    oled.setCursor(8, 30);
    oled.print(F("No networks found"));
  } else {
    for (uint8_t i = 0; i < scanCount && i < 4; i++) {
      oled.setCursor(0, 14 + i * 11);
      oled.print(scanSsid[i].substring(0, 18));
      oled.setCursor(110, 14 + i * 11);
      oled.print(scanRssi[i]);
    }
    if (scanCount > 4) {
      oled.setCursor(0, 57);
      oled.print(F("+more in web UI"));
    }
  }
  flush();
}

void drawMenuHint() {
  drawMenuHeader("MENU  |  Web");
  oled.setTextSize(1);
  oled.setCursor(0, 16);  oled.print(F("Open a browser at"));
  oled.setCursor(0, 27);  oled.print(WiFi.softAPIP().toString()); oled.print(F("/wifi"));
  oled.setCursor(0, 39);  oled.print(F("type your home WiFi"));
  oled.setCursor(0, 49);  oled.print(F("name + password there"));
  oled.setCursor(0, 57);  oled.print(F("hold: back"));
  flush();
}

void drawMenuConfirm() {
  drawMenuHeader("CONFIRM");
  oled.setTextSize(1);
  oled.setCursor(0, 16);
  oled.print(apOn ? F("Turn hotspot OFF?") : F("Turn hotspot ON?"));
  oled.setCursor(0, 30);
  oled.print(F("Your phone will lose"));
  oled.setCursor(0, 40);
  oled.print(F("the C3-F007 network."));
  oled.setCursor(0, 54);
  oled.print(F("hold=yes  click=no"));
  flush();
}

void drawMenu() {
  switch (menuView) {
    case 1: drawMenuStatus();  break;
    case 2: drawMenuScan();    break;
    case 3: drawMenuHint();    break;
    case 4: drawMenuConfirm(); break;
    default: drawMenuList();   break;
  }
}

/* ============================================================
 *  cat face (full screen, animated)
 * ============================================================ */
// Lucy AI face animation state. Kept local to the OLED renderer.
uint32_t aiNextBlink = 0;
uint32_t aiBlinkUntil = 0;
uint32_t aiNextGaze = 0;
int8_t aiGazeX = 0;
int8_t aiGazeY = 0;

void drawCat() {
  const uint32_t now = millis();

  if (now >= aiNextBlink) {
    aiNextBlink = now + (uint32_t)random(2800, 6200);
    aiBlinkUntil = now + 150;
  }
  if (now >= aiNextGaze) {
    aiNextGaze = now + (uint32_t)random(700, 1900);
    aiGazeX = (int8_t)random(-3, 4);
    aiGazeY = (int8_t)random(-2, 3);
  }

  const bool blink = now < aiBlinkUntil;
  const int bob = (int)(sinf((float)now * 0.0022f) * 1.0f);

  // Outer 3D halo and four small HUD markers.
  oled.drawRoundRect(27, 3 + bob, 74, 58, 19, SSD1306_WHITE);
  oled.drawRoundRect(30, 5 + bob, 68, 54, 16, SSD1306_WHITE);
  oled.drawLine(8, 31, 18, 31, SSD1306_WHITE);
  oled.drawLine(110, 31, 120, 31, SSD1306_WHITE);
  oled.drawPixel(13, 26, SSD1306_WHITE);
  oled.drawPixel(115, 26, SSD1306_WHITE);
  oled.drawPixel(13, 36, SSD1306_WHITE);
  oled.drawPixel(115, 36, SSD1306_WHITE);

  // Side modules give the head a layered, dimensional silhouette.
  oled.fillRoundRect(19, 22 + bob, 12, 23, 4, SSD1306_WHITE);
  oled.fillRoundRect(97, 22 + bob, 12, 23, 4, SSD1306_WHITE);
  oled.fillRoundRect(22, 26 + bob, 6, 15, 2, SSD1306_BLACK);
  oled.fillRoundRect(100, 26 + bob, 6, 15, 2, SSD1306_BLACK);

  // Face plate with a recessed inner panel and a small top crest.
  oled.fillRoundRect(31, 12 + bob, 66, 46, 14, SSD1306_WHITE);
  oled.fillRoundRect(35, 16 + bob, 58, 38, 11, SSD1306_BLACK);
  oled.drawRoundRect(35, 16 + bob, 58, 38, 11, SSD1306_WHITE);
  oled.drawLine(53, 9 + bob, 64, 5 + bob, SSD1306_WHITE);
  oled.drawLine(64, 5 + bob, 75, 9 + bob, SSD1306_WHITE);
  oled.fillCircle(64, 8 + bob, 2, SSD1306_WHITE);

  // Brow ridge and expressive eyes.
  oled.drawLine(42, 24 + bob, 56, 21 + bob, SSD1306_WHITE);
  oled.drawLine(72, 21 + bob, 86, 24 + bob, SSD1306_WHITE);
  if (!blink) {
    oled.fillRoundRect(41, 27 + bob, 18, 15, 6, SSD1306_WHITE);
    oled.fillRoundRect(69, 27 + bob, 18, 15, 6, SSD1306_WHITE);
    oled.fillCircle(50 + aiGazeX, 34 + bob + aiGazeY, 5, SSD1306_BLACK);
    oled.fillCircle(78 + aiGazeX, 34 + bob + aiGazeY, 5, SSD1306_BLACK);
    oled.fillCircle(52 + aiGazeX, 31 + bob + aiGazeY, 1, SSD1306_WHITE);
    oled.fillCircle(80 + aiGazeX, 31 + bob + aiGazeY, 1, SSD1306_WHITE);
  } else {
    oled.drawLine(42, 35 + bob, 58, 35 + bob, SSD1306_WHITE);
    oled.drawLine(70, 35 + bob, 86, 35 + bob, SSD1306_WHITE);
  }

  // Nose bridge, animated-looking friendly mouth, and lower chin facets.
  oled.drawLine(64, 37 + bob, 61, 43 + bob, SSD1306_WHITE);
  oled.drawLine(61, 43 + bob, 67, 43 + bob, SSD1306_WHITE);
  oled.drawLine(52, 48 + bob, 58, 50 + bob, SSD1306_WHITE);
  oled.drawLine(58, 50 + bob, 64, 51 + bob, SSD1306_WHITE);
  oled.drawLine(64, 51 + bob, 70, 50 + bob, SSD1306_WHITE);
  oled.drawLine(70, 50 + bob, 76, 48 + bob, SSD1306_WHITE);
  oled.drawLine(43, 55 + bob, 55, 58 + bob, SSD1306_WHITE);
  oled.drawLine(73, 58 + bob, 85, 55 + bob, SSD1306_WHITE);

  flush();
}

void drawCurrentPage() {
  if (!oledOk) return;
  oled.clearDisplay();
  if (inMenu) { drawMenu(); return; }
  switch (page) {
    case 0:  drawCat();         break;
    case 1:  drawPageInfo();    break;
    case 2:  drawPageClients(); break;
    default: drawPageSystem();  break;
  }
}

/* ============================================================
 *  web UI
 * ============================================================ */
String htmlHead(const char *title) {
  String s = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>");
  s += title;
  s += F("</title><style>"
         ":root{color-scheme:dark}"
         "*{box-sizing:border-box}"
         "body{margin:0;font:16px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
         "background:#0b1020;color:#e6ecff;padding:18px}"
         ".wrap{max-width:520px;margin:0 auto}"
         "h1{font-size:20px;margin:0 0 4px}"
         ".sub{color:#8b9ac4;font-size:13px;margin-bottom:18px}"
         ".card{background:#141c33;border:1px solid #23305a;border-radius:14px;padding:14px 16px;margin-bottom:12px}"
         ".row{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px dashed #23305a}"
         ".row:last-child{border:0}"
         ".k{color:#8b9ac4}"
         ".v{font-weight:600;font-variant-numeric:tabular-nums;word-break:break-all;text-align:right}"
         ".pill{display:inline-block;background:#1b5e3a;color:#8ef0b8;border-radius:99px;"
         "padding:2px 10px;font-size:12px;font-weight:600}"
         "a.btn,button{display:block;width:100%;text-align:center;padding:12px;border-radius:12px;"
         "border:0;font-weight:700;font-size:15px;text-decoration:none;cursor:pointer}"
         "a.btn{background:#2f6bff;color:#fff;margin-top:6px}"
         "button{background:#2f6bff;color:#fff;margin-top:10px}"
         "input{width:100%;padding:11px;border-radius:10px;border:1px solid #2b3a68;"
         "background:#0d1428;color:#e6ecff;font-size:15px;margin:6px 0 12px}"
         "label{font-size:13px;color:#8b9ac4}"
         ".foot{text-align:center;color:#5d6b93;font-size:12px;margin-top:8px}"
         "</style></head><body><div class='wrap'>");
  return s;
}

void send404Redirect() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void handleRoot() {
  wifi_sta_info_t sta[AP_MAX_CLIENTS];
  int n = readStations(sta, AP_MAX_CLIENTS);

  String p = htmlHead("ESP32-C3 Hotspot");
  p += F("<h1>ESP32-C3 Hotspot <span class='pill'>ONLINE</span></h1>");
  p += F("<div class='sub'>SoftAP is broadcasting &mdash; connect and enjoy.</div>");

  p += F("<div class='card'>");
  p += F("<div class='row'><span class='k'>Network (SSID)</span><span class='v'>"); p += apSsid; p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Password</span><span class='v'>");    p += apPass; p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Gateway IP</span><span class='v'>");  p += WiFi.softAPIP().toString(); p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Channel</span><span class='v'>");     p += AP_CHANNEL; p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Clients</span><span class='v'>");
  p += n; p += F(" / "); p += AP_MAX_CLIENTS; p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Uptime</span><span class='v'>");     p += uptimeStr(); p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Home WiFi</span><span class='v'>");
  if (staState == 2)          p += WiFi.localIP().toString();
  else if (staState == 1)     p += F("connecting...");
  else if (staSsid.length())  p += F("not joined");
  else                        p += F("not set");
  p += F("</span></div>");
  p += F("</div>");

  p += F("<div class='card'><div class='k' style='margin-bottom:8px'>Connected devices</div>");
  if (n == 0) {
    p += F("<div class='k'>None yet.</div>");
  } else {
    for (int i = 0; i < n; i++) {
      p += F("<div class='row'><span class='v' style='text-align:left'>");
      p += macToStr(sta[i].mac);
      p += F("</span><span class='k'>"); p += sta[i].rssi; p += F(" dBm</span></div>");
    }
  }
  p += F("</div>");

  p += F("<div class='card'>");
  p += F("<div class='row'><span class='k'>Free heap</span><span class='v'>");
  p += ESP.getFreeHeap() / 1024; p += F(" kB</span></div>");
  p += F("<div class='row'><span class='k'>Chip</span><span class='v'>");
  p += ESP.getChipModel(); p += F(" rev "); p += ESP.getChipRevision(); p += F("</span></div>");
  p += F("<div class='row'><span class='k'>Firmware</span><span class='v'>v" FW_VERSION "</span></div>");
  p += F("</div>");

  p += F("<a class='btn' href='/config'>Change name / password</a>");
  p += F("<a class='btn' href='/wifi' style='background:#1b5e3a'>Home WiFi (join router)</a>");
  p += F("<a class='btn' href='/ap?state=");
  p += (apOn ? "off" : "on");
  p += F("' style='background:");
  p += (apOn ? "#7a2b2b" : "#37406b");
  p += F("'>Access point: ");
  p += (apOn ? "ON" : "OFF");
  p += F(" &mdash; tap to turn ");
  p += (apOn ? "off" : "on");
  p += F("</a>");
  p += F("<a class='btn' style='background:#37406b' href='/display'>Panel offset: ");
  p += (oledOffset ? "SH1106 (1.3in)" : "SSD1306 (0.96in)");
  p += F(" &mdash; tap to switch</a>");
  p += F("<div class='foot'>http://hotspot.local &middot; page refreshes every 10 s</div>");
  p += F("<meta http-equiv='refresh' content='10'>");
  p += F("</div></body></html>");

  server.send(200, "text/html", p);
}

void handleConfig() {
  String p = htmlHead("Hotspot settings");
  p += F("<h1>Hotspot settings</h1><div class='sub'>Saved to flash, AP restarts on save.</div>");
  p += F("<div class='card'><form method='get' action='/save'>");
  p += F("<label>Network name (SSID)</label>");
  p += F("<input name='ssid' maxlength='32' required value='"); p += apSsid; p += F("'>");
  p += F("<label>Password (8&ndash;63 characters)</label>");
  p += F("<input name='pass' minlength='8' maxlength='63' required value='"); p += apPass; p += F("'>");
  p += F("<button type='submit'>Save &amp; restart AP</button></form></div>");
  p += F("<a class='btn' href='/'>Back to dashboard</a>");
  p += F("<div class='foot'>Note: after restart, reconnect with the new credentials.</div>");
  p += F("</div></body></html>");
  server.send(200, "text/html", p);
}

void handleSave() {
  String s = server.arg("ssid");
  String w = server.arg("pass");
  s.trim();

  if (s.length() == 0 || s.length() > 32 || w.length() < 8 || w.length() > 63) {
    server.send(400, "text/html",
                "<body style='font-family:system-ui;background:#0b1020;color:#fff;padding:24px'>"
                "Invalid values. <a style='color:#7aa2ff' href='/config'>Go back</a></body>");
    return;
  }

  prefs.putString("ssid", s);
  prefs.putString("pass", w);
  apSsid = s;
  apPass = w;

  String p = htmlHead("Saved");
  p += F("<h1>Saved</h1><div class='card'>New hotspot: <b>"); p += apSsid;
  p += F("</b><br>Password: <b>"); p += apPass;
  p += F("</b><br><br>The access point restarts in a second &mdash; reconnect with the new details.</div>");
  p += F("</div></body></html>");
  server.send(200, "text/html", p);

  restartAt = millis() + 1200;
}

String esc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&') o += F("&amp;");
    else if (c == '<') o += F("&lt;");
    else if (c == '>') o += F("&gt;");
    else if (c == '"') o += F("&quot;");
    else if (c == '\'') o += F("&#39;");
    else if ((uint8_t)c < 32) o += ' ';
    else o += c;
  }
  return o;
}

String jsonEsc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    if ((uint8_t)c < 32) continue;
    o += c;
  }
  return o;
}

void handleWifiPage() {
  String p = htmlHead("Home WiFi");
  p += F("<h1>Home WiFi</h1><div class='sub'>Join your router &mdash; the hotspot stays up.</div>");

  p += F("<div class='card'>");
  p += F("<div class='row'><span class='k'>Saved network</span><span class='v'>");
  p += (staSsid.length() ? esc(staSsid) : String("none"));
  p += F("</span></div>");
  p += F("<div class='row'><span class='k'>State</span><span class='v'>");
  p += (staState == 2 ? "connected" : staState == 1 ? "connecting..." :
        staState == 3 ? "failed" : "idle");
  p += F("</span></div>");
  p += F("<div class='row'><span class='k'>IP on home network</span><span class='v'>");
  p += (staState == 2 ? WiFi.localIP().toString() : String("-"));
  p += F("</span></div></div>");

  p += F("<div class='card'><form method='get' action='/wifisave'>"
         "<label>Network name (SSID)</label>"
         "<input id='ssid' name='ssid' maxlength='32' required>"
         "<label>Password</label>"
         "<input name='pass' type='password' maxlength='63'>"
         "<button type='submit'>Connect</button></form></div>");

  p += F("<div class='card'><div class='k' style='margin-bottom:8px'>Networks in range</div>");
  String arr = "[";
  int n = WiFi.scanNetworks();
  int shown = 0;
  for (int i = 0; i < n && shown < 12; i++) {
    String s = String(WiFi.SSID(i));
    if (s.length() == 0) continue;
    if (shown) arr += ",";
    arr += "\"" + jsonEsc(s) + "\"";
    p += F("<div class='row'><a href='#' style='color:#e6ecff;text-decoration:none'"
           " onclick='pick(");
    p += shown;
    p += F(");return false'>");
    p += esc(s);
    p += F("</a><span class='k'>");
    p += WiFi.RSSI(i);
    p += F(" dBm</span></div>");
    shown++;
  }
  WiFi.scanDelete();
  arr += "]";
  if (!shown) p += F("<div class='k'>No networks found.</div>");
  p += F("</div>");

  p += F("<a class='btn' href='/'>Back to dashboard</a>");
  p += F("<script>var N=");
  p += arr;
  p += F(";function pick(i){document.getElementById('ssid').value=N[i];"
         "document.querySelector('input[name=pass]').focus();}</script>");
  p += F("</div></body></html>");
  server.send(200, "text/html", p);
}

void handleWifiSave() {
  String s = server.arg("ssid");
  String w = server.arg("pass");
  s.trim();
  if (s.length() == 0 || s.length() > 32) {
    server.send(400, "text/html",
                "<body style='font-family:system-ui;background:#0b1020;color:#fff;padding:24px'>"
                "Invalid network name. <a style='color:#7aa2ff' href='/wifi'>Back</a></body>");
    return;
  }
  staSsid = s;
  staPass = w;
  prefs.putString("wssid", staSsid);
  prefs.putString("wpass", staPass);
  staConnectSaved();

  String p = htmlHead("Connecting");
  p += F("<h1>Connecting</h1><div class='card'>Trying to join <b>");
  p += esc(staSsid);
  p += F("</b>.<br><br>This page refreshes in 4 seconds &mdash; if it says "
         "<b>failed</b>, the password is probably wrong.</div>");
  p += F("<a class='btn' href='/wifi'>Check status</a>");
  p += F("<meta http-equiv='refresh' content='4;url=/wifi'>");
  p += F("</div></body></html>");
  server.send(200, "text/html", p);
}

void handleAP() {
  String st = server.arg("state");
  if ((st == "off" && apOn) || (st == "on" && !apOn)) {
    String p = htmlHead("Access point");
    p += F("<h1>Access point ");
    p += (st == "off" ? "stopped" : "started");
    p += F("</h1><div class='card'>The ESP32-C3 hotspot is now ");
    p += (st == "off" ? "OFF" : "ON");
    p += F(". Use the BOOT button on the board to bring it back.</div>");
    p += F("<a class='btn' href='http://192.168.4.1/'>Dashboard</a>");
    p += F("</div></body></html>");
    server.send(200, "text/html", p);
    if (st == "on" && !apOn) toggleAP();
    else if (st == "off" && apOn) { delay(120); toggleAP(); }
    return;
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleReboot() {
  server.send(200, "text/html",
              "<body style='font-family:system-ui;background:#0b1020;color:#fff;padding:24px'>"
              "Rebooting&hellip;</body>");
  restartAt = millis() + 600;
}

// Toggle the 2-pixel column offset used by 1.3" SH1106 panels.
void handleDisplay() {
  oledOffset = (oledOffset == 0) ? 2 : 0;
  prefs.putUChar("offset", oledOffset);
  Serial.printf("panel column offset -> %u (%s)\n", (unsigned)oledOffset,
                oledOffset ? "SH1106 / 1.3in" : "SSD1306 / 0.96in");

  oledHardInit();
  oled.clearDisplay();
  flush();
  page = 0;

  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(303, "text/plain", "");
}

/* ============================================================
 *  AP start
 * ============================================================ */
void startAP() {
  // Conservative RF power is more reliable on some ESP32-C3 Super Mini clones.
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  delay(250);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  bool configOk = WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                                    IPAddress(192, 168, 4, 1),
                                    IPAddress(255, 255, 255, 0));
  delay(100);
  bool apStarted = WiFi.softAP(apSsid.c_str(), apPass.c_str(),
                               AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);

  // Fallback: keep the setup hotspot alive even if AP+STA fails on this clone.
  if (!apStarted) {
    Serial.println(F("WARN: AP+STA start failed; retrying AP-only"));
    WiFi.mode(WIFI_AP);
    delay(300);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    configOk = WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                                 IPAddress(192, 168, 4, 1),
                                 IPAddress(255, 255, 255, 0));
    delay(100);
    apStarted = WiFi.softAP(apSsid.c_str(), apPass.c_str(),
                            AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);
  }

  apOn = apStarted;
  Serial.printf("AP config: %s | AP start: %s\\n",
                configOk ? "OK" : "FAILED", apStarted ? "OK" : "FAILED");
  Serial.printf("AP radio: ssid='%s' ip=%s ch=%u hidden=%d tx=%.1f dBm mode=%d\\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str(),
                WiFi.channel(), (int)AP_HIDDEN,
                WiFi.getTxPower() * 0.25, (int)WiFi.getMode());

  if (!apStarted) {
    Serial.println(F("ERROR: hotspot did not start; check power, RF hardware and serial log."));
    return;
  }

  dns.stop();
  dns.start(53, "*", WiFi.softAPIP());
  MDNS.end();
  MDNS.begin("hotspot");

  Serial.println(F("--- Hotspot running ---"));
  Serial.printf("SSID : %s\\n", apSsid.c_str());
  Serial.printf("PASS : %s\\n", apPass.c_str());
  Serial.printf("URL  : http://%s/  (http://hotspot.local)\\n",
                WiFi.softAPIP().toString().c_str());
  dirty = true;
}

void loadCredentials() {
  prefs.begin("hotspot", false);
  apSsid = prefs.getString("ssid", "");
  apPass = prefs.getString("pass", "");
  if (apSsid.length() == 0) {
    uint64_t mac = ESP.getEfuseMac();
    char b[16];
    snprintf(b, sizeof(b), "C3-%04X", (uint16_t)(mac >> 32));
    apSsid = String(b);
  }
  if (apPass.length() < 8) apPass = AP_PASS_DEFAULT;
  oledOffset = prefs.getUChar("offset", 2);
  if (oledOffset != 0) oledOffset = 2;
  staSsid = prefs.getString("wssid", "");
  staPass = prefs.getString("wpass", "");
}

/* ============================================================
 *  station mode (home WiFi) + menu actions
 * ============================================================ */
void staConnectSaved() {
  if (staSsid.length() == 0) { staState = 3; return; }
  WiFi.mode(WIFI_AP_STA);          // keep the access point alive
  WiFi.setAutoReconnect(true);     // driver handles drops after a good join
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  staStarting = true;
  staStartMs  = millis();
  staState    = 1;
  Serial.printf("STA: connecting to '%s'\n", staSsid.c_str());
}

void staPoll() {
  if (!staStarting) return;
  if (WiFi.status() == WL_CONNECTED) {
    staStarting = false;
    staState    = 2;
    Serial.printf("STA: connected, IP %s\n", WiFi.localIP().toString().c_str());
    dirty = true;
  } else if (millis() - staStartMs > 15000) {
    // give up instead of hammering WiFi.begin(): repeated attempts while the
    // AP is running can wedge the AP interface and make the SSID vanish.
    staStarting = false;
    staState    = 3;
    WiFi.disconnect(false, false);
    Serial.println(F("STA: failed - not retrying automatically"));
    startAP();                     // re-arm the hotspot after a failed attempt
  }
}

void doScan() {
  WiFi.scanDelete();
  int n = WiFi.scanNetworks();     // blocking, ~2 s
  scanCount = 0;
  for (int i = 0; i < n && scanCount < 8; i++) {
    String s = String(WiFi.SSID(i));
    if (s.length() == 0) continue;
    scanSsid[scanCount] = s;
    scanRssi[scanCount] = WiFi.RSSI(i);
    scanCount++;
  }
  WiFi.scanDelete();
  Serial.printf("scan: %u networks\n", (unsigned)scanCount);
}

void toggleAP() {
  if (apOn) {
    WiFi.softAPdisconnect(true);
    dns.stop();
    apOn = false;
    apOffSince = millis();
    Serial.println(F("AP: off"));
  } else {
    startAP();
    apOn = true;
    apOffSince = 0;
    Serial.println(F("AP: on"));
  }
}

void menuActivate() {
  switch (menuSel) {
    case 0:
      menuView = 1;
      if (staState != 2 && staSsid.length()) staConnectSaved();
      break;
    case 1:
      menuView = 2;
      doScan();
      break;
    case 2:
      if (staSsid.length() == 0) { menuView = 3; }   // no credentials -> show how to set them
      else { staConnectSaved(); menuView = 1; }
      break;
    case 3:
      // two-step: first hold asks, second hold within 8 s confirms
      if (apConfirm && millis() - apConfirmAt < 8000) {
        apConfirm = false;
        menuView  = 0;
        toggleAP();
      } else {
        apConfirm   = true;
        apConfirmAt = millis();
        menuView    = 4;
      }
      break;
    case 4:
      menuView = 3;
      break;
    case 5:
      inMenu = false;
      page = (page % 3) + 1;      // rotate info screens 1..3
      break;
    default:
      inMenu = false;
      page = 0;
      break;
  }
}

/* ============================================================
 *  OLED bring-up (auto-detects swapped SDA/SCL)
 * ============================================================ */
bool i2cProbe(uint8_t sda, uint8_t scl) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(I2C_CLOCK);
  delay(30);
  Wire.beginTransmission(0x3C);
  return (Wire.endTransmission() == 0);
}

// If a slave holds SDA low (loose wire / hot-plugged display) the bus locks up.
// Clock SCL nine times to let it finish its byte, then issue a STOP.
void i2cBusRecover(uint8_t sda, uint8_t scl) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT);
  digitalWrite(scl, HIGH);
  for (int i = 0; i < 9; i++) {
    digitalWrite(scl, LOW);  delayMicroseconds(10);
    digitalWrite(scl, HIGH); delayMicroseconds(10);
  }
  pinMode(sda, OUTPUT);
  digitalWrite(sda, LOW);  delayMicroseconds(10);
  digitalWrite(scl, HIGH); delayMicroseconds(10);
  digitalWrite(sda, HIGH); delayMicroseconds(10);
  pinMode(sda, INPUT_PULLUP);
}

bool oledInit() {
  uint8_t sda = I2C_SDA, scl = I2C_SCL;
  if (!i2cProbe(sda, scl)) {
    if (i2cProbe(I2C_SCL, I2C_SDA)) {
      sda = I2C_SCL; scl = I2C_SDA;
      Serial.printf("note: SDA/SCL were swapped, using SDA=%u SCL=%u\n", sda, scl);
    } else {
      i2cBusRecover(I2C_SDA, I2C_SCL);
      if (!i2cProbe(I2C_SDA, I2C_SCL)) {
        Serial.printf("OLED not found at 0x3C on SDA=%u/SCL=%u\n", I2C_SDA, I2C_SCL);
        return false;
      }
      Serial.println(F("note: I2C bus recovered"));
      sda = I2C_SDA; scl = I2C_SCL;
    }
  }
  Serial.printf("OLED probe ok: SDA=%u SCL=%u addr=0x3C, %lu kHz, column offset %u\n",
                sda, scl, (unsigned long)(I2C_CLOCK / 1000), (unsigned)oledOffset);

  // Adafruit only allocates/draws into the 1 KB framebuffer. The actual panel
  // traffic uses our own page-mode init/flush, which works on both SSD1306 and
  // SH1106 (the latter has no horizontal-addressing mode at all).
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) return false;
  oledHardInit();

  delay(60);
  oled.clearDisplay();
  flush();
  delay(40);
  oled.clearDisplay();
  flush();
  return true;
}

/* ------------------------------------------------------------
 *  Panel driver: page addressing only.
 *  Both SSD1306 and SH1106 understand 0xB0|page plus column-set
 *  commands, so the framebuffer is pushed page by page and never
 *  depends on the horizontal-addressing mode SH1106 lacks.
 * ------------------------------------------------------------ */
void oledCmd(uint8_t c) {
  Wire.beginTransmission(0x3C);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  if (Wire.endTransmission() != 0) i2cErrors++;
}

void oledHardInit() {
  static const uint8_t seq[] = {
    0xAE,             // display off
    0xD5, 0x80,       // clock divide / oscillator
    0xA8, 0x3F,       // multiplex ratio 1/64
    0xD3, 0x00,       // display offset 0
    0x40,             // start line 0
    0x8D, 0x14,       // SSD1306: charge pump on
    0xAD, 0x8B,       // SH1106: DC-DC on
    0x32,             // SH1106: pump voltage
    0x20, 0x02,       // SSD1306: page addressing (ignored by SH1106)
    0xA1,             // segment remap
    0xC8,             // COM scan reversed
    0xDA, 0x12,       // COM pins
    0x81, 0xCF,       // contrast
    0xD9, 0xF1,       // pre-charge
    0xDB, 0x40,       // VCOMH
    0xA4,             // display follows RAM
    0xA6,             // normal (not inverted)
    0xAF              // display on
  };
  for (uint16_t i = 0; i < sizeof(seq); i++) oledCmd(seq[i]);
}

void oledFlushPages() {
  uint8_t *buf = oled.getBuffer();
  if (!buf) return;
  for (uint8_t page = 0; page < 8; page++) {
    uint8_t col = oledOffset;
    Wire.beginTransmission(0x3C);
    Wire.write((uint8_t)0x00);                   // command stream
    Wire.write((uint8_t)(0xB0 | page));          // page address
    Wire.write((uint8_t)(0x00 | (col & 0x0F)));  // column low nibble
    Wire.write((uint8_t)(0x10 | (col >> 4)));    // column high nibble
    if (Wire.endTransmission() != 0) { i2cErrors++; continue; }

    for (uint8_t x = 0; x < 128; x += 16) {
      Wire.beginTransmission(0x3C);
      Wire.write((uint8_t)0x40);                 // data stream
      Wire.write(buf + page * 128 + x, 16);
      if (Wire.endTransmission() != 0) i2cErrors++;
    }
  }
}

void flush() {
  if (!oledOk) return;
  oledFlushPages();

  // self-healing: if bytes are being lost, drop the clock and report it
  if (!i2cSlow && i2cErrors > 20) {
    i2cSlow = true;
    Wire.setClock(50000);
    Serial.printf("I2C errors (%lu) -> bus dropped to 50 kHz\n",
                  (unsigned long)i2cErrors);
  }
}

/* ============================================================
 *  setup / loop
 * ============================================================ */
void setup() {
  Serial.begin(115200);
  delay(300);
  bootMs = millis();

  pinMode(LED_PIN, OUTPUT);
  ledWrite(true);                       // LED on while booting
  pinMode(BTN_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println(F("ESP32-C3 Super Mini hotspot booting..."));

  Wire.begin(I2C_SDA, I2C_SCL);
  oledOk = oledInit();
  if (!oledOk) {
    Serial.println(F("OLED disabled - hotspot still works, check wiring"));
  } else {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(6, 20);
    oled.print(F("ESP32-C3 Super Mini"));
    oled.setCursor(6, 32);
    oled.print(F("Starting hotspot..."));
    flush();
  }

  loadCredentials();
  startAP();
  if (staSsid.length()) staConnectSaved();   // auto-join home WiFi

  // web routes
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", handleSave);
  server.on("/reboot", handleReboot);
  server.on("/display", handleDisplay);
  server.on("/wifi", handleWifiPage);
  server.on("/wifisave", handleWifiSave);
  server.on("/ap", handleAP);
  // captive-portal probes
  server.on("/generate_204", send404Redirect);
  server.on("/gen_204", send404Redirect);
  server.on("/hotspot-detect.html", send404Redirect);
  server.on("/library/test/success.html", send404Redirect);
  server.on("/connecttest.txt", send404Redirect);
  server.on("/ncsi.txt", send404Redirect);
  server.onNotFound(send404Redirect);
  server.begin();

  Serial.println(F("Ready."));
  ledWrite(false);
  drawCurrentPage();
  selfCheckAt = millis() + 5000;   // one-shot: can we hear our own AP?
}

void loop() {
  dns.processNextRequest();
  server.handleClient();

  // ---- one-shot radio status check; avoid active scan disrupting AP beacons ----
  if (selfCheckAt && millis() > selfCheckAt) {
    selfCheckAt = 0;
    Serial.printf("SELFCHECK: AP=%s SSID='%s' IP=%s clients=%u channel=%u tx=%.1f dBm mode=%d\\n",
                  WiFi.softAPIP() == IPAddress(0, 0, 0, 0) ? "NO-IP" : "UP",
                  apSsid.c_str(), WiFi.softAPIP().toString().c_str(),
                  (unsigned)WiFi.softAPgetStationNum(), WiFi.channel(),
                  WiFi.getTxPower() * 0.25, (int)WiFi.getMode());
  }

  // ---- deferred restart (after AP credential change / reboot) ----
  if (restartAt && millis() > restartAt) {
    restartAt = 0;
    Serial.println(F("Restarting access point..."));
    WiFi.softAPdisconnect(true);
    delay(300);
    startAP();
    lastDraw = 0;
  }

  // ---- BOOT button: short press = navigate, hold = select ----
  static bool     btnDown   = false;
  static uint32_t btnAt     = 0;
  static bool     holdFired = false;
  bool pressed = (digitalRead(BTN_PIN) == LOW);

  if (pressed && !btnDown) {
    btnDown = true; btnAt = millis(); holdFired = false;

  } else if (pressed && btnDown && !holdFired && millis() - btnAt > 700) {
    // ---- hold: open menu / select item / back out ----
    holdFired = true;
    if (!inMenu) {
      inMenu = true; menuSel = 0; menuTop = 0; menuView = 0; apConfirm = false;
    } else if (menuView == 4) {
      menuActivate();                 // confirm the AP toggle
    } else if (menuView != 0) {
      menuView = 0;
      apConfirm = false;
    } else {
      menuActivate();
    }
    drawCurrentPage();

  } else if (!pressed && btnDown) {
    // ---- short click: next item / next screen ----
    btnDown = false;
    if (!holdFired && millis() - btnAt > 40) {
      if (!inMenu) {
        inMenu = true; menuSel = 0; menuTop = 0; menuView = 0; apConfirm = false;
      } else if (menuView != 0) {
        menuView = 0;
        apConfirm = false;
      } else {
        menuSel = (menuSel + 1) % MENU_ITEMS;
      }
      drawCurrentPage();
    }
  }

  // ---- client count change -> instant redraw ----
  uint32_t c = WiFi.softAPgetStationNum();
  if (c != lastClients) {
    lastClients = c;
    dirty = true;
  }

  // ---- station state machine (home WiFi) ----
  staPoll();

  // ---- lockout guard: AP off with no home WiFi for 60 s -> hotspot returns ----
  if (!apOn && staState != 2) {
    if (apOffSince && millis() - apOffSince > 60000) {
      Serial.println(F("AP: no uplink for 60 s, restoring hotspot"));
      startAP();
      apOn = true;
      apOffSince = 0;
      drawCurrentPage();
    }
  } else {
    apOffSince = 0;
  }

  // ---- periodic redraw (cat animates faster than the info pages) ----
  uint32_t interval = (inMenu ? 1000 : (page == 0 ? 160 : 1000));
  if (millis() - lastDraw > interval) {
    lastDraw = millis();
    dirty = false;
    drawCurrentPage();
  }

  // ---- status LED: on = clients connected, slow blink = idle ----
  if (c > 0) {
    ledWrite(true);
  } else if (millis() - lastBlink > 1500) {
    lastBlink = millis();
    ledWrite(!ledOn);
  }
}
