/*
  LED Clock  -  ESP8266 + 6x 74HC595 (6 digits, 7 segments + dp, active-low outputs)
  Firmware 2.3.0

  Changes in 2.3.0 (on top of 2.2.0)
  -----------------------------------
  * Wi-Fi credentials (ssid/password/configured) now live in their own EEPROM
    region with their own magic/CRC, separate from the rest of Config. Every
    earlier version reset Wi-Fi whenever an unrelated setting was added to
    Config and its magic got bumped, forcing a trip back through the AP to
    re-enter the SSID/password. From this version on, only a change to the
    WifiCreds struct itself (see near EEPROM_SIZE) can do that; Config can
    keep growing without touching Wi-Fi. THIS ONE update still needs a manual
    step: on first boot it copies whatever SSID/password is already sitting in
    the old Config blob into the new protected region (logged as "Wi-Fi
    credentials migrated..."), so if Wi-Fi was already configured before this
    flash, it carries over automatically - only a device coming from a
    reset/never-configured state needs the AP again.

  Changes in 2.2.0 (on top of 2.1.0)
  -----------------------------------
  * Night mode can now run on a schedule: pick a start and end time (HH:MM,
    wraps past midnight) in the Clock tab, and brightness drops to 50% of the
    configured ceiling (Settings -> Max Brightness Limit) automatically
    inside that window. The manual switch still works when the schedule is off;
    it's disabled (and shows the live computed state) while the schedule is on.
  * PWM refresh raised from ~208 Hz to ~1 kHz (software only, no rewiring) to
    remove visible strobing at low brightness.
  * NTP server is now configurable in Settings, with the current server and
    last-sync time shown next to the Sync button.
  * Firmware can be uploaded either the network way (ArduinoOTA, Arduino IDE /
    PlatformIO) or from a browser file-picker at /firmware (ESP8266HTTPUpdateServer).
  * Config layout changed (new fields) - EEPROM magic was bumped, so the very
    first boot after flashing this version resets to defaults; Wi-Fi has to be
    entered again through the AP once more.

  What changed compared with 2.0.0
  --------------------------------
  * Compiles: no ESP8266TelnetServer / getLocalTime(), globals declared before use,
    no broken string literals, no ANSI garbage in the HTML.
  * The web page is a static PROGMEM document. All texts (RU/EN) live in the page,
    the device only sends JSON. No more string-index mismatches, no big String in RAM.
  * Display is refreshed from a timer1 ISR (5 kHz tick, 32 PWM steps -> ~200 Hz,
    gamma-corrected). Nothing in loop() can make it flicker any more.
  * Night mode really works (half brightness).
  * Wi-Fi and NTP are non-blocking. AP no longer disappears after 30 s.
    SNTP uses 3 servers and a POSIX TZ string (DST support).
  * Config in EEPROM has a version/CRC, is written with a 2 s debounce.
  * Telnet: real WiFiServer, non-blocking line reader, disabled by default, setting persists.
  * Cheap hardening: state changing requests are POST + custom header, logs are shown
    with textContent (no XSS), OTA password is not shown in the UI, AP password 8..63 chars.

  Build: Arduino IDE / PlatformIO, ESP8266 core 3.x (2.7.x should work too)
  Pins : DATA=GPIO13, CLOCK=GPIO14, LATCH=GPIO12 (same as before)
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>
#include <time.h>
#include <coredecls.h>
#include <stddef.h>

#define FIRMWARE_VERSION "2.3.0"

// ============================================================================
//  Tunables
// ============================================================================
static const char*    AP_SSID             = "Clock-Setup";
static const char*    DEFAULT_AP_PASSWORD = "12345678";   // change it in Settings -> AP password
static const char*    OTA_PASSWORD        = "ota123";     // change before flashing
static const char*    DEFAULT_TZ          = "<+03>-3";    // POSIX TZ, UTC+3 without DST
static const char*    DEFAULT_NTP_SERVER  = "pool.ntp.org";
static const uint32_t AP_IDLE_TIMEOUT_MS  = 180000UL;     // AP off after 3 min idle (only if STA is connected)
static const uint32_t AP_FALLBACK_MS      = 20000UL;      // start AP if STA is down this long
static const uint32_t STA_RETRY_MS        = 60000UL;      // re-issue WiFi.begin() while disconnected
static const time_t   MIN_VALID_TIME      = 1700000000;   // anything below = clock not set yet

// ============================================================================
//  Display (74HC595 x6, driven from timer1 ISR)
// ============================================================================
static const uint8_t  DATA_PIN  = 13;
static const uint8_t  CLOCK_PIN = 14;
static const uint8_t  LATCH_PIN = 12;
#define DATA_MASK   (1UL << 13)
#define CLOCK_MASK  (1UL << 14)
#define LATCH_MASK  (1UL << 12)

static const uint8_t  PWM_STEPS     = 32;     // brightness resolution
static const uint32_t TIMER_TICKS   = 312;    // 312 / 5 MHz = 62.4 us -> 32 * 62.4 us = 2.0 ms (~500 Hz)
static const uint8_t  MIN_OFF_TICKS = 3;      // never let the "off" pulse shrink below this many ticks
// v2.1.0 ran the refresh at ~208 Hz, which caused long (up to ~4.6 ms) unbroken
// on/off bursts that read as visible strobing at LOW brightness. Bumping it to
// ~1040 Hz fixed that, but it also quadrupled how often the ESP8266's LED
// current makes a big step (every on/off transition), and how often a stray
// few-hundred-microsecond interrupt stall from the Wi-Fi stack can land inside
// the (now very short) "off" pulse near HIGH brightness and swallow it whole -
// both of which show up as irregular, brightness-dependent flicker instead.
// ~500 Hz is a middle ground: bursts stay under ~2 ms (well short of anything
// visible), while current-step and jitter-collision events happen about half
// as often as at 1040 Hz. MIN_OFF_TICKS below is the other half of the fix:
// below it, an "off" pulse gets rounded up to a safe minimum length instead of
// shrinking to 1 tick, so a single missed timer tick can no longer erase it
// completely. Neither of these eliminates the underlying cause (interrupt
// jitter and/or a marginal power supply) - they just make single misses much
// less likely and much less visible when they do happen.

// segment bits: bit0 = dp, bit1 = a, bit2 = b, bit3 = c, bit4 = d, bit5 = e, bit6 = f, bit7 = g
static const uint8_t DIGIT_MAP[10] = {
  0b01111110, 0b00001100, 0b10110110, 0b10011110, 0b11001100,
  0b11011010, 0b11111010, 0b00001110, 0b11111110, 0b11011110
};
static const uint8_t SEG_DASH = 0b10000000;

static volatile uint8_t g_frame[6] = {0, 0, 0, 0, 0, 0};   // active-high (1 = segment on)
static volatile bool    g_frameDirty = true;
static volatile uint8_t g_duty = PWM_STEPS;                 // 0..PWM_STEPS
static uint8_t          s_pwmPos = 0;
static int8_t           s_lastOn = -1;

static inline void IRAM_ATTR pushRegisters(bool on) {
  GPOC = LATCH_MASK;
  for (uint8_t i = 0; i < 6; i++) {
    const uint8_t v = on ? (uint8_t)~g_frame[i] : (uint8_t)0xFF;   // 0xFF = everything off
    for (uint8_t b = 0x80; b; b >>= 1) {
      if (v & b) GPOS = DATA_MASK; else GPOC = DATA_MASK;
      GPOS = CLOCK_MASK;
      asm volatile("nop; nop; nop;");
      GPOC = CLOCK_MASK;
    }
  }
  GPOS = LATCH_MASK;
}

static void IRAM_ATTR displayIsr() {
  const bool on = s_pwmPos < g_duty;
  if (++s_pwmPos >= PWM_STEPS) s_pwmPos = 0;
  if ((int8_t)on != s_lastOn || (on && g_frameDirty)) {
    s_lastOn = on;
    g_frameDirty = false;
    pushRegisters(on);
  }
}

static void startDisplayTimer() {
  timer1_isr_init();
  timer1_attachInterrupt(displayIsr);
  timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);   // 80 MHz / 16 = 5 MHz
  timer1_write(TIMER_TICKS);
}

static void setFrame(const uint8_t* f) {
  for (uint8_t i = 0; i < 6; i++) g_frame[i] = f[i];
  g_frameDirty = true;   // must be set after the data is written
}

// ============================================================================
//  Config (EEPROM)
// ============================================================================
static const uint32_t CONFIG_MAGIC = 0xC10C0212;   // bumped: added maxBrightness field
static const size_t   EEPROM_SIZE  = 512;

struct Config {
  uint32_t magic;
  char     ssid[33];
  char     password[65];
  char     apPassword[65];
  char     tz[48];
  char     ntpServer[48];
  uint8_t  configured;
  uint8_t  nightMode;
  uint8_t  blinkDots;
  uint8_t  telnetEnabled;
  uint8_t  brightness;    // 1..100
  uint8_t  maxBrightness; // 1..100, hard ceiling applied on top of "brightness"
  uint8_t  language;      // 0 = ru, 1 = en
  uint8_t  nightAutoEnabled;
  uint8_t  nightStartHour;
  uint8_t  nightStartMinute;
  uint8_t  nightEndHour;
  uint8_t  nightEndMinute;
  uint32_t crc;           // must stay last
};

static Config   config;
static bool     configDirty = false;
static uint32_t configDirtyAt = 0;

static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFUL;
  while (n--) {
    c ^= *d++;
    for (uint8_t i = 0; i < 8; i++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320UL : 0UL);
  }
  return ~c;
}

static uint32_t configCrc() {
  return crc32((const uint8_t*)&config, offsetof(Config, crc));
}

static void copyStr(char* dst, size_t cap, const String& s) {
  size_t n = s.length();
  if (n > cap - 1) n = cap - 1;
  memcpy(dst, s.c_str(), n);
  dst[n] = '\0';
}

// ============================================================================
//  Wi-Fi credentials - stored in their own EEPROM region, independent of the
//  rest of Config. Rationale: Config's magic/CRC gets bumped whenever the
//  struct's layout changes (a new setting added, say), which invalidates the
//  whole blob and resets everything in it, Wi-Fi included - meaning every
//  firmware update that adds an unrelated setting would otherwise force the
//  clock back into AP mode and require re-entering the SSID/password by hand.
//  Keeping ssid/password/configured in their own block with their own magic
//  means only a change to THIS struct can reset them; an unrelated Config
//  change can't touch it. config.ssid/config.password/config.configured stay
//  as before for the rest of the code to read - they're just no longer the
//  thing that gets persisted on every save.
// ============================================================================
static const uint32_t WIFI_CREDS_MAGIC  = 0xC10CF1D0;
static const size_t   WIFI_CREDS_OFFSET = 300;   // fixed; must stay >= sizeof(Config), see static_assert below

struct WifiCreds {
  uint32_t magic;
  char     ssid[33];
  char     password[65];
  uint8_t  configured;
  uint32_t crc;   // must stay last
};
static_assert(sizeof(Config) <= WIFI_CREDS_OFFSET,
              "Config grew past WIFI_CREDS_OFFSET - move WIFI_CREDS_OFFSET further out");
static_assert(WIFI_CREDS_OFFSET + sizeof(WifiCreds) <= EEPROM_SIZE,
              "WifiCreds no longer fits before EEPROM_SIZE - raise EEPROM_SIZE");

static uint32_t wifiCredsCrc(const WifiCreds& w) {
  return crc32((const uint8_t*)&w, offsetof(WifiCreds, crc));
}

static bool loadWifiCreds(WifiCreds& w) {
  EEPROM.get(WIFI_CREDS_OFFSET, w);
  if (w.magic != WIFI_CREDS_MAGIC || w.crc != wifiCredsCrc(w)) return false;
  w.ssid[sizeof(w.ssid) - 1] = '\0';
  w.password[sizeof(w.password) - 1] = '\0';
  return true;
}

// Persists the in-memory config.ssid / config.password / config.configured
// into the protected region. Call this any time those three change.
static void saveWifiCreds() {
  WifiCreds w{};
  w.magic = WIFI_CREDS_MAGIC;
  copyStr(w.ssid, sizeof(w.ssid), String(config.ssid));
  copyStr(w.password, sizeof(w.password), String(config.password));
  w.configured = config.configured;
  w.crc = wifiCredsCrc(w);
  EEPROM.put(WIFI_CREDS_OFFSET, w);
  EEPROM.commit();
}

// ============================================================================
//  Logging (line-based ring buffer)
// ============================================================================
static const size_t LOG_SIZE = 2048;
static char   logBuf[LOG_SIZE] = "";
static size_t logLen = 0;

static void logAppend(const char* line, size_t n) {
  if (n >= LOG_SIZE) { line += n - (LOG_SIZE - 1); n = LOG_SIZE - 1; }
  if (logLen + n >= LOG_SIZE) {
    size_t cut = logLen + n - (LOG_SIZE - 1);           // at least this much has to go
    while (cut < logLen && logBuf[cut - 1] != '\n') cut++;   // ...and only whole lines
    if (cut >= logLen) {
      logLen = 0;
    } else {
      memmove(logBuf, logBuf + cut, logLen - cut);
      logLen -= cut;
    }
  }
  memcpy(logBuf + logLen, line, n);
  logLen += n;
  logBuf[logLen] = '\0';
}

void logMessage(const String& msg, bool isError = false) {
  const unsigned long up = millis() / 1000UL;
  char line[200];
  snprintf(line, sizeof(line), "[%02lu:%02lu:%02lu] %s %s\n",
           up / 3600UL, (up % 3600UL) / 60UL, up % 60UL,
           isError ? "[ERROR]" : "[INFO] ", msg.c_str());
  size_t n = strlen(line);
  if (n == sizeof(line) - 1) line[n - 1] = '\n';   // message was truncated, keep the line break
  Serial.print(line);
  logAppend(line, n);
}

static void clearLogs() {
  logBuf[0] = '\0';
  logLen = 0;
  logMessage("Logs cleared");
}

// ============================================================================
//  Config load / save
// ============================================================================
static void setDefaults() {
  memset(&config, 0, sizeof(config));
  config.magic = CONFIG_MAGIC;
  copyStr(config.apPassword, sizeof(config.apPassword), DEFAULT_AP_PASSWORD);
  copyStr(config.tz, sizeof(config.tz), DEFAULT_TZ);
  copyStr(config.ntpServer, sizeof(config.ntpServer), DEFAULT_NTP_SERVER);
  config.configured    = 0;
  config.nightMode     = 0;
  config.blinkDots     = 1;
  config.telnetEnabled = 0;
  config.brightness    = 100;
  config.maxBrightness = 100;
  config.language      = 0;
  config.nightAutoEnabled = 0;
  config.nightStartHour   = 22;
  config.nightStartMinute = 0;
  config.nightEndHour     = 7;
  config.nightEndMinute   = 0;
}

static void saveConfigNow() {
  config.magic = CONFIG_MAGIC;
  config.crc = configCrc();
  EEPROM.put(0, config);
  EEPROM.commit();
  configDirty = false;
  logMessage("Configuration saved");
}

static void markConfigDirty() {
  configDirty = true;
  configDirtyAt = millis();
}

static void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);
  if (config.magic != CONFIG_MAGIC || config.crc != configCrc()) {
    logMessage("No valid config found, using defaults");
    setDefaults();
    saveConfigNow();
  }
  // sanitize
  config.ssid[sizeof(config.ssid) - 1] = '\0';
  config.password[sizeof(config.password) - 1] = '\0';
  config.apPassword[sizeof(config.apPassword) - 1] = '\0';
  config.tz[sizeof(config.tz) - 1] = '\0';
  config.ntpServer[sizeof(config.ntpServer) - 1] = '\0';
  if (config.brightness < 1 || config.brightness > 100) config.brightness = 100;
  if (config.maxBrightness < 1 || config.maxBrightness > 100) config.maxBrightness = 100;
  if (config.language > 1) config.language = 0;
  if (config.tz[0] == '\0') copyStr(config.tz, sizeof(config.tz), DEFAULT_TZ);
  if (config.ntpServer[0] == '\0') copyStr(config.ntpServer, sizeof(config.ntpServer), DEFAULT_NTP_SERVER);
  if (config.nightStartHour > 23 || config.nightStartMinute > 59) { config.nightStartHour = 22; config.nightStartMinute = 0; }
  if (config.nightEndHour > 23 || config.nightEndMinute > 59)     { config.nightEndHour = 7;    config.nightEndMinute = 0; }
  if (strlen(config.apPassword) < 8) copyStr(config.apPassword, sizeof(config.apPassword), DEFAULT_AP_PASSWORD);
  if (config.ssid[0] == '\0') config.configured = 0;

  // Wi-Fi credentials: always prefer the protected region over whatever is in
  // the (possibly just-defaulted) main blob, since only the protected region
  // is guaranteed to survive an unrelated Config layout change.
  WifiCreds w;
  if (loadWifiCreds(w)) {
    copyStr(config.ssid, sizeof(config.ssid), String(w.ssid));
    copyStr(config.password, sizeof(config.password), String(w.password));
    config.configured = w.configured;
    logMessage("Wi-Fi credentials restored from protected storage");
  } else if (config.ssid[0] != '\0') {
    // First boot on this scheme: the main blob still has valid credentials
    // (it wasn't just reset) but the protected copy doesn't exist yet - adopt
    // them so every later update stops resetting Wi-Fi.
    saveWifiCreds();
    logMessage("Wi-Fi credentials migrated to protected storage");
  }
}

// ============================================================================
//  Runtime state
// ============================================================================
static String   hostName;
static bool     apActive = false;
static uint32_t apLastActivity = 0;
static uint32_t staDownSince = 0;
static uint32_t lastStaBegin = 0;
static bool     wasConnected = false;

static bool     needSntp = false;
static bool     needMdns = false;
static bool     mdnsStarted = false;
static volatile uint32_t syncCount = 0;       // incremented by SNTP callback
static volatile bool     syncLogPending = false;
static time_t            lastSyncTime = 0;
static bool     forceRedraw = true;

static bool     pendingRestart = false;
static uint32_t restartAt = 0;

static const IPAddress apIP(192, 168, 4, 1);
static const IPAddress apMask(255, 255, 255, 0);

static ESP8266WebServer server(80);
static ESP8266HTTPUpdateServer httpUpdater;
static const char* HTTP_UPDATE_PATH = "/firmware";
static const char* HTTP_UPDATE_USER = "admin";

// ============================================================================
//  Helpers
// ============================================================================
static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s.charAt(i);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

static bool isHexStr(const String& s) {
  for (size_t i = 0; i < s.length(); i++) if (!isxdigit((unsigned char)s.charAt(i))) return false;
  return true;
}

// WPA2 passphrase: 8..63 printable chars, or exactly 64 hex digits
static bool validWpaPass(const String& p) {
  const size_t n = p.length();
  if (n >= 8 && n <= 63) return true;
  return n == 64 && isHexStr(p);
}

static bool validTz(const String& tz) {
  if (tz.length() == 0 || tz.length() >= sizeof(config.tz)) return false;
  for (size_t i = 0; i < tz.length(); i++) {
    const char c = tz.charAt(i);
    if (!(isalnum((unsigned char)c) || strchr("+-,./:<>", c))) return false;
  }
  return true;
}

// hostname or IPv4: letters, digits, dot, dash
static bool validNtpServer(const String& s) {
  if (s.length() == 0 || s.length() >= sizeof(config.ntpServer)) return false;
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s.charAt(i);
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
  }
  return true;
}

// "HH:MM", 24h, zero-padded (what <input type="time"> sends)
static bool parseHHMM(const String& s, uint8_t& h, uint8_t& m) {
  if (s.length() != 5 || s.charAt(2) != ':') return false;
  for (uint8_t i = 0; i < 5; i++) {
    if (i == 2) continue;
    if (!isdigit((unsigned char)s.charAt(i))) return false;
  }
  const int hh = s.substring(0, 2).toInt();
  const int mm = s.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  h = (uint8_t)hh;
  m = (uint8_t)mm;
  return true;
}

static String formatHHMM(uint8_t h, uint8_t m) {
  char b[8];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)h, (unsigned)m);
  return String(b);
}

static String tzFromOffset(int h) {
  const int a = h < 0 ? -h : h;
  String s = "<";
  s += (h < 0 ? "-" : "+");
  if (a < 10) s += "0";
  s += String(a);
  s += ">";
  s += String(-h);
  return s;
}

static void applyTimezone() {
  setenv("TZ", config.tz, 1);
  tzset();
  forceRedraw = true;
}

// True while "now" falls inside [start,end), wrapping past midnight if end <= start.
// A zero-length window (start == end) is treated as "schedule off" rather than "always on".
static bool nightScheduleActive() {
  const time_t t = time(nullptr);
  if (t < MIN_VALID_TIME) return false;   // no time yet: don't guess, stay at normal brightness
  struct tm tmv;
  localtime_r(&t, &tmv);
  const int nowMin   = tmv.tm_hour * 60 + tmv.tm_min;
  const int startMin = config.nightStartHour * 60 + config.nightStartMinute;
  const int endMin   = config.nightEndHour * 60 + config.nightEndMinute;
  if (startMin == endMin) return false;
  if (startMin < endMin) return nowMin >= startMin && nowMin < endMin;
  return nowMin >= startMin || nowMin < endMin;   // wraps past midnight, e.g. 22:00 -> 07:00
}

static bool effectiveNightMode() {
  return config.nightAutoEnabled ? nightScheduleActive() : (bool)config.nightMode;
}

static uint8_t computeDuty() {
  uint32_t b;
  if (effectiveNightMode()) {
    b = config.maxBrightness / 2;     // night brightness = 50% of the ceiling, not of the day slider
  } else {
    b = config.brightness;
    if (b > config.maxBrightness) b = config.maxBrightness;   // safety ceiling
  }
  if (b == 0) b = 1;
  uint32_t d = (b * b * PWM_STEPS + 5000UL) / 10000UL;    // gamma 2.0
  if (d == 0) d = 1;
  if (d > PWM_STEPS) d = PWM_STEPS;
  // Round the danger zone (an "off" pulse of 1..MIN_OFF_TICKS-1 ticks) down to a
  // safe floor. d == PWM_STEPS (fully on, no "off" pulse at all) is left alone -
  // it can't be glitched the same way since there's nothing to swallow.
  if (d < PWM_STEPS && d > PWM_STEPS - MIN_OFF_TICKS) d = PWM_STEPS - MIN_OFF_TICKS;
  return (uint8_t)d;
}

static void updateDuty() {
  g_duty = computeDuty();
}

static void scheduleRestart(uint32_t delayMs) {
  pendingRestart = true;
  restartAt = millis() + delayMs;
  logMessage(String("Restart scheduled in ") + delayMs + " ms");
}

static void handlePendingRestart() {
  if (pendingRestart && (int32_t)(millis() - restartAt) >= 0) {
    if (configDirty) saveConfigNow();
    logMessage("Restarting");
    Serial.flush();
    ESP.restart();
  }
}

static void commitConfigIfDue() {
  if (configDirty && (millis() - configDirtyAt) > 2000UL) saveConfigNow();
}

static String timeString() {
  const time_t t = time(nullptr);
  if (t < MIN_VALID_TIME) return String("--:--:--");
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[12];
  snprintf(b, sizeof(b), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return String(b);
}

static String dateString() {
  const time_t t = time(nullptr);
  if (t < MIN_VALID_TIME) return String("----------");
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[24];
  snprintf(b, sizeof(b), "%04d-%02d-%02d", (tmv.tm_year + 1900) % 10000, (tmv.tm_mon + 1) % 100, tmv.tm_mday % 100);
  return String(b);
}

static String formatTime(time_t t) {
  if (t < MIN_VALID_TIME) return String("-");
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[24];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
           (tmv.tm_year + 1900) % 10000, (tmv.tm_mon + 1) % 100, tmv.tm_mday % 100,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return String(b);
}

// ============================================================================
//  Clock task
// ============================================================================
static void clockTask() {
  static time_t lastT = 0;
  static bool   lastValid = false;

  const time_t t = time(nullptr);
  const bool valid = t >= MIN_VALID_TIME;
  if (!forceRedraw && valid == lastValid && (!valid || t == lastT)) return;

  if (config.nightAutoEnabled) updateDuty();   // schedule depends on the clock, re-check every tick here (~1/s)

  uint8_t f[6];
  if (valid) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    f[0] = DIGIT_MAP[tmv.tm_hour / 10];
    f[1] = DIGIT_MAP[tmv.tm_hour % 10];
    f[2] = DIGIT_MAP[tmv.tm_min / 10];
    f[3] = DIGIT_MAP[tmv.tm_min % 10];
    f[4] = DIGIT_MAP[tmv.tm_sec / 10];
    f[5] = DIGIT_MAP[tmv.tm_sec % 10];
    if (config.blinkDots && (tmv.tm_sec & 1)) {
      for (uint8_t i = 0; i < 6; i++) f[i] |= 0x01;
    }
  } else {
    for (uint8_t i = 0; i < 6; i++) f[i] = SEG_DASH;
  }
  setFrame(f);
  lastT = t;
  lastValid = valid;
  forceRedraw = false;
}

// ============================================================================
//  SNTP
// ============================================================================
static void onTimeSet() {          // called by the SNTP client, keep it tiny
  syncCount++;
  syncLogPending = true;
}

static void startSntp() {
  // Custom server first, two fixed fallbacks behind it. Avoid sending the same
  // hostname twice if the user's server happens to match one of the fallbacks.
  const char* fb1 = "pool.ntp.org";
  const char* fb2 = "time.cloudflare.com";
  if (strcmp(config.ntpServer, fb1) == 0) fb1 = "time.google.com";
  else if (strcmp(config.ntpServer, fb2) == 0) fb2 = "time.google.com";
  configTime(config.tz, config.ntpServer, fb1, fb2);
  forceRedraw = true;
  logMessage(String("SNTP started, primary server: ") + config.ntpServer);
}

// ============================================================================
//  Wi-Fi / AP
// ============================================================================
static void startAP() {
  if (apActive) return;
  WiFi.mode(config.configured ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, apMask);
  bool ok = WiFi.softAP(AP_SSID, config.apPassword);
  if (!ok) ok = WiFi.softAP(AP_SSID, DEFAULT_AP_PASSWORD);
  apActive = ok;
  apLastActivity = millis();
  if (ok) logMessage(String("AP started: ") + AP_SSID + ", IP " + WiFi.softAPIP().toString());
  else    logMessage("Failed to start AP", true);
}

static void stopAP() {
  if (!apActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  logMessage("AP stopped");
}

static void touchAP() {
  apLastActivity = millis();
}

static void wifiTask() {
  const bool conn = WiFi.status() == WL_CONNECTED;
  const uint32_t now = millis();

  if (conn != wasConnected) {
    wasConnected = conn;
    if (conn) {
      logMessage(String("Wi-Fi connected, IP ") + WiFi.localIP().toString() +
                 ", RSSI " + WiFi.RSSI() + " dBm");
      needSntp = true;
      needMdns = true;
    } else {
      staDownSince = now;
      if (config.configured) logMessage("Wi-Fi connection lost", true);
    }
  }

  if (!config.configured) {
    startAP();                                    // nothing to connect to: AP stays up
    return;
  }

  if (!conn) {
    if (!apActive && (now - staDownSince) > AP_FALLBACK_MS) startAP();
    if ((now - lastStaBegin) > STA_RETRY_MS) {
      lastStaBegin = now;
      WiFi.begin(config.ssid, config.password);   // non-blocking
    }
  } else if (apActive && (now - apLastActivity) > AP_IDLE_TIMEOUT_MS) {
    stopAP();
  }

  if (needSntp && conn) {
    needSntp = false;
    startSntp();
  }
  if (needMdns && conn) {
    needMdns = false;
    if (!mdnsStarted) {
      if (MDNS.begin(hostName)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("arduino", "tcp", 8266);
        mdnsStarted = true;
        logMessage(String("mDNS started: ") + hostName + ".local");
      } else {
        logMessage("Failed to start mDNS", true);
      }
    }
  }
  if (syncLogPending) {
    syncLogPending = false;
    lastSyncTime = time(nullptr);
    logMessage(String("Time synchronized: ") + dateString() + " " + timeString());
  }
}

// ============================================================================
//  OTA
// ============================================================================
static void setupOTA() {
  ArduinoOTA.setHostname(hostName.c_str());
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { logMessage("OTA update started"); });
  ArduinoOTA.onEnd([]() { logMessage("OTA update finished"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastStep = 99;
    const unsigned int pct = total ? (unsigned int)((uint64_t)progress * 100 / total) : 0;
    if (pct / 10 != lastStep) {
      lastStep = pct / 10;
      logMessage(String("OTA progress: ") + pct + "%");
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    String m = "OTA error: ";
    switch (error) {
      case OTA_AUTH_ERROR:    m += "auth failed";    break;
      case OTA_BEGIN_ERROR:   m += "begin failed";   break;
      case OTA_CONNECT_ERROR: m += "connect failed"; break;
      case OTA_RECEIVE_ERROR: m += "receive failed"; break;
      case OTA_END_ERROR:     m += "end failed";     break;
      default:                m += String((int)error); break;
    }
    logMessage(m, true);
  });
  ArduinoOTA.begin(false);   // mDNS is started by us
  logMessage(String("OTA ready: ") + hostName + ", port 8266");
}

// ============================================================================
//  Telnet
// ============================================================================
static const uint8_t MAX_TELNET = 3;

struct TelnetSession {
  WiFiClient client;
  char       buf[80];
  uint8_t    len;
  uint8_t    iac;      // telnet negotiation state
  bool       active;
};

static WiFiServer    telnetServer(23);
static TelnetSession sessions[MAX_TELNET];
static bool          telnetRunning = false;

static void applyTelnet() {
  if (config.telnetEnabled && !telnetRunning) {
    telnetServer.begin();
    telnetServer.setNoDelay(true);
    telnetRunning = true;
    logMessage("Telnet server started on port 23");
  } else if (!config.telnetEnabled && telnetRunning) {
    for (uint8_t i = 0; i < MAX_TELNET; i++) {
      if (sessions[i].active) sessions[i].client.stop();
      sessions[i].active = false;
    }
    telnetServer.stop();
    telnetRunning = false;
    logMessage("Telnet server stopped");
  }
}

static void telnetHelp(Print& out) {
  out.println("Commands:");
  out.println("  time | date | wifi | ip | ntp | version | status");
  out.println("  brightness [1-100]");
  out.println("  maxbrightness [1-100] (safety ceiling, applies to brightness above it)");
  out.println("  night [on|off]        (half brightness)");
  out.println("  blink [on|off]");
  out.println("  tz [UTC+N | POSIX TZ] (e.g. tz EST5EDT,M3.2.0,M11.1.0)");
  out.println("  sync                  (restart SNTP)");
  out.println("  logs | clear | restart | exit | help");
}

// Returns true when the session should be closed (exit/quit/logout/bye).
static bool processTelnetCommand(WiFiClient& out, String line) {
  line.trim();
  String cmd = line;
  cmd.toLowerCase();
  String arg = "";
  const int sp = line.indexOf(' ');
  if (sp > 0) {
    cmd = line.substring(0, sp);
    cmd.toLowerCase();
    arg = line.substring(sp + 1);
    arg.trim();
  }
  String argLower = arg;
  argLower.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    telnetHelp(out);
  } else if (cmd == "time") {
    out.println("Time: " + timeString());
  } else if (cmd == "date") {
    out.println("Date: " + dateString());
  } else if (cmd == "wifi") {
    out.println(String("Connected: ") + (WiFi.status() == WL_CONNECTED ? "yes" : "no"));
    out.println(String("SSID: ") + config.ssid);
    out.println("IP: " + WiFi.localIP().toString());
    out.println(String("RSSI: ") + WiFi.RSSI() + " dBm");
    out.println(String("AP: ") + (apActive ? "on" : "off"));
  } else if (cmd == "ip") {
    if (WiFi.status() == WL_CONNECTED) out.println("IP: " + WiFi.localIP().toString());
    else if (apActive)                 out.println("AP IP: " + WiFi.softAPIP().toString());
    else                               out.println("Not connected");
  } else if (cmd == "ntp") {
    out.println(String("Synced: ") + (syncCount > 0 ? "yes" : "no") + " (" + syncCount + " updates)");
    out.println(String("TZ: ") + config.tz);
  } else if (cmd == "status") {
    out.println("Time: " + dateString() + " " + timeString());
    out.println(String("Brightness: ") + config.brightness + "%, night " + (config.nightMode ? "on" : "off"));
    out.println(String("Heap: ") + ESP.getFreeHeap() + " bytes");
    out.println(String("Uptime: ") + (millis() / 1000UL) + " s");
  } else if (cmd == "version") {
    out.println("Firmware " FIRMWARE_VERSION);
  } else if (cmd == "brightness") {
    if (arg.length() == 0) {
      out.println(String("Brightness: ") + config.brightness + "% (ceiling " + config.maxBrightness + "%)");
    } else {
      const long v = arg.toInt();
      if (v >= 1 && v <= 100) {
        config.brightness = (uint8_t)v;
        updateDuty();
        markConfigDirty();
        out.println(String("Brightness set to ") + v + "%");
      } else {
        out.println("Invalid value (1-100)");
      }
    }
  } else if (cmd == "maxbrightness") {
    if (arg.length() == 0) {
      out.println(String("Max brightness: ") + config.maxBrightness + "%");
    } else {
      const long v = arg.toInt();
      if (v >= 1 && v <= 100) {
        config.maxBrightness = (uint8_t)v;
        updateDuty();
        markConfigDirty();
        out.println(String("Max brightness set to ") + v + "%");
      } else {
        out.println("Invalid value (1-100)");
      }
    }
  } else if (cmd == "night") {
    if (argLower == "on" || argLower == "off") {
      config.nightMode = (argLower == "on");
      updateDuty();
      markConfigDirty();
    }
    out.println(String("Night mode: ") + (config.nightMode ? "on" : "off"));
  } else if (cmd == "blink") {
    if (argLower == "on" || argLower == "off") {
      config.blinkDots = (argLower == "on");
      forceRedraw = true;
      markConfigDirty();
    }
    out.println(String("Blink dots: ") + (config.blinkDots ? "on" : "off"));
  } else if (cmd == "tz") {
    if (arg.length() == 0) {
      out.println(String("TZ: ") + config.tz);
    } else {
      String tz = arg;
      if (argLower.startsWith("utc") && (argLower.length() == 3 || argLower.charAt(3) == '+' || argLower.charAt(3) == '-')) {
        const int h = argLower.substring(3).toInt();
        if (h < -12 || h > 14) { out.println("Invalid offset (-12..+14)"); return false; }
        tz = tzFromOffset(h);
      }
      if (!validTz(tz)) {
        out.println("Invalid TZ string");
      } else {
        copyStr(config.tz, sizeof(config.tz), tz);
        applyTimezone();
        markConfigDirty();
        out.println(String("TZ set to ") + config.tz);
      }
    }
  } else if (cmd == "sync") {
    startSntp();
    out.println("SNTP restarted");
  } else if (cmd == "logs") {
    out.println("=== Logs ===");
    out.print(logBuf);
  } else if (cmd == "clear") {
    clearLogs();
    out.println("Logs cleared");
  } else if (cmd == "restart") {
    out.println("Restarting...");
    scheduleRestart(1000);
  } else if (cmd == "exit" || cmd == "quit" || cmd == "logout" || cmd == "bye") {
    out.println("Bye!");
    return true;
  } else if (cmd.length() > 0) {
    out.println("Unknown command, type 'help'");
  }
  return false;
}

static const char* TELNET_PROMPT = "clock> ";

static void handleTelnet() {
  if (!telnetRunning) return;

  if (telnetServer.hasClient()) {
#if defined(ARDUINO_ESP8266_MAJOR) && ARDUINO_ESP8266_MAJOR >= 3
    WiFiClient nc = telnetServer.accept();
#else
    WiFiClient nc = telnetServer.available();
#endif
    int slot = -1;
    for (uint8_t i = 0; i < MAX_TELNET; i++) {
      if (!sessions[i].active) { slot = i; break; }
    }
    if (slot < 0) {
      nc.println("Server busy");
      nc.stop();
    } else {
      sessions[slot].client = nc;
      sessions[slot].len = 0;
      sessions[slot].iac = 0;
      sessions[slot].active = true;
      nc.println("LED Clock " FIRMWARE_VERSION " - type 'help'");
      nc.print(TELNET_PROMPT);
      logMessage(String("Telnet client connected from ") + nc.remoteIP().toString());
    }
  }

  for (uint8_t i = 0; i < MAX_TELNET; i++) {
    TelnetSession& s = sessions[i];
    if (!s.active) continue;
    if (!s.client.connected() && !s.client.available()) {
      s.client.stop();
      s.active = false;
      logMessage("Telnet client disconnected");
      continue;
    }
    while (s.client.available()) {
      const int ch = s.client.read();
      if (ch < 0) break;
      const uint8_t c = (uint8_t)ch;
      if (s.iac) {                                   // swallow IAC negotiation
        if (s.iac == 1) {
          if (c >= 251 && c <= 254) s.iac = 2; else s.iac = 0;
        } else {
          s.iac = 0;
        }
        continue;
      }
      if (c == 0xFF) { s.iac = 1; continue; }
      if (c == '\n') {
        s.buf[s.len] = '\0';
        s.len = 0;
        const bool wantExit = processTelnetCommand(s.client, String(s.buf));
        if (wantExit) {
          s.client.flush();
          delay(1);              // give the "Bye!" line a moment to actually go out
          s.client.stop();
          s.active = false;
          logMessage("Telnet client exited");
          break;                 // s.client is gone, stop reading from this session
        }
        s.client.print(TELNET_PROMPT);
      } else if (c >= 32 && c < 127 && s.len < sizeof(s.buf) - 1) {
        s.buf[s.len++] = (char)c;
      }
    }
  }
}

// ============================================================================
//  Web UI (static, PROGMEM). All texts are translated in the browser.
// ============================================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>LED Clock</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#121212;color:#e0e0e0;text-align:center;padding:20px;margin:0}
h1{color:#4CAF50;margin:10px 0}
h3{color:#4CAF50;margin:5px 0 10px}
.card{background:#1e1e1e;border-radius:12px;padding:20px;margin:15px auto;max-width:420px;box-shadow:0 4px 6px rgba(0,0,0,.3)}
.switch{position:relative;display:inline-block;width:60px;height:34px;margin:10px}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;transition:.4s;border-radius:34px}
.slider:before{position:absolute;content:"";height:26px;width:26px;left:4px;bottom:4px;background:#fff;transition:.4s;border-radius:50%}
input:checked+.slider{background:#4CAF50}
input:checked+.slider:before{transform:translateX(26px)}
button{background:#4CAF50;color:#fff;border:none;padding:12px 24px;border-radius:8px;font-size:16px;cursor:pointer;margin:5px}
button:hover{background:#45a049}
button.danger{background:#ff6b6b}
button.danger:hover{background:#ee5a52}
input[type=text],input[type=password],input[type=time],select{width:90%;padding:10px;margin:8px 0;border-radius:6px;border:1px solid #333;background:#2a2a2a;color:#fff;font-size:14px;box-sizing:border-box}
input[type=range]{width:100%;height:8px;border-radius:4px;background:#333;outline:none;-webkit-appearance:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:20px;height:20px;border-radius:50%;background:#4CAF50;cursor:pointer}
.slider-label{display:flex;justify-content:space-between;margin-bottom:5px}
.status{font-size:14px;color:#888;margin-top:10px;min-height:1em}
.info{font-size:13px;color:#aaa;margin:5px 0}
.hint{font-size:12px;color:#777;margin:5px 0}
.divider{border-top:1px solid #333;margin:15px 0}
#nightScheduleFields{display:none}
.row{display:flex;align-items:center;justify-content:center;gap:8px;margin:8px 0}
.row label{font-size:14px;color:#ccc}
.error{color:#ff6b6b}
.success{color:#4CAF50}
.tab{display:none}
.tab.active{display:block}
.tabs{margin:20px 0}
.tab-button{padding:10px 14px;margin:3px;background:#2a2a2a;border:none;color:#aaa;cursor:pointer;border-radius:6px;font-size:14px}
.tab-button.active{background:#1e1e1e;color:#4CAF50}
.tab-button:hover{background:#333}
.log-container{background:#000;color:#0f0;font-family:monospace;padding:10px;border-radius:6px;text-align:left;max-height:300px;overflow-y:auto;white-space:pre-wrap;font-size:12px;min-height:60px}
.version{font-size:12px;color:#888;margin:0}
code{color:#9ccc65}
</style>
</head>
<body>
<h1 data-i="title">LED Clock</h1>
<p class="version"><span data-i="version"></span>: <span id="fwVersion">-</span> &middot; <span id="clockNow">--:--:--</span></p>

<div class="tabs">
  <button class="tab-button active" data-tab="main" data-i="tabClock"></button>
  <button class="tab-button" data-tab="wifi" data-i="tabWifi"></button>
  <button class="tab-button" data-tab="telnet" data-i="tabTelnet"></button>
  <button class="tab-button" data-tab="ota" data-i="tabOta"></button>
  <button class="tab-button" data-tab="logs" data-i="tabLogs"></button>
  <button class="tab-button" data-tab="settings" data-i="tabSettings"></button>
</div>

<div id="main" class="tab active">
  <div class="card">
    <h3 data-i="night"></h3>
    <label class="switch"><input type="checkbox" id="night"><span class="slider"></span></label>
    <p class="hint" id="nightHintText" data-i="nightHint"></p>
    <div class="divider"></div>
    <div class="row">
      <label class="switch"><input type="checkbox" id="nightAuto"><span class="slider"></span></label>
      <span data-i="nightAutoLabel"></span>
    </div>
    <div id="nightScheduleFields">
      <div class="row"><label data-i="nightStart"></label><input type="time" id="nightStart" style="width:auto"></div>
      <div class="row"><label data-i="nightEnd"></label><input type="time" id="nightEnd" style="width:auto"></div>
      <button id="nightScheduleBtn" data-i="apply"></button>
      <p id="nightScheduleStatus" class="status"></p>
    </div>
  </div>
  <div class="card">
    <h3 data-i="brightness"></h3>
    <div class="slider-label"><span>1%</span><span id="brightnessValue">100%</span></div>
    <input type="range" id="brightness" min="1" max="100" value="100">
    <p class="hint" id="brightnessCapHint"></p>
  </div>
  <div class="card">
    <h3 data-i="blink"></h3>
    <label class="switch"><input type="checkbox" id="blink"><span class="slider"></span></label>
  </div>
  <div class="card">
    <h3 data-i="tz"></h3>
    <select id="tzSel"></select>
  </div>
  <div class="card">
    <p class="info"><span data-i="ntpServer"></span>: <span id="ntpServerInfo">-</span></p>
    <p class="info"><span data-i="lastSync"></span>: <span id="lastSyncInfo">-</span></p>
    <button id="syncBtn" data-i="sync"></button>
    <p id="syncStatus" class="status"></p>
  </div>
</div>

<div id="wifi" class="tab">
  <div class="card">
    <h3 data-i="wifiSetup"></h3>
    <p class="info"><span data-i="currentNet"></span>: <span id="currentSSID">-</span></p>
    <p class="info"><span data-i="ipAddr"></span>: <span id="ipAddr">-</span></p>
    <p class="info"><span data-i="rssi"></span>: <span id="rssi">-</span></p>
    <input type="text" id="newSSID" maxlength="32" data-ph="ssid" autocomplete="off">
    <input type="password" id="newPassword" maxlength="64" data-ph="password" autocomplete="new-password">
    <button id="saveWifiBtn" data-i="saveRestart"></button>
    <p id="wifiStatus" class="status"></p>
  </div>
  <div class="card" style="background:#3d1f1f">
    <h3 style="color:#ff6b6b" data-i="resetWifi"></h3>
    <p class="info"><span data-i="apName"></span>: Clock-Setup (192.168.4.1)</p>
    <button class="danger" id="resetBtn" data-i="resetWifi"></button>
    <p id="resetStatus" class="status"></p>
  </div>
</div>

<div id="telnet" class="tab">
  <div class="card">
    <h3 data-i="telnetTitle"></h3>
    <p class="info"><span data-i="status"></span>: <span id="telnetStatus">-</span></p>
    <p class="info"><span data-i="port"></span>: 23</p>
    <p class="hint"><code>telnet <span id="telnetHost">192.168.4.1</span></code></p>
    <p class="hint" data-i="telnetCmds"></p>
    <label class="switch"><input type="checkbox" id="telnetToggle"><span class="slider"></span></label>
  </div>
</div>

<div id="ota" class="tab">
  <div class="card">
    <h3 data-i="otaNetTitle"></h3>
    <p class="info"><span data-i="hostname"></span>: <code id="otaHost">-</code></p>
    <p class="info"><span data-i="port"></span>: 8266</p>
    <p class="hint" data-i="otaNetHint"></p>
  </div>
  <div class="card">
    <h3 data-i="otaBrowserTitle"></h3>
    <p class="hint" data-i="otaBrowserHint"></p>
    <a id="otaBrowserLink" href="/firmware" target="_blank" rel="noopener"><button type="button" data-i="otaBrowserBtn"></button></a>
  </div>
</div>

<div id="logs" class="tab">
  <div class="card">
    <h3 data-i="tabLogs"></h3>
    <div class="log-container" id="logContainer"></div>
    <button id="refreshLogsBtn" data-i="refresh"></button>
    <button id="clearLogsBtn" data-i="clear"></button>
    <button id="downloadLogsBtn" data-i="download"></button>
  </div>
</div>

<div id="settings" class="tab">
  <div class="card">
    <h3 data-i="language"></h3>
    <select id="langSel"><option value="ru">Русский</option><option value="en">English</option></select>
  </div>
  <div class="card">
    <h3 data-i="tzCustom"></h3>
    <input type="text" id="tzCustom" maxlength="47" data-ph="tzCustomPh" autocomplete="off">
    <button id="tzApplyBtn" data-i="apply"></button>
    <p class="hint">POSIX TZ, e.g. <code>EST5EDT,M3.2.0,M11.1.0</code></p>
  </div>
  <div class="card">
    <h3 data-i="ntpTitle"></h3>
    <input type="text" id="ntpCustom" maxlength="47" data-ph="ntpPh" autocomplete="off">
    <button id="ntpApplyBtn" data-i="apply"></button>
    <p class="hint" data-i="ntpHint"></p>
    <p id="ntpStatus" class="status"></p>
  </div>
  <div class="card">
    <h3 data-i="maxBrightnessTitle"></h3>
    <div class="slider-label"><span>1%</span><span id="maxBrightnessValue">100%</span></div>
    <input type="range" id="maxBrightness" min="1" max="100" value="100">
    <p class="hint" data-i="maxBrightnessHint"></p>
  </div>
  <div class="card">
    <h3 data-i="apPasswordTitle"></h3>
    <input type="password" id="apPassword" maxlength="64" data-ph="apPasswordPh" autocomplete="new-password">
    <button id="apPassBtn" data-i="apPasswordSet"></button>
    <p id="apPasswordStatus" class="status"></p>
  </div>
  <p id="settingsStatus" class="status"></p>
</div>

<script>
const I18N={
ru:{title:'LED Часы',version:'Версия',tabClock:'Часы',tabWifi:'Wi-Fi',tabTelnet:'Telnet',tabOta:'OTA',tabLogs:'Логи',tabSettings:'Настройки',
night:'Ночной режим',nightHint:'50% от лимита максимальной яркости',nightAutoLabel:'По расписанию',nightStart:'Начало',nightEnd:'Конец',
nightActiveNow:'Сейчас активен',nightInactiveNow:'Сейчас не активен',brightness:'Яркость',blink:'Мигание точек',tz:'Часовой пояс',
sync:'Синхронизировать время',syncing:'Синхронизация времени...',synced:'Время синхронизировано!',syncFail:'NTP сервер недоступен',
ntpServer:'NTP сервер',lastSync:'Последняя синхронизация',ntpTitle:'NTP сервер',ntpPh:'например time.google.com',ntpHint:'Резервные серверы используются автоматически, если этот недоступен',err_ntp_bad:'Ошибка: неверное имя NTP сервера',
maxBrightnessTitle:'Ограничение максимальной яркости',maxBrightnessHint:'Защитный потолок: реальная яркость никогда не превысит это значение, даже если ползунок на вкладке «Часы» выставлен выше',
brightnessCapped:'Ограничена потолком до',
wifiSetup:'Настройка Wi-Fi',currentNet:'Текущая сеть',ipAddr:'IP адрес',rssi:'Сигнал',ssid:'Имя Wi-Fi сети (SSID)',password:'Пароль Wi-Fi',
saveRestart:'Сохранить и перезагрузить',savedRestart:'Сохранено! Перезагрузка...',resetWifi:'Сброс Wi-Fi настроек',
resetConfirm:'Вы уверены? Часы сбросят настройки Wi-Fi и создадут точку доступа',resetting:'Сброс...',apName:'Точка доступа',notConnected:'Не подключено',
telnetTitle:'Telnet сервер',status:'Статус',port:'Порт',on:'Включено',off:'Выключено',telnetCmds:'Команды: help',
hostname:'Имя',
otaNetTitle:'Сетевая загрузка (Arduino IDE / PlatformIO)',otaNetHint:'В Tools -> Port выберите сетевой порт с этим именем. Пароль задаётся в прошивке (OTA_PASSWORD).',
otaBrowserTitle:'Загрузка через браузер',otaBrowserHint:'Откроет страницу выбора .bin файла. Логин admin, пароль тот же, что и OTA_PASSWORD в прошивке.',otaBrowserBtn:'Открыть страницу обновления',
refresh:'Обновить',clear:'Очистить логи',download:'Скачать логи',
language:'Язык',tzCustom:'Свой часовой пояс (POSIX TZ)',tzCustomPh:'например CET-1CEST,M3.5.0,M10.5.0/3',apply:'Применить',
apPasswordTitle:'Пароль точки доступа',apPasswordPh:'Новый пароль (8-63 символа)',apPasswordSet:'Задать пароль',apPasswordSaved:'Сохранено, применится при следующем запуске AP',
saved:'Сохранено',error:'Ошибка',
err_ssid_empty:'Ошибка: введите имя сети!',err_ssid_long:'Ошибка: SSID слишком длинный!',err_pass_short:'Ошибка: пароль короче 8 символов',
err_pass_long:'Ошибка: пароль слишком длинный!',err_ap_pass_bad:'Ошибка: пароль должен быть 8-63 символа',err_tz_bad:'Ошибка: неверный часовой пояс',
err_bad_request:'Ошибка: неверный запрос',err_forbidden:'Ошибка: запрос отклонён',err_network:'Нет связи с часами',err_time_bad:'Ошибка: неверное время (ЧЧ:ММ)'},
en:{title:'LED Clock',version:'Version',tabClock:'Clock',tabWifi:'Wi-Fi',tabTelnet:'Telnet',tabOta:'OTA',tabLogs:'Logs',tabSettings:'Settings',
night:'Night Mode',nightHint:'50% of the brightness ceiling',nightAutoLabel:'On schedule',nightStart:'Start',nightEnd:'End',
nightActiveNow:'Currently active',nightInactiveNow:'Currently inactive',brightness:'Brightness',blink:'Blink Dots',tz:'Time Zone',
sync:'Sync Time',syncing:'Syncing time...',synced:'Time synchronized!',syncFail:'NTP server unavailable',
ntpServer:'NTP server',lastSync:'Last sync',ntpTitle:'NTP Server',ntpPh:'e.g. time.google.com',ntpHint:'Fallback servers are used automatically if this one is unreachable',err_ntp_bad:'Error: invalid NTP server name',
maxBrightnessTitle:'Max Brightness Limit',maxBrightnessHint:'Safety ceiling: actual brightness never exceeds this, even if the slider on the Clock tab is set higher',
brightnessCapped:'Capped at',
wifiSetup:'Wi-Fi Setup',currentNet:'Current Network',ipAddr:'IP Address',rssi:'Signal',ssid:'Wi-Fi Name (SSID)',password:'Wi-Fi Password',
saveRestart:'Save and Restart',savedRestart:'Saved! Restarting...',resetWifi:'Reset Wi-Fi Settings',
resetConfirm:'Are you sure? This will reset Wi-Fi settings and create an AP',resetting:'Resetting...',apName:'Access point',notConnected:'Not connected',
telnetTitle:'Telnet Server',status:'Status',port:'Port',on:'On',off:'Off',telnetCmds:'Commands: help',
hostname:'Hostname',
otaNetTitle:'Network Upload (Arduino IDE / PlatformIO)',otaNetHint:'Pick the matching network port under Tools -> Port. The password is set in the firmware (OTA_PASSWORD).',
otaBrowserTitle:'Browser Upload',otaBrowserHint:'Opens a page to pick a .bin file. Username admin, password is the same as OTA_PASSWORD in the firmware.',otaBrowserBtn:'Open Update Page',
refresh:'Refresh',clear:'Clear Logs',download:'Download Logs',
language:'Language',tzCustom:'Custom time zone (POSIX TZ)',tzCustomPh:'e.g. CET-1CEST,M3.5.0,M10.5.0/3',apply:'Apply',
apPasswordTitle:'AP Password',apPasswordPh:'New password (8-63 chars)',apPasswordSet:'Set AP Password',apPasswordSaved:'Saved, applies the next time the AP starts',
saved:'Saved',error:'Error',
err_ssid_empty:'Error: enter SSID!',err_ssid_long:'Error: SSID too long!',err_pass_short:'Error: password shorter than 8 characters',
err_pass_long:'Error: password too long!',err_ap_pass_bad:'Error: password must be 8-63 characters',err_tz_bad:'Error: invalid time zone',
err_bad_request:'Error: bad request',err_forbidden:'Error: request rejected',err_network:'Cannot reach the clock',err_time_bad:'Error: invalid time (HH:MM)'}
};
const TZ_DST=[['GMT0BST,M3.5.0/1,M10.5.0','London'],['CET-1CEST,M3.5.0,M10.5.0/3','Central Europe'],['EET-2EEST,M3.5.0/3,M10.5.0/4','Eastern Europe'],
['EST5EDT,M3.2.0,M11.1.0','US Eastern'],['CST6CDT,M3.2.0,M11.1.0','US Central'],['MST7MDT,M3.2.0,M11.1.0','US Mountain'],['PST8PDT,M3.2.0,M11.1.0','US Pacific'],
['AEST-10AEDT,M10.1.0,M4.1.0/3','Sydney']];
const $=id=>document.getElementById(id);
let L=(navigator.language||'en').toLowerCase().startsWith('ru')?'ru':'en';
let st=null;
const t=k=>(I18N[L]&&I18N[L][k])||I18N.en[k]||k;
function offTz(h){const a=Math.abs(h);return '<'+(h<0?'-':'+')+(a<10?'0':'')+a+'>'+(-h);}
function say(id,text,ok){const e=$(id);e.textContent=text;e.className='status '+(ok?'success':'error');}
function errText(r){const k='err_'+(r&&r.err?r.err:'');return I18N[L][k]||I18N.en[k]||t('error');}
async function api(path,data){
  try{
    const r=await fetch(path,{method:'POST',headers:{'X-Clock':'1'},body:new URLSearchParams(data||{})});
    return await r.json();
  }catch(e){return {ok:false,err:'network'};}
}
function fillTz(cur){
  const sel=$('tzSel');sel.innerHTML='';
  const add=(v,n)=>{const o=document.createElement('option');o.value=v;o.textContent=n;sel.appendChild(o);};
  TZ_DST.forEach(z=>add(z[0],z[1]));
  for(let h=-12;h<=14;h++)add(offTz(h),'UTC'+(h>=0?'+':'')+h);
  if(![...sel.options].some(o=>o.value===cur))add(cur,cur);
  sel.value=cur;
}
function renderInfo(){
  if(!st)return;
  $('fwVersion').textContent=st.version;
  $('clockNow').textContent=st.time;
  $('currentSSID').textContent=st.ssid||'-';
  $('ipAddr').textContent=st.ip||t('notConnected');
  $('rssi').textContent=st.connected?st.rssi+' dBm':'-';
  $('otaHost').textContent=st.hostname+'.local';
  $('telnetStatus').textContent=st.telnet?t('on'):t('off');
  $('telnetHost').textContent=st.ip||'192.168.4.1';
  $('ntpServerInfo').textContent=st.ntpServer||'-';
  $('lastSyncInfo').textContent=st.lastSync||'-';
  $('nightAuto').checked=!!st.nightAuto;
  $('night').disabled=!!st.nightAuto;
  $('night').checked=st.nightAuto?!!st.nightActive:!!st.night;
  $('nightScheduleFields').style.display=st.nightAuto?'block':'none';
  $('nightHintText').textContent=st.nightAuto?(st.nightActive?t('nightActiveNow'):t('nightInactiveNow')):t('nightHint');
  $('brightnessCapHint').textContent=(st.maxBrightness<st.brightness)?(t('brightnessCapped')+' '+st.maxBrightness+'%'):'';
}
function applyLang(){
  document.documentElement.lang=L;
  document.title=t('title');
  document.querySelectorAll('[data-i]').forEach(e=>{e.textContent=t(e.dataset.i);});
  document.querySelectorAll('[data-ph]').forEach(e=>{e.placeholder=t(e.dataset.ph);});
  $('langSel').value=L;
  renderInfo();
}
async function loadStatus(full){
  try{const r=await fetch('/status');st=await r.json();}catch(e){return;}
  if(full){
    L=st.lang==='en'?'en':'ru';
    $('night').checked=st.night;$('blink').checked=st.blink;
    $('brightness').value=st.brightness;$('brightnessValue').textContent=st.brightness+'%';
    $('maxBrightness').value=st.maxBrightness;$('maxBrightnessValue').textContent=st.maxBrightness+'%';
    $('telnetToggle').checked=st.telnet;
    fillTz(st.tz);
    $('ntpCustom').value=st.ntpServer||'';
    $('nightAuto').checked=!!st.nightAuto;
    $('nightStart').value=st.nightStart||'22:00';
    $('nightEnd').value=st.nightEnd||'07:00';
    applyLang();
  }else{renderInfo();}
}
function showTab(name,btn){
  document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));
  document.querySelectorAll('.tab-button').forEach(x=>x.classList.remove('active'));
  $(name).classList.add('active');btn.classList.add('active');
  if(name==='logs')refreshLogs();
}
document.querySelectorAll('.tab-button').forEach(b=>b.addEventListener('click',()=>showTab(b.dataset.tab,b)));

$('night').onchange=e=>api('/set',{night:e.target.checked?1:0});
$('nightAuto').onchange=async e=>{
  const want=e.target.checked;
  const r=await api('/set',{nightAuto:want?1:0});
  if(r.ok){st.nightAuto=want;renderInfo();}else e.target.checked=!want;
};
$('nightScheduleBtn').onclick=async()=>{
  const a=$('nightStart').value,b=$('nightEnd').value;
  if(!a||!b)return;
  const r=await api('/set',{nightStart:a,nightEnd:b});
  if(r.ok){say('nightScheduleStatus',t('saved'),true);await loadStatus(false);}else say('nightScheduleStatus',errText(r),false);
};
$('blink').onchange=e=>api('/set',{blink:e.target.checked?1:0});
$('brightness').oninput=e=>{$('brightnessValue').textContent=e.target.value+'%';};
$('brightness').onchange=e=>api('/set',{brightness:e.target.value}).then(()=>loadStatus(false));
$('maxBrightness').oninput=e=>{$('maxBrightnessValue').textContent=e.target.value+'%';};
$('maxBrightness').onchange=e=>api('/set',{maxBrightness:e.target.value}).then(()=>loadStatus(false));
$('tzSel').onchange=e=>api('/set',{tz:e.target.value});
$('langSel').onchange=async e=>{
  const r=await api('/set',{lang:e.target.value});
  if(r.ok){L=e.target.value;applyLang();say('settingsStatus',t('saved'),true);}
  else say('settingsStatus',errText(r),false);
};
$('tzApplyBtn').onclick=async()=>{
  const v=$('tzCustom').value.trim();if(!v)return;
  const r=await api('/set',{tz:v});
  if(r.ok){fillTz(v);say('settingsStatus',t('saved'),true);}else say('settingsStatus',errText(r),false);
};
$('ntpApplyBtn').onclick=async()=>{
  const v=$('ntpCustom').value.trim();if(!v)return;
  const r=await api('/set',{ntp:v});
  if(r.ok){say('ntpStatus',t('saved'),true);await loadStatus(false);}else say('ntpStatus',errText(r),false);
};
$('syncBtn').onclick=async()=>{
  say('syncStatus',t('syncing'),true);
  const before=st?st.syncCount:0;
  const r=await api('/sync');
  if(!r.ok){say('syncStatus',errText(r),false);return;}
  for(let i=0;i<12;i++){
    await new Promise(z=>setTimeout(z,1000));
    await loadStatus(false);
    if(st&&st.syncCount!==before){say('syncStatus',t('synced'),true);return;}
  }
  say('syncStatus',t('syncFail'),false);
};
$('saveWifiBtn').onclick=async()=>{
  const r=await api('/wifi',{ssid:$('newSSID').value,password:$('newPassword').value});
  say('wifiStatus',r.ok?t('savedRestart'):errText(r),!!r.ok);
};
$('resetBtn').onclick=async()=>{
  if(!confirm(t('resetConfirm')))return;
  say('resetStatus',t('resetting'),true);
  const r=await api('/reset-wifi');
  if(!r.ok)say('resetStatus',errText(r),false);
};
$('telnetToggle').onchange=async e=>{
  const want=e.target.checked;
  const r=await api('/set',{telnet:want?1:0});
  if(r.ok){st.telnet=want;renderInfo();}else e.target.checked=!want;
};
async function refreshLogs(){
  try{const r=await fetch('/logs');$('logContainer').textContent=await r.text();}catch(e){}
}
$('refreshLogsBtn').onclick=refreshLogs;
$('clearLogsBtn').onclick=async()=>{await api('/clear-logs');refreshLogs();};
$('downloadLogsBtn').onclick=async()=>{
  const r=await fetch('/logs');
  const u=URL.createObjectURL(new Blob([await r.text()],{type:'text/plain'}));
  const a=document.createElement('a');a.href=u;a.download='clock_logs.txt';
  document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(u);
};
$('apPassBtn').onclick=async()=>{
  const r=await api('/ap-password',{password:$('apPassword').value});
  if(r.ok){$('apPassword').value='';say('apPasswordStatus',t('apPasswordSaved'),true);}
  else say('apPasswordStatus',errText(r),false);
};
applyLang();
loadStatus(true);
setInterval(()=>loadStatus(false),5000);
setInterval(()=>{if($('logs').classList.contains('active'))refreshLogs();},5000);
</script>
</body>
</html>
)rawliteral";

// ============================================================================
//  Web handlers
// ============================================================================
static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendOk() {
  sendJson(200, "{\"ok\":true}");
}

static void sendErr(int code, const char* err) {
  sendJson(code, String("{\"ok\":false,\"err\":\"") + err + "\"}");
}

// State changing requests must carry a custom header. Browsers refuse to send it
// cross-origin without a CORS preflight, which this server never approves.
static bool requireAjax() {
  touchAP();
  if (!server.hasHeader("X-Clock")) {
    sendErr(403, "forbidden");
    return false;
  }
  return true;
}

static void handleRoot() {
  touchAP();
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  touchAP();
  String ip = "";
  if (WiFi.status() == WL_CONNECTED) ip = WiFi.localIP().toString();
  else if (apActive)                 ip = WiFi.softAPIP().toString();

  String j;
  j.reserve(360);
  j += "{\"night\":";      j += config.nightMode ? "true" : "false";
  j += ",\"blink\":";      j += config.blinkDots ? "true" : "false";
  j += ",\"telnet\":";     j += config.telnetEnabled ? "true" : "false";
  j += ",\"brightness\":"; j += String(config.brightness);
  j += ",\"maxBrightness\":"; j += String(config.maxBrightness);
  j += ",\"lang\":\"";     j += (config.language == 0 ? "ru" : "en"); j += "\"";
  j += ",\"tz\":\"";       j += jsonEscape(String(config.tz)); j += "\"";
  j += ",\"ntpServer\":\""; j += jsonEscape(String(config.ntpServer)); j += "\"";
  j += ",\"lastSync\":\""; j += formatTime(lastSyncTime); j += "\"";
  j += ",\"nightAuto\":";  j += config.nightAutoEnabled ? "true" : "false";
  j += ",\"nightStart\":\""; j += formatHHMM(config.nightStartHour, config.nightStartMinute); j += "\"";
  j += ",\"nightEnd\":\"";   j += formatHHMM(config.nightEndHour, config.nightEndMinute); j += "\"";
  j += ",\"nightActive\":";  j += effectiveNightMode() ? "true" : "false";
  j += ",\"ip\":\"";       j += ip; j += "\"";
  j += ",\"ssid\":\"";     j += jsonEscape(String(config.ssid)); j += "\"";
  j += ",\"connected\":";  j += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  j += ",\"rssi\":";       j += String(WiFi.RSSI());
  j += ",\"ap\":";         j += apActive ? "true" : "false";
  j += ",\"hostname\":\""; j += hostName; j += "\"";
  j += ",\"syncCount\":";  j += String((uint32_t)syncCount);
  j += ",\"time\":\"";     j += timeString(); j += "\"";
  j += ",\"version\":\"" FIRMWARE_VERSION "\"}";
  sendJson(200, j);
}

static void handleSet() {
  if (!requireAjax()) return;

  // validate everything first, apply afterwards
  long brightness = -1;
  if (server.hasArg("brightness")) {
    brightness = server.arg("brightness").toInt();
    if (brightness < 1 || brightness > 100) { sendErr(400, "bad_request"); return; }
  }
  long maxBrightness = -1;
  if (server.hasArg("maxBrightness")) {
    maxBrightness = server.arg("maxBrightness").toInt();
    if (maxBrightness < 1 || maxBrightness > 100) { sendErr(400, "bad_request"); return; }
  }
  int lang = -1;
  if (server.hasArg("lang")) {
    const String l = server.arg("lang");
    if (l == "ru") lang = 0;
    else if (l == "en") lang = 1;
    else { sendErr(400, "bad_request"); return; }
  }
  String tz = "";
  if (server.hasArg("tz")) {
    tz = server.arg("tz");
    if (!validTz(tz)) { sendErr(400, "tz_bad"); return; }
  }
  String ntp = "";
  if (server.hasArg("ntp")) {
    ntp = server.arg("ntp");
    if (!validNtpServer(ntp)) { sendErr(400, "ntp_bad"); return; }
  }
  bool haveStart = false, haveEnd = false;
  uint8_t nsH = 0, nsM = 0, neH = 0, neM = 0;
  if (server.hasArg("nightStart")) {
    if (!parseHHMM(server.arg("nightStart"), nsH, nsM)) { sendErr(400, "time_bad"); return; }
    haveStart = true;
  }
  if (server.hasArg("nightEnd")) {
    if (!parseHHMM(server.arg("nightEnd"), neH, neM)) { sendErr(400, "time_bad"); return; }
    haveEnd = true;
  }

  bool changed = false;
  bool ntpChanged = false;
  if (server.hasArg("night"))  { config.nightMode = server.arg("night") == "1"; changed = true; }
  if (server.hasArg("blink"))  { config.blinkDots = server.arg("blink") == "1"; forceRedraw = true; changed = true; }
  if (brightness > 0)          { config.brightness = (uint8_t)brightness; changed = true; }
  if (maxBrightness > 0)       { config.maxBrightness = (uint8_t)maxBrightness; changed = true; }
  if (lang >= 0)               { config.language = (uint8_t)lang; changed = true; }
  if (tz.length() > 0)         { copyStr(config.tz, sizeof(config.tz), tz); applyTimezone(); changed = true; }
  if (ntp.length() > 0)        { copyStr(config.ntpServer, sizeof(config.ntpServer), ntp); changed = true; ntpChanged = true; }
  if (server.hasArg("telnet")) { config.telnetEnabled = server.arg("telnet") == "1"; applyTelnet(); changed = true; }
  if (server.hasArg("nightAuto")) { config.nightAutoEnabled = server.arg("nightAuto") == "1"; changed = true; }
  if (haveStart)                { config.nightStartHour = nsH; config.nightStartMinute = nsM; changed = true; }
  if (haveEnd)                  { config.nightEndHour = neH;   config.nightEndMinute = neM;   changed = true; }

  if (changed) {
    updateDuty();
    markConfigDirty();
  }
  if (ntpChanged && (WiFi.status() == WL_CONNECTED)) startSntp();   // apply the new server right away
  sendOk();
}

static void handleSync() {
  if (!requireAjax()) return;
  startSntp();
  sendOk();
}

static void handleWiFi() {
  if (!requireAjax()) return;
  if (!server.hasArg("ssid") || !server.hasArg("password")) { sendErr(400, "bad_request"); return; }
  const String ssid = server.arg("ssid");
  const String pass = server.arg("password");
  if (ssid.length() == 0)                 { sendErr(400, "ssid_empty"); return; }
  if (ssid.length() > sizeof(config.ssid) - 1) { sendErr(400, "ssid_long"); return; }
  if (pass.length() > 64)                 { sendErr(400, "pass_long"); return; }
  if (pass.length() > 0 && !validWpaPass(pass)) { sendErr(400, "pass_short"); return; }

  copyStr(config.ssid, sizeof(config.ssid), ssid);
  copyStr(config.password, sizeof(config.password), pass);
  config.configured = 1;
  saveConfigNow();
  saveWifiCreds();
  logMessage("Wi-Fi credentials updated");
  sendOk();
  scheduleRestart(2500);
}

static void handleResetWiFi() {
  if (!requireAjax()) return;
  config.ssid[0] = '\0';
  config.password[0] = '\0';
  config.configured = 0;
  saveConfigNow();
  saveWifiCreds();
  logMessage("Wi-Fi configuration reset");
  sendOk();
  scheduleRestart(2000);
}

static void handleLogs() {
  touchAP();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", logBuf);
}

static void handleClearLogs() {
  if (!requireAjax()) return;
  clearLogs();
  sendOk();
}

static void handleSetAPPassword() {
  if (!requireAjax()) return;
  const String p = server.arg("password");
  if (!validWpaPass(p)) { sendErr(400, "ap_pass_bad"); return; }
  copyStr(config.apPassword, sizeof(config.apPassword), p);
  markConfigDirty();
  logMessage("AP password updated");
  sendOk();
}

static void handleNotFound() {
  touchAP();
  server.send(404, "text/plain", "Not found");
}

// ============================================================================
//  setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);
  digitalWrite(DATA_PIN, LOW);
  digitalWrite(CLOCK_PIN, LOW);
  digitalWrite(LATCH_PIN, HIGH);

  logMessage(String("=== LED Clock firmware v") + FIRMWARE_VERSION + " ===");
  logMessage(String("Chip ID: ") + String(ESP.getChipId(), HEX) + ", free heap: " + ESP.getFreeHeap());

  loadConfig();
  applyTimezone();
  updateDuty();
  clockTask();                 // dashes until the time is known
  startDisplayTimer();

  hostName = String("LED-Clock-") + String(ESP.getChipId(), HEX);
  settimeofday_cb(onTimeSet);

  WiFi.persistent(false);      // we keep credentials in our own EEPROM block
  WiFi.setAutoReconnect(true);
  WiFi.hostname(hostName);
  if (config.configured) {
    logMessage(String("Connecting to Wi-Fi: ") + config.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid, config.password);
    staDownSince = millis();
    lastStaBegin = millis();
  } else {
    logMessage("Wi-Fi not configured, starting AP");
    startAP();
  }

  setupOTA();

  static const char* hdrKeys[] = {"X-Clock"};
  server.collectHeaders(hdrKeys, (size_t)1);
  server.on("/",             HTTP_GET,  handleRoot);
  server.on("/status",       HTTP_GET,  handleStatus);
  server.on("/logs",         HTTP_GET,  handleLogs);
  server.on("/set",          HTTP_POST, handleSet);
  server.on("/sync",         HTTP_POST, handleSync);
  server.on("/wifi",         HTTP_POST, handleWiFi);
  server.on("/reset-wifi",   HTTP_POST, handleResetWiFi);
  server.on("/clear-logs",   HTTP_POST, handleClearLogs);
  server.on("/ap-password",  HTTP_POST, handleSetAPPassword);
  server.onNotFound(handleNotFound);
  httpUpdater.setup(&server, HTTP_UPDATE_PATH, HTTP_UPDATE_USER, OTA_PASSWORD);
  server.begin();
  logMessage(String("Web server started on port 80, browser OTA at ") + HTTP_UPDATE_PATH);

  applyTelnet();
  logMessage("Setup complete");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (mdnsStarted) MDNS.update();
  handleTelnet();
  wifiTask();
  handlePendingRestart();
  commitConfigIfDue();
  clockTask();
}
