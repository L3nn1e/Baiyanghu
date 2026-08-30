#include <ESP8266WiFi.h>
#include <time.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiClient.h>
#include <ESP8266TelnetServer.h>

// ================= VERSION =================
const char* FIRMWARE_VERSION = "2.0.0";

// ================= LANGUAGE SUPPORT =================
enum Language { RU, EN };
Language currentLanguage = RU;

// Language strings
const char* LANG_RU[] = {
  "LED Часы",
  "Настройка часов",
  "Ночной режим",
  "Яркость",
  "Мигание точек",
  "Часовой пояс",
  "Настройка Wi-Fi",
  "Текущая сеть",
  "Имя Wi-Fi сети (SSID)",
  "Пароль Wi-Fi",
  "Сохранить и перезагрузить",
  "Синхронизировать время",
  "Сброс Wi-Fi настроек",
  "IP адрес",
  "Не найдено",
  "Синхронизация времени...",
  "Время синхронизировано!",
  "Ошибка: введите имя сети!",
  "Ошибка: SSID слишком длинный!",
  "Ошибка: пароль слишком длинный!",
  "Ошибка: не указаны SSID или пароль",
  "Сохранено! Перезагрузка через 3 сек...",
  "Настройки сброшены! Перезагрузка...",
  "Вы уверены? Часы сбросят настройки Wi-Fi и создадут точку доступа",
  "Сброс...",
  "Ошибка",
  "OK",
  "Обновление прошивки",
  "Текущая версия",
  "Проверка обновлений...",
  "Доступно обновление!",
  "Прошивка актуальна",
  "Логи",
  "Очистить логи",
  "Скачать логи",
  "Назад",
  "Сохранить",
  "Отмена",
  "Язык",
  "Русский",
  "Английский",
  "Включено",
  "Выключено",
  "Слабый сигнал Wi-Fi",
  "Не удалось подключиться к Wi-Fi",
  "NTP сервер недоступен",
  "\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u0443\u0435\u0442\u0441\u044f \u0440\u0435\u0437\u0435\u0440\u0432\u043d\u044b\u0439 NTP \u0441\u0435\u0440\u0432\u0435\u0440",
  "Telnet \u0441\u0435\u0440\u0432\u0435\u0440",
  "Telnet \u0432\u043a\u043b\u044e\u0447\u0435\u043d \u043d\u0430 \u043f\u043e\u0440\u0442\u0443 23",
  "Telnet \u043e\u0442\u043a\u043b\u044e\u0447\u0435\u043d",
  "\u0412\u043a\u043b\u044e\u0447\u0438\u0442\u044c Telnet",
  "\u041e\u0442\u043a\u043b\u044e\u0447\u0438\u0442\u044c Telnet",
  "\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u043e \u0447\u0435\u0440\u0435\u0437 Telnet",
  "Telnet \u043e\u0442\u0441\u043e\u0435\u0434\u0438\u043d\u0435\u043d\u043e"
};

const char* LANG_EN[] = {
  "LED Clock",
  "Clock Setup",
  "Night Mode",
  "Brightness",
  "Blink Dots",
  "Time Zone",
  "Wi-Fi Setup",
  "Current Network",
  "Wi-Fi Name (SSID)",
  "Wi-Fi Password",
  "Save and Restart",
  "Sync Time",
  "Reset Wi-Fi Settings",
  "IP Address",
  "Not found",
  "Syncing time...",
  "Time synchronized!",
  "Error: Enter SSID!",
  "Error: SSID too long!",
  "Error: Password too long!",
  "Error: SSID or password not specified",
  "Saved! Restarting in 3 sec...",
  "Settings reset! Restarting...",
  "Are you sure? This will reset Wi-Fi settings and create an AP",
  "Resetting...",
  "Error",
  "OK",
  "Firmware Update",
  "Current Version",
  "Checking for updates...",
  "Update available!",
  "Firmware is up to date",
  "Logs",
  "Clear Logs",
  "Download Logs",
  "Back",
  "Save",
  "Cancel",
  "Language",
  "Russian",
  "English",
  "On",
  "Off",
  "Weak Wi-Fi Signal",
  "Failed to connect to Wi-Fi",
  "NTP server unavailable",
  "Using fallback NTP server",
  "Telnet Server",
  "Telnet enabled on port 23",
  "Telnet disabled",
  "Enable Telnet",
  "Disable Telnet",
  "Connected via Telnet",
  "Telnet disconnected"
};

const char** getLangStrings() {
  return currentLanguage == RU ? LANG_RU : LANG_EN;
}

// ================= LOGGING =================
#define LOG_BUFFER_SIZE 2048
char logBuffer[LOG_BUFFER_SIZE] = "";

// ================= TELNET SERVER =================
ESP8266TelnetServer telnetServer(23);
bool telnetEnabled = false;
bool telnetConnected = false;

// Telnet clients
WiFiClient telnetClients[4];
int telnetClientCount = 0;
const int MAX_TELNET_CLIENTS = 4;

void logMessage(const char* message, bool isError = false) {
  static unsigned long lastLogTime = 0;
  unsigned long currentTime = millis();
  
  // Add timestamp
  char timestamp[20];
  unsigned long uptime = currentTime / 1000;
  unsigned int hours = uptime / 3600;
  unsigned int minutes = (uptime % 3600) / 60;
  unsigned int seconds = uptime % 60;
  snprintf(timestamp, sizeof(timestamp), "[%02u:%02u:%02u] ", hours, minutes, seconds);
  
  // Format log message
  char logEntry[256];
  const char* prefix = isError ? "[ERROR] " : "[INFO] ";
  snprintf(logEntry, sizeof(logEntry), "%s%s%s\n", timestamp, prefix, message);
  
  // Add to buffer
  int logLen = strlen(logBuffer);
  int entryLen = strlen(logEntry);
  if (logLen + entryLen < LOG_BUFFER_SIZE - 1) {
    strcat(logBuffer, logEntry);
  } else {
    // Shift buffer to make room
    memmove(logBuffer, logBuffer + (logLen - (LOG_BUFFER_SIZE / 2)), LOG_BUFFER_SIZE / 2);
    logBuffer[LOG_BUFFER_SIZE / 2] = '\0';
    strcat(logBuffer, logEntry);
  }
  
  // Print to Serial
  Serial.print(logEntry);
  
  lastLogTime = currentTime;
}

String getLogs() {
  return String(logBuffer);
}

void clearLogs() {
  logBuffer[0] = '\0';
  logMessage("Logs cleared");
}

// ================= TELNET FUNCTIONS =================
void setupTelnet() {
  if (telnetEnabled) {
    telnetServer.begin();
    telnetServer.setNoDelay(true);
    logMessage("Telnet server started on port 23");
  } else {
    telnetServer.close();
    logMessage("Telnet server stopped");
  }
}

void enableTelnet() {
  telnetEnabled = true;
  setupTelnet();
}

void disableTelnet() {
  telnetEnabled = false;
  // Close all existing connections
  for (int i = 0; i < telnetClientCount; i++) {
    telnetClients[i].stop();
  }
  telnetClientCount = 0;
  telnetConnected = false;
  setupTelnet();
}

void handleTelnetClients() {
  if (!telnetEnabled) return;

  // Check for new connections
  if (telnetServer.hasClient()) {
    if (telnetClientCount < MAX_TELNET_CLIENTS) {
      telnetClients[telnetClientCount] = telnetServer.available();
      telnetClients[telnetClientCount].flush();
      telnetClients[telnetClientCount].println(getLangText(40)); // Connected via Telnet
      telnetClients[telnetClientCount].println("Type 'help' for available commands.");
      telnetClientCount++;
      telnetConnected = true;
      logMessage("New Telnet connection. Total clients: " + String(telnetClientCount));
    } else {
      WiFiClient client = telnetServer.available();
      client.println("Server busy. Try again later.");
      client.stop();
    }
  }

  // Check existing clients
  for (int i = 0; i < telnetClientCount; i++) {
    if (telnetClients[i].connected()) {
      // Process incoming commands
      if (telnetClients[i].available()) {
        String command = telnetClients[i].readStringUntil('\n');
        command.trim();
        if (command.length() > 0) {
          processTelnetCommand(telnetClients[i], command);
        }
      }
    } else {
      telnetClients[i].stop();
      // Shift remaining clients
      for (int j = i; j < telnetClientCount - 1; j++) {
        telnetClients[j] = telnetClients[j + 1];
      }
      telnetClientCount--;
      if (telnetClientCount == 0) {
        telnetConnected = false;
      }
      logMessage("Telnet client disconnected. Remaining: " + String(telnetClientCount));
    }
  }
}

void processTelnetCommand(WiFiClient& client, String command) {
  const char** lang = getLangStrings();
  
  if (command.equalsIgnoreCase("help") || command.equalsIgnoreCase("?")) {
    client.println("Available commands:");
    client.println("  time      - Show current time");
    client.println("  date      - Show current date");
    client.println("  wifi      - Show Wi-Fi status");
    client.println("  ip        - Show IP address");
    client.println("  ntp       - Show NTP status");
    client.println("  brightness - Show/get brightness");
    client.println("  brightness X - Set brightness (0-100)");
    client.println("  night on|off - Toggle night mode");
    client.println("  blink on|off - Toggle blink dots");
    client.println("  tz UTC+X  - Set timezone (e.g., UTC+3)");
    client.println("  logs      - Show recent logs");
    client.println("  clear     - Clear logs");
    client.println("  restart   - Restart device");
    client.println("  version   - Show firmware version");
    client.println("  help      - Show this help");
  }
  else if (command.equalsIgnoreCase("time")) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[20];
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
      client.println("Current time: " + String(timeStr));
    } else {
      client.println("Time not synchronized");
    }
  }
  else if (command.equalsIgnoreCase("date")) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char dateStr[20];
      strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
      client.println("Current date: " + String(dateStr));
    } else {
      client.println("Date not available");
    }
  }
  else if (command.equalsIgnoreCase("wifi")) {
    client.println("Wi-Fi Status:");
    client.println("  Connected: " + String(WiFi.status() == WL_CONNECTED ? "Yes" : "No"));
    client.println("  SSID: " + String(WiFi.SSID()));
    client.println("  IP: " + WiFi.localIP().toString());
    client.println("  Signal: " + String(WiFi.RSSI()) + " dBm");
  }
  else if (command.equalsIgnoreCase("ip")) {
    if (WiFi.status() == WL_CONNECTED) {
      client.println("IP Address: " + WiFi.localIP().toString());
    } else if (apActive) {
      client.println("AP IP Address: " + WiFi.softAPIP().toString());
    } else {
      client.println("Not connected");
    }
  }
  else if (command.equalsIgnoreCase("ntp")) {
    client.println("NTP Status:");
    client.println("  Synced: " + String(ntpSynced ? "Yes" : "No"));
    client.println("  Current server: " + String(ntpServers[currentNtpServerIndex]));
    client.println("  Timezone offset: " + String(gmtOffsetSec / 3600) + " hours");
  }
  else if (command.equalsIgnoreCase("brightness")) {
    client.println("Current brightness: " + String(brightness) + "%");
  }
  else if (command.startsWith("brightness ")) {
    int newBrightness = command.substring(11).toInt();
    if (newBrightness >= 0 && newBrightness <= 100) {
      brightness = newBrightness;
      config.brightness = brightness;
      saveConfig();
      client.println("Brightness set to: " + String(brightness) + "%");
    } else {
      client.println("Invalid brightness value (0-100)");
    }
  }
  else if (command.equalsIgnoreCase("night on")) {
    nightMode = true;
    config.nightMode = true;
    saveConfig();
    client.println("Night mode: ON");
  }
  else if (command.equalsIgnoreCase("night off")) {
    nightMode = false;
    config.nightMode = false;
    saveConfig();
    client.println("Night mode: OFF");
  }
  else if (command.equalsIgnoreCase("night")) {
    client.println("Night mode: " + String(nightMode ? "ON" : "OFF"));
  }
  else if (command.equalsIgnoreCase("blink on")) {
    blinkDots = true;
    config.blinkDots = true;
    saveConfig();
    client.println("Blink dots: ON");
  }
  else if (command.equalsIgnoreCase("blink off")) {
    blinkDots = false;
    config.blinkDots = false;
    saveConfig();
    client.println("Blink dots: OFF");
  }
  else if (command.equalsIgnoreCase("blink")) {
    client.println("Blink dots: " + String(blinkDots ? "ON" : "OFF"));
  }
  else if (command.startsWith("tz ")) {
    String tzStr = command.substring(3);
    if (tzStr.startsWith("UTC") || tzStr.startsWith("utc")) {
      int offset = tzStr.substring(3).toInt();
      if (offset >= -12 && offset <= 14) {
        gmtOffsetSec = offset * 3600;
        config.gmtOffsetSec = gmtOffsetSec;
        configTime(gmtOffsetSec, daylightOffset_sec, ntpServers[currentNtpServerIndex]);
        saveConfig();
        client.println("Timezone set to: UTC" + String(offset >= 0 ? "+" : "") + String(offset));
      } else {
        client.println("Invalid timezone offset (-12 to +14)");
      }
    } else {
      client.println("Usage: tz UTC+X (e.g., UTC+3)");
    }
  }
  else if (command.equalsIgnoreCase("logs")) {
    client.println("=== Recent Logs ===");
    client.println(getLogs());
  }
  else if (command.equalsIgnoreCase("clear")) {
    clearLogs();
    client.println("Logs cleared");
  }
  else if (command.equalsIgnoreCase("restart")) {
    client.println("Restarting device...");
    logMessage("Device restart requested via Telnet");
    scheduleRestart(1000);
  }
  else if (command.equalsIgnoreCase("version")) {
    client.println("Firmware version: " + String(FIRMWARE_VERSION));
  }
  else {
    client.println("Unknown command. Type 'help' for available commands.");
  }
}

// ================= AP CONFIG =================
const char* apSSID = "Clock-Setup";
const char* DEFAULT_AP_PASSWORD = "12345678";
const unsigned long apTimeout = 180000; // 3 minutes

IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ================= SHIFT REGISTER PINS =================
const int DATA_PIN  = 13;
const int CLOCK_PIN = 14;
const int LATCH_PIN = 12;

// ================= TIME CONFIG =================
long gmtOffsetSec = 3 * 3600; // Default UTC+3
const int daylightOffset_sec = 0;
const char* ntpServers[] = {
  "pool.ntp.org",
  "time.google.com",
  "time.nist.gov",
  "time.windows.com",
  "0.pool.ntp.org",
  "1.pool.ntp.org",
  "2.pool.ntp.org",
  "3.pool.ntp.org"
};
const int NUM_NTP_SERVERS = 8;
int currentNtpServerIndex = 0;

// ================= EEPROM CONFIG =================
const uint32_t CONFIG_MAGIC = 0xC10CFEED;
const int EEPROM_SIZE = 1024;

struct Config {
  uint32_t magic;
  char ssid[33];
  char password[65];
  char apPassword[65];
  bool configured;
  bool nightMode;
  bool blinkDots;
  long gmtOffsetSec;
  uint8_t brightness; // 0-100%
  Language language;
};

Config config;

// ================= GLOBAL VARIABLES =================
bool blinkDots = true;
bool nightMode = false;
uint8_t brightness = 100; // Default 100%

const byte customDigitMap[10] = {
  0b01111110, 0b00001100, 0b10110110, 0b10011110, 0b11001100,
  0b11011010, 0b11111010, 0b00001110, 0b11111110, 0b11011110
};

ESP8266WebServer server(80);

unsigned long apStartTime = 0;
bool apActive = false;

// Deferred restart
bool pendingRestart = false;
unsigned long restartAtMillis = 0;

// Timing variables
unsigned long lastTimeUpdate = 0;
unsigned long lastPWMStep = 0;
unsigned long lastNtpSyncAttempt = 0;
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long NTP_SYNC_INTERVAL = 3600000; // 1 hour
const unsigned long WIFI_RECONNECT_INTERVAL = 30000; // 30 seconds

int pwmStep = 0;
const int PWM_STEPS = 20;
const unsigned long PWM_STEP_MS = 1;

int curHour = 0, curMinute = 0, curSecond = 0;
bool timeOK = false;
bool ntpSynced = false;

// ================= EEPROM FUNCTIONS =================
void saveConfig() {
  config.magic = CONFIG_MAGIC;
  config.nightMode = nightMode;
  config.blinkDots = blinkDots;
  config.gmtOffsetSec = gmtOffsetSec;
  config.brightness = brightness;
  config.language = currentLanguage;
  
  EEPROM.put(0, config);
  EEPROM.commit();
  logMessage("Configuration saved to EEPROM");
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);

  if (config.magic != CONFIG_MAGIC) {
    logMessage("No valid config found. Using defaults.");
    strncpy(config.ssid, "", sizeof(config.ssid));
    strncpy(config.password, "", sizeof(config.password));
    strncpy(config.apPassword, DEFAULT_AP_PASSWORD, sizeof(config.apPassword));
    config.configured = false;
    config.nightMode = false;
    config.blinkDots = true;
    config.gmtOffsetSec = 3 * 3600;
    config.brightness = 100;
    config.language = RU;
    saveConfig();
  }

  nightMode = config.nightMode;
  blinkDots = config.blinkDots;
  gmtOffsetSec = config.gmtOffsetSec;
  brightness = config.brightness;
  currentLanguage = config.language;

  if (!config.configured || config.ssid[0] == '\0') {
    logMessage("Wi-Fi not configured. Will start AP mode.");
    config.configured = false;
  } else {
    logMessage(String("Loaded Wi-Fi config: ") + config.ssid);
  }
}

void resetConfig() {
  strncpy(config.ssid, "", sizeof(config.ssid));
  strncpy(config.password, "", sizeof(config.password));
  config.configured = false;
  saveConfig();
  logMessage("Configuration reset");
}

// ================= JSON ESCAPE =================
String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// ================= RESTART FUNCTIONS =================
void scheduleRestart(unsigned long delayMs) {
  pendingRestart = true;
  restartAtMillis = millis() + delayMs;
  logMessage(String("Restart scheduled in ") + delayMs + "ms");
}

void handlePendingRestart() {
  if (pendingRestart && (long)(millis() - restartAtMillis) >= 0) {
    logMessage("Performing scheduled restart...");
    ESP.restart();
  }
}

// ================= AP FUNCTIONS =================
void touchAP() {
  if (apActive) {
    apStartTime = millis();
  }
}

void checkAPTimeout() {
  if (apActive && (millis() - apStartTime > apTimeout)) {
    logMessage("AP timeout reached. Disabling AP mode.");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive = false;
    logMessage("AP mode disabled");
  }
}

// ================= DISPLAY FUNCTIONS =================
void updateDisplay(int h, int m, int s, bool visible) {
  digitalWrite(LATCH_PIN, LOW);

  if (!visible) {
    for (int i = 0; i < 6; i++) {
      shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, 0xFF); // All segments off
    }
    digitalWrite(LATCH_PIN, HIGH);
    return;
  }

  byte digits[6];
  digits[0] = customDigitMap[h / 10];
  digits[1] = customDigitMap[h % 10];
  digits[2] = customDigitMap[m / 10];
  digits[3] = customDigitMap[m % 10];
  digits[4] = customDigitMap[s / 10];
  digits[5] = customDigitMap[s % 10];

  // Apply brightness via PWM simulation
  // We'll handle this in the main loop with the visible parameter

  if (blinkDots && (s % 2 != 0)) {
    digits[0] |= 0b00000001; // Dot for hour tens
    digits[1] |= 0b00000001; // Dot for hour units
    digits[2] |= 0b00000001; // Dot for minute tens
    digits[3] |= 0b00000001; // Dot for minute units
    digits[4] |= 0b00000001; // Dot for second tens
    digits[5] |= 0b00000001; // Dot for second units
  }

  for (int i = 0; i < 6; i++) {
    shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, ~digits[i]);
  }

  digitalWrite(LATCH_PIN, HIGH);
}

// ================= TIME FUNCTIONS =================
bool syncTimeWithNTP() {
  static bool firstAttempt = true;
  
  if (firstAttempt) {
    firstAttempt = false;
    logMessage("First NTP sync attempt...");
  }

  // Try current server
  configTime(gmtOffsetSec, daylightOffset_sec, ntpServers[currentNtpServerIndex]);
  
  // Wait for time to be set
  time_t now = time(nullptr);
  unsigned long startTime = millis();
  int attempts = 0;
  
  unsigned long lastAttemptTime = startTime;
  while (now < 946684800 && attempts < 10) { // 946684800 = 2000-01-01
    if (millis() - lastAttemptTime >= 500) {
      now = time(nullptr);
      attempts++;
      lastAttemptTime = millis();
    }
    if (millis() - startTime > 5000) break; // Timeout after 5 seconds
    yield();
  }
  
  if (now >= 946684800) {
    logMessage(String("NTP sync successful with server: ") + ntpServers[currentNtpServerIndex]);
    ntpSynced = true;
    return true;
  } else {
    logMessage(String("NTP sync failed with server: ") + ntpServers[currentNtpServerIndex], true);
    
    // Try next server
    currentNtpServerIndex = (currentNtpServerIndex + 1) % NUM_NTP_SERVERS;
    
    if (attempts >= NUM_NTP_SERVERS) {
      logMessage("All NTP servers failed. Will retry later.", true);
      currentNtpServerIndex = 0; // Reset to first server
      return false;
    }
    
    return false;
  }
}

void checkTimeSync() {
  unsigned long now = millis();
  
  // Initial sync
  if (!ntpSynced && WiFi.status() == WL_CONNECTED) {
    if (now - lastNtpSyncAttempt > 10000) { // Try every 10 seconds
      lastNtpSyncAttempt = now;
      syncTimeWithNTP();
    }
  }
  
  // Periodic resync
  if (ntpSynced && now - lastNtpSyncAttempt > NTP_SYNC_INTERVAL) {
    lastNtpSyncAttempt = now;
    logMessage("Periodic NTP resync...");
    syncTimeWithNTP();
  }
}

// ================= WIFI FUNCTIONS =================
bool connectToWiFi() {
  if (!config.configured || strlen(config.ssid) == 0) {
    logMessage("Wi-Fi not configured. Starting AP mode.");
    return false;
  }

  logMessage(String("Connecting to Wi-Fi: ") + config.ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);

  unsigned long startTime = millis();
  int attempts = 0;
  
  unsigned long lastAttemptTime = startTime;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    if (millis() - startTime > 15000) break; // 15 second timeout
    if (millis() - lastAttemptTime >= 500) {
      attempts++;
      lastAttemptTime = millis();
    }
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    logMessage(String("Wi-Fi connected! IP: ") + WiFi.localIP().toString());
    return true;
  } else {
    logMessage(String("Failed to connect to Wi-Fi: ") + config.ssid, true);
    return false;
  }
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED && config.configured && strlen(config.ssid) > 0) {
    unsigned long now = millis();
    if (now - lastWiFiReconnectAttempt > WIFI_RECONNECT_INTERVAL) {
      lastWiFiReconnectAttempt = now;
      logMessage("Attempting to reconnect to Wi-Fi...");
      if (!connectToWiFi()) {
        // If still not connected, start AP mode
        if (!apActive) {
          setupAP();
        }
      }
    }
  }
}

void setupAP() {
  logMessage("Starting AP mode...");
  
  // Generate a random password if not set
  if (strlen(config.apPassword) == 0) {
    strncpy(config.apPassword, DEFAULT_AP_PASSWORD, sizeof(config.apPassword));
  }
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(apSSID, config.apPassword);

  logMessage(String("AP started. SSID: ") + apSSID + ", IP: " + WiFi.softAPIP().toString());
  apActive = true;
  apStartTime = millis();
}

// ================= OTA FUNCTIONS =================
void setupOTA() {
  logMessage("Setting up OTA...");
  
  // Hostname defaults to esp8266-[ChipID]
  String hostname = "LED-Clock-";
  hostname += String(ESP.getChipId(), HEX);
  
  ArduinoOTA.setHostname(hostname.c_str());
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setPassword("ota123"); // Default OTA password
  
  ArduinoOTA.onStart([]() {
    logMessage("OTA update started");
  });
  
  ArduinoOTA.onEnd([]() {
    logMessage("OTA update finished");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (progress % (total / 10) == 0) {
      logMessage(String("OTA progress: ") + (progress / (total / 100)) + "%");
    }
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    String errorMsg = "OTA Error: ";
    switch (error) {
      case OTA_AUTH_ERROR: errorMsg += "Auth Failed"; break;
      case OTA_BEGIN_ERROR: errorMsg += "Begin Failed"; break;
      case OTA_CONNECT_ERROR: errorMsg += "Connect Failed"; break;
      case OTA_RECEIVE_ERROR: errorMsg += "Receive Failed"; break;
      case OTA_END_ERROR: errorMsg += "End Failed"; break;
      default: errorMsg += String(error); break;
    }
    logMessage(errorMsg.c_str(), true);
  });
  
  ArduinoOTA.begin();
  logMessage(String("OTA ready. Hostname: ") + hostname + ", Port: 8266");
}

// ================= WEB HANDLERS =================
String getLangText(int index) {
  const char** strings = getLangStrings();
  if (index >= 0 && index < (sizeof(LANG_RU) / sizeof(LANG_RU[0]))) {
    return String(strings[index]);
  }
  return "";
}

String generateHtmlPage() {
  const char** lang = getLangStrings();
  
  String html = R"rawliteral(<!DOCTYPE html>
<html>)rawliteral";
  html += String("\n<html lang=\"") + (currentLanguage == RU ? "ru" : "en") + "\">";
  html += R"rawliteral(
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>)rawliteral";
  html += getLangText(0); // LED Clock
  html += R"rawliteral(</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
           background-color: #121212; color: #e0e0e0; text-align: center; padding: 20px; margin: 0; }
    h1 { color: #4CAF50; }
    h2 { color: #4CAF50; margin: 5px 0; }
    .card { background: #1e1e1e; border-radius: 12px; padding: 20px; margin: 15px auto; max-width: 400px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    .switch { position: relative; display: inline-block; width: 60px; height: 34px; margin: 10px; }
    .switch input { opacity: 0; width: 0; height: 0; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: .4s; border-radius: 34px; }
    .slider:before { position: absolute; content: ""; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }
    input:checked + .slider { background-color: #4CAF50; }
    input:checked + .slider:before { transform: translateX(26px); }
    button { background-color: #4CAF50; color: white; border: none; padding: 12px 24px; border-radius: 8px; font-size: 16px; cursor: pointer; margin: 5px; }
    button:hover { background-color: #45a049; }
    button.danger { background-color: #ff6b6b; }
    button.danger:hover { background-color: #ee5a52; }
    button.secondary { background-color: #607d8b; }
    button.secondary:hover { background-color: #546e7a; }
    input[type="text"], input[type="password"], select { width: 90%; padding: 10px; margin: 8px 0; border-radius: 6px; border: 1px solid #333; background: #2a2a2a; color: #fff; font-size: 14px; box-sizing: border-box; }
    .slider-container { margin: 20px 0; }
    .slider-label { display: flex; justify-content: space-between; margin-bottom: 5px; }
    input[type="range"] { width: 100%; height: 8px; border-radius: 4px; background: #333; outline: none; -webkit-appearance: none; }
    input[type="range"]::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 20px; height: 20px; border-radius: 50%; background: #4CAF50; cursor: pointer; }
    .status { font-size: 14px; color: #888; margin-top: 10px; }
    .info { font-size: 12px; color: #aaa; margin-top: 5px; }
    .error { color: #ff6b6b; }
    .success { color: #4CAF50; }
    hr { border: none; border-top: 1px solid #333; margin: 20px auto; max-width: 400px; }
    .tab { display: none; }
    .tab.active { display: block; }
    .tabs { margin: 20px 0; }
    .tab-button { padding: 10px 20px; margin: 0 5px; background: #2a2a2a; border: none; color: #aaa; cursor: pointer; border-radius: 6px; }
    .tab-button.active { background: #1e1e1e; color: #4CAF50; }
    .tab-button:hover { background: #333; }
    .log-container { background: #000; color: #0f0; font-family: monospace; padding: 10px; border-radius: 6px; text-align: left; max-height: 300px; overflow-y: auto; white-space: pre-wrap; font-size: 12px; }
    .log-line.error { color: #f88; }
    a { color: #4CAF50; text-decoration: none; }
    a:hover { text-decoration: underline; }
    .version { font-size: 12px; color: #888; margin-top: 20px; }
  </style>
</head>
<body>
  <h1>[1;32m)rawliteral";
  html += getLangText(0); // LED Clock
  html += R"rawliteral(</h1>
  <p class="version">)rawliteral";
  html += getLangText(21); // Current Version
  html += String(": ") + FIRMWARE_VERSION + "</p>";
  
  // Tabs
  html += R"rawliteral(
  <div class="tabs">
    <button class="tab-button active" onclick="showTab('main')">)rawliteral";
  html += getLangText(1); // Clock Setup
  html += R"rawliteral(</button>
    <button class="tab-button" onclick="showTab('wifi')">)rawliteral";
  html += getLangText(6); // Wi-Fi Setup
  html += R"rawliteral(</button>
    <button class="tab-button" onclick="showTab('ota')">OTA</button>
    <button class="tab-button" onclick="showTab('telnet')">Telnet</button>
    <button class="tab-button" onclick="showTab('logs')">)rawliteral";
  html += getLangText(25); // Logs
  html += R"rawliteral(</button>
    <button class="tab-button" onclick="showTab('settings')">Settings</button>
  </div>

  <!-- Main Tab -->
  <div id="main" class="tab active">
    <div class="card">
      <h3>[1;33m)rawliteral";
  html += getLangText(2); // Night Mode
  html += R"rawliteral( ([0m)rawliteral";
  html += getLangText(16); // On
  html += String(" x0.5)") + R"rawliteral(</h3>
      <label class="switch">
        <input type="checkbox" id="nightMode" onchange="toggleSetting('night', this.checked)">
        <span class="slider"></span>
      </label>
    </div>

    <div class="card">
      <h3>)rawliteral";
  html += getLangText(3); // Brightness
  html += R"rawliteral(</h3>
      <div class="slider-container">
        <div class="slider-label">
          <span>0%</span>
          <span id="brightnessValue">100%</span>
        </div>
        <input type="range" id="brightness" min="0" max="100" value=")rawliteral";
  html += String(brightness);
  html += R"rawliteral(" oninput="updateBrightnessValue(this.value)" onchange="setBrightness(this.value)">
      </div>
    </div>

    <div class="card">
      <h3>)rawliteral";
  html += getLangText(4); // Blink Dots
  html += R"rawliteral(</h3>
      <label class="switch">
        <input type="checkbox" id="blinkDots" checked onchange="toggleSetting('blink', this.checked)">
        <span class="slider"></span>
      </label>
    </div>

    <div class="card">
      <h3>[1;36m)rawliteral";
  html += getLangText(5); // Time Zone
  html += R"rawliteral(</h3>
      <select id="tzSelect" onchange="setTimezone(this.value)"></select>
    </div>

    <div class="card">
      <button onclick="syncTime()">[1;34m)rawliteral";
  html += getLangText(11); // Sync Time
  html += R"rawliteral(</button>
      <p id="syncStatus" class="status"></p>
    </div>
  </div>

  <!-- Wi-Fi Tab -->
  <div id="wifi" class="tab">
    <div class="card">
      <h3>[1;35m)rawliteral";
  html += getLangText(6); // Wi-Fi Setup
  html += R"rawliteral(</h3>
      <p class="info">)rawliteral";
  html += getLangText(7); // Current Network
  html += String(": <span id="currentSSID">-</span></p>
      <input type="text" id="newSSID" placeholder=")rawliteral";
  html += getLangText(8); // Wi-Fi Name (SSID)
  html += R"rawliteral(" maxlength="32">
      <input type="password" id="newPassword" placeholder=")rawliteral";
  html += getLangText(9); // Wi-Fi Password
  html += R"rawliteral(" maxlength="64">
      <button onclick="saveWiFi()">)rawliteral";
  html += getLangText(10); // Save and Restart
  html += R"rawliteral(</button>
      <p id="wifiStatus" class="status"></p>
    </div>

    <div class="card" style="background: #3d1f1f;">
      <h3 style="color: #ff6b6b;">[1;31m[0m[1;31m[0m[1;31m[0m [1;31m)rawliteral";
  html += getLangText(12); // Reset Wi-Fi Settings
  html += R"rawliteral(</h3>
      <p class="info">)rawliteral";
  html += getLangText(13); // IP Address
  html += String(": <span id="ipAddr">-</span></p>
      <p class="info">[1;33mAP: [0m)rawliteral";
  html += String(apSSID) + " (" + getLangText(13) + ": 192.168.4.1)</p>
      <button class="danger" onclick="resetWiFi()">[1;31m[0m)rawliteral";
  html += getLangText(12); // Reset Wi-Fi Settings
  html += R"rawliteral(</button>
      <p id="resetStatus" class="status"></p>
    </div>
  </div>

  <!-- OTA Tab -->
  <div id="ota" class="tab">
    <div class="card">
      <h3>OTA Firmware Update</h3>
      <p class="info">Hostname: <code id="otaHostname">-</code></p>
      <p class="info">Port: 8266</p>
      <p class="info">Password: ota123</p>
      <p class="status">To update: Upload .bin file via Arduino IDE or PlatformIO to LED-Clock-[CHIPID].local:8266</p>
      <p class="status">Current version: )rawliteral";
  html += String(FIRMWARE_VERSION);
  html += R"rawliteral(</p>
    </div>
  </div>

  <!-- Telnet Tab -->
  <div id="telnet" class="tab">
    <div class="card">
      <h3>)rawliteral";
  html += getLangText(38); // Telnet Server
  html += R"rawliteral(</h3>
      <p class="info">Status: <span id="telnetStatus">)rawliteral";
  html += (telnetEnabled ? getLangText(41) : getLangText(42));
  html += R"rawliteral(</span></p>
      <p class="info">Port: 23</p>
      <p class="status">Connect using: telnet )rawliteral";
  html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "192.168.4.1");
  html += R"rawliteral( or LED-Clock-[CHIPID].local</p>
      <p class="status">Available commands: time, date, wifi, ip, ntp, brightness, night, blink, tz, logs, clear, restart, version, help</p>
      <label class="switch">
        <input type="checkbox" id="telnetToggle" )rawliteral";
  html += (telnetEnabled ? "checked" : "");
  html += R"rawliteral( onchange="toggleTelnet(this.checked)">
        <span class="slider"></span>
      </label>
      <p style="margin-top: 10px;">)rawliteral";
  html += getLangText(43); // Enable Telnet
  html += R"rawliteral(/Disable Telnet)</p>
    </div>
  </div>

  <!-- Logs Tab -->
  <div id="logs" class="tab">
    <div class="card">
      <h3>)rawliteral";
  html += getLangText(25); // Logs
  html += R"rawliteral(</h3>
      <div class="log-container" id="logContainer"></div>
      <button onclick="refreshLogs()">)rawliteral";
  html += getLangText(26); // Refresh
  html += R"rawliteral(</button>
      <button onclick="clearLogs()">)rawliteral";
  html += getLangText(27); // Clear Logs
  html += R"rawliteral(</button>
      <button onclick="downloadLogs()">)rawliteral";
  html += getLangText(28); // Download Logs
  html += R"rawliteral(</button>
    </div>
  </div>

  <!-- Settings Tab -->
  <div id="settings" class="tab">
    <div class="card">
      <h3>Additional Settings</h3>
      <div class="card">
        <h4>)rawliteral";
  html += getLangText(33); // Language
  html += R"rawliteral(</h4>
        <select id="languageSelect" onchange="setLanguage(this.value)">
          <option value="ru" )rawliteral";
  html += (currentLanguage == RU ? "selected" : "");
  html += R"rawliteral(>)rawliteral";
  html += getLangText(34); // Russian
  html += R"rawliteral(</option>
          <option value="en" )rawliteral";
  html += (currentLanguage == EN ? "selected" : "");
  html += R"rawliteral(>)rawliteral";
  html += getLangText(35); // English
  html += R"rawliteral(</option>
        </select>
      </div>
      <button onclick="saveSettings()">)rawliteral";
  html += getLangText(31); // Save
  html += R"rawliteral(</button>
      <p id="settingsStatus" class="status"></p>
    </div>
    
    <div class="card">
      <h3>AP Password</h3>
      <input type="password" id="apPassword" placeholder="New AP Password" maxlength="64">
      <button onclick="saveAPPassword()">Set AP Password</button>
      <p id="apPasswordStatus" class="status"></p>
    </div>
  </div>

  <script>
    // Language strings for JavaScript
    const lang = {
      syncing: ")rawliteral";
  html += getLangText(15); // Syncing time...
  html += R"rawliteral(",
      synced: ")rawliteral";
  html += getLangText(16); // Time synchronized!
  html += R"rawliteral(",
      errorNoSSID: ")rawliteral";
  html += getLangText(17); // Error: Enter SSID!
  html += R"rawliteral(",
      errorSSIDLong: ")rawliteral";
  html += getLangText(18); // Error: SSID too long!
  html += R"rawliteral(",
      errorPasswordLong: ")rawliteral";
  html += getLangText(19); // Error: Password too long!
  html += R"rawliteral(",
      errorNoCredentials: ")rawliteral";
  html += getLangText(20); // Error: SSID or password not specified
  html += R"rawliteral(",
      savedRestart: ")rawliteral";
  html += getLangText(21); // Saved! Restarting in 3 sec...
  html += R"rawliteral(",
      resetConfirm: ")rawliteral";
  html += getLangText(22); // Are you sure?...
  html += R"rawliteral(",
      resetting: ")rawliteral";
  html += getLangText(23); // Resetting...
  html += R"rawliteral(",
      error: ")rawliteral";
  html += getLangText(24); // Error
  html += R"rawliteral(",
      ok: ")rawliteral";
  html += getLangText(29) // OK
  html += R"rawliteral("
    };

    // Tab switching
    function showTab(tabName) {
      document.querySelectorAll('.tab').forEach(tab => tab.classList.remove('active'));
      document.querySelectorAll('.tab-button').forEach(btn => btn.classList.remove('active'));
      document.getElementById(tabName).classList.add('active');
      event.target.classList.add('active');
      
      if (tabName === 'logs') {
        refreshLogs();
      }
    }

    // Fill timezone select
    const tzSelect = document.getElementById('tzSelect');
    for (let h = -12; h <= 14; h++) {
      const opt = document.createElement('option');
      opt.value = h * 3600;
      opt.text = 'UTC' + (h >= 0 ? '+' : '') + h;
      tzSelect.appendChild(opt);
    }

    // Load initial data
    fetch('/status')
      .then(response => response.json())
      .then(data => {
        document.getElementById('nightMode').checked = data.night;
        document.getElementById('blinkDots').checked = data.blink;
        document.getElementById('ipAddr').innerText = data.ip;
        document.getElementById('currentSSID').innerText = data.ssid || 'Not connected';
        document.getElementById('tzSelect').value = data.tz;
        document.getElementById('brightness').value = data.brightness;
        updateBrightnessValue(data.brightness);
        document.getElementById('otaHostname').innerText = data.hostname;
        document.getElementById('telnetStatus').innerText = data.telnetEnabled ? lang.ok : lang.error;
        document.getElementById('telnetToggle').checked = data.telnetEnabled;
      });

    function toggleSetting(type, value) {
      fetch('/set?' + type + '=' + (value ? 1 : 0))
        .then(response => response.text())
        .then(text => console.log(text));
    }

    function setBrightness(value) {
      fetch('/set?brightness=' + value)
        .then(response => response.text())
        .then(text => console.log(text));
    }

    function updateBrightnessValue(value) {
      document.getElementById('brightnessValue').innerText = value + '%';
    }

    function setTimezone(value) {
      fetch('/set?tz=' + value)
        .then(response => response.text())
        .then(text => console.log(text));
    }

    function saveWiFi() {
      const ssid = document.getElementById('newSSID').value;
      const password = document.getElementById('newPassword').value;

      if (!ssid) {
        document.getElementById('wifiStatus').innerText = lang.errorNoSSID;
        document.getElementById('wifiStatus').className = 'status error';
        return;
      }
      if (ssid.length > 32) {
        document.getElementById('wifiStatus').innerText = lang.errorSSIDLong;
        document.getElementById('wifiStatus').className = 'status error';
        return;
      }
      if (password.length > 64) {
        document.getElementById('wifiStatus').innerText = lang.errorPasswordLong;
        document.getElementById('wifiStatus').className = 'status error';
        return;
      }

      document.getElementById('wifiStatus').innerText = lang.syncing;
      document.getElementById('wifiStatus').className = 'status';

      fetch('/wifi?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password))
        .then(response => response.text())
        .then(text => {
          document.getElementById('wifiStatus').innerText = text;
          document.getElementById('wifiStatus').className = 'status success';
        });
    }

    function syncTime() {
      document.getElementById('syncStatus').innerText = lang.syncing;
      document.getElementById('syncStatus').className = 'status';
      fetch('/sync')
        .then(response => response.text())
        .then(text => {
          document.getElementById('syncStatus').innerText = text;
          document.getElementById('syncStatus').className = 'status success';
          setTimeout(() => { 
            document.getElementById('syncStatus').innerText = ''; 
            document.getElementById('syncStatus').className = 'status'; 
          }, 3000);
        });
    }

    function resetWiFi() {
      if (confirm(lang.resetConfirm)) {
        document.getElementById('resetStatus').innerText = lang.resetting;
        document.getElementById('resetStatus').className = 'status';
        fetch('/reset-wifi')
          .then(response => response.text())
          .then(text => {
            document.getElementById('resetStatus').innerText = text;
            document.getElementById('resetStatus').className = 'status success';
          });
      }
    }

    function toggleTelnet(enabled) {
      fetch('/telnet?enable=' + (enabled ? '1' : '0'))
        .then(response => response.text())
        .then(text => {
          document.getElementById('telnetStatus').innerText = enabled ? lang.ok : lang.error;
          console.log(text);
        });
    }

    function refreshLogs() {
      fetch('/logs')
        .then(response => response.text())
        .then(text => {
          document.getElementById('logContainer').innerHTML = text.replace(/\n/g, '<br>');
        });
    }

    function clearLogs() {
      fetch('/clear-logs')
        .then(response => response.text())
        .then(text => {
          document.getElementById('logContainer').innerHTML = '';
        });
    }

    function downloadLogs() {
      fetch('/logs')
        .then(response => response.text())
        .then(text => {
          const blob = new Blob([text], { type: 'text/plain' });
          const url = URL.createObjectURL(blob);
          const a = document.createElement('a');
          a.href = url;
          a.download = 'clock_logs.txt';
          document.body.appendChild(a);
          a.click();
          document.body.removeChild(a);
          URL.revokeObjectURL(url);
        });
    }

    function setLanguage(langCode) {
      fetch('/set?language=' + langCode)
        .then(response => response.text())
        .then(text => {
          document.getElementById('settingsStatus').innerText = text;
          document.getElementById('settingsStatus').className = 'status success';
          setTimeout(() => {
            document.getElementById('settingsStatus').innerText = '';
            document.getElementById('settingsStatus').className = 'status';
          }, 2000);
        });
    }

    function saveSettings() {
      document.getElementById('settingsStatus').innerText = lang.ok;
      document.getElementById('settingsStatus').className = 'status success';
      setTimeout(() => {
        document.getElementById('settingsStatus').innerText = '';
        document.getElementById('settingsStatus').className = 'status';
      }, 2000);
    }

    function saveAPPassword() {
      const password = document.getElementById('apPassword').value;
      if (!password) {
        document.getElementById('apPasswordStatus').innerText = lang.errorNoSSID;
        document.getElementById('apPasswordStatus').className = 'status error';
        return;
      }
      
      fetch('/set-ap-password?password=' + encodeURIComponent(password))
        .then(response => response.text())
        .then(text => {
          document.getElementById('apPasswordStatus').innerText = text;
          document.getElementById('apPasswordStatus').className = 'status success';
          document.getElementById('apPassword').value = '';
        });
    }

    // Refresh logs every 5 seconds
    setInterval(() => {
      if (document.getElementById('logs').classList.contains('active')) {
        refreshLogs();
      }
    }, 5000);
  </script>
</body>
</html>
)rawliteral";
  
  return html;
}

String htmlPage;

void handleRoot() {
  touchAP();
  server.send(200, "text/html", htmlPage);
}

void handleSet() {
  touchAP();
  bool changed = false;

  if (server.hasArg("night")) {
    nightMode = (server.arg("night") == "1");
    changed = true;
  }
  if (server.hasArg("blink")) {
    blinkDots = (server.arg("blink") == "1");
    changed = true;
  }
  if (server.hasArg("tz")) {
    long tz = server.arg("tz").toInt();
    if (tz >= -12L * 3600 && tz <= 14L * 3600) {
      gmtOffsetSec = tz;
      configTime(gmtOffsetSec, daylightOffset_sec, ntpServers[currentNtpServerIndex]);
      changed = true;
    }
  }
  if (server.hasArg("brightness")) {
    int newBrightness = server.arg("brightness").toInt();
    if (newBrightness >= 0 && newBrightness <= 100) {
      brightness = newBrightness;
      changed = true;
    }
  }
  if (server.hasArg("language")) {
    String lang = server.arg("language");
    if (lang == "ru") {
      currentLanguage = RU;
    } else if (lang == "en") {
      currentLanguage = EN;
    }
    changed = true;
    htmlPage = generateHtmlPage(); // Regenerate HTML with new language
  }

  if (changed) {
    config.nightMode = nightMode;
    config.blinkDots = blinkDots;
    config.gmtOffsetSec = gmtOffsetSec;
    config.brightness = brightness;
    config.language = currentLanguage;
    saveConfig();
  }

  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  touchAP();
  String ssid = jsonEscape(String(config.ssid));
  String ip = "Not connected";
  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP().toString();
  } else if (apActive) {
    ip = WiFi.softAPIP().toString();
  }
  
  String hostname = "LED-Clock-" + String(ESP.getChipId(), HEX);
  
  String json = "{\"night\":" + String(nightMode ? "true" : "false") +
                ",\"blink\":" + String(blinkDots ? "true" : "false") +
                ",\"tz\":" + String(gmtOffsetSec) +
                ",\"ip\":\"" + ip + "\"" +
                ",\"ssid\":\"" + ssid + "\"" +
                ",\"brightness\":" + String(brightness) +
                ",\"hostname\":\"" + hostname + "\"" +
                ",\"ntpSynced\":" + String(ntpSynced ? "true" : "false") +
                ",\"version\":\"" + String(FIRMWARE_VERSION) + "\"" +
                ",\"telnetEnabled\":" + String(telnetEnabled ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleSync() {
  touchAP();
  ntpSynced = false;
  currentNtpServerIndex = 0;
  syncTimeWithNTP();
  server.send(200, "text/plain", getLangText(16)); // Time synchronized!
}

void handleWiFi() {
  touchAP();
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");

    if (newSSID.length() == 0) {
      server.send(400, "text/plain", getLangText(17)); // Error: Enter SSID!
      return;
    }
    if (newSSID.length() > sizeof(config.ssid) - 1) {
      server.send(400, "text/plain", getLangText(18)); // Error: SSID too long!
      return;
    }
    if (newPassword.length() > sizeof(config.password) - 1) {
      server.send(400, "text/plain", getLangText(19)); // Error: Password too long!
      return;
    }

    newSSID.toCharArray(config.ssid, sizeof(config.ssid));
    newPassword.toCharArray(config.password, sizeof(config.password));
    config.configured = true;

    saveConfig();

    server.send(200, "text/plain", getLangText(21)); // Saved! Restarting...
    scheduleRestart(3000);
  } else {
    server.send(400, "text/plain", getLangText(20)); // Error: SSID or password not specified
  }
}

void handleResetWiFi() {
  touchAP();
  resetConfig();
  server.send(200, "text/plain", getLangText(23)); // Resetting...
  scheduleRestart(2000);
}

void handleLogs() {
  touchAP();
  server.send(200, "text/plain", getLogs());
}

void handleClearLogs() {
  touchAP();
  clearLogs();
  server.send(200, "text/plain", "Logs cleared");
}

void handleSetAPPassword() {
  touchAP();
  if (server.hasArg("password")) {
    String password = server.arg("password");
    if (password.length() > 0 && password.length() <= sizeof(config.apPassword) - 1) {
      password.toCharArray(config.apPassword, sizeof(config.apPassword));
      saveConfig();
      server.send(200, "text/plain", getLangText(29)); // OK
      logMessage("AP password updated");
    } else {
      server.send(400, "text/plain", getLangText(24)); // Error
    }
  } else {
    server.send(400, "text/plain", getLangText(24)); // Error
  }
}

void handleTelnetToggle() {
  touchAP();
  if (server.hasArg("enable")) {
    String enableStr = server.arg("enable");
    if (enableStr == "1" || enableStr == "true") {
      enableTelnet();
      server.send(200, "text/plain", getLangText(41)); // Telnet enabled
    } else {
      disableTelnet();
      server.send(200, "text/plain", getLangText(42)); // Telnet disabled
    }
  } else {
    // Toggle
    telnetEnabled = !telnetEnabled;
    setupTelnet();
    server.send(200, "text/plain", telnetEnabled ? getLangText(41) : getLangText(42));
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  unsigned long startTime = millis();
  while (millis() - startTime < 100) { yield(); } // Small delay for Serial to initialize
  
  logMessage("=== LED Clock Firmware v" + String(FIRMWARE_VERSION) + " ===");
  logMessage("Chip ID: " + String(ESP.getChipId(), HEX));
  logMessage("Flash Chip ID: " + String(ESP.getFlashChipId(), HEX));
  logMessage("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);

  loadConfig();
  logMessage("Configuration loaded");

  // Initialize display
  updateDisplay(0, 0, 0, true);

  // Setup Wi-Fi
  logMessage("Initializing Wi-Fi...");
  if (!connectToWiFi()) {
    setupAP();
  }

  // Setup OTA
  setupOTA();

  // Setup mDNS
  String hostname = "LED-Clock-" + String(ESP.getChipId(), HEX);
  if (MDNS.begin(hostname.c_str())) {
    logMessage("mDNS started. Hostname: " + hostname + ".local");
  } else {
    logMessage("Failed to start mDNS", true);
  }

  // Setup web server
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/status", handleStatus);
  server.on("/sync", handleSync);
  server.on("/wifi", handleWiFi);
  server.on("/reset-wifi", handleResetWiFi);
  server.on("/logs", handleLogs);
  server.on("/clear-logs", handleClearLogs);
  server.on("/set-ap-password", handleSetAPPassword);
  server.on("/telnet", handleTelnetToggle);
  server.begin();
  logMessage("Web server started on port 80");

  // Setup Telnet (enabled by default)
  telnetEnabled = true;
  setupTelnet();

  // Generate HTML page
  htmlPage = generateHtmlPage();
  logMessage("HTML page generated");

  // Initial time sync
  if (WiFi.status() == WL_CONNECTED) {
    logMessage("Attempting initial NTP sync...");
    syncTimeWithNTP();
  }

  logMessage("Setup complete!");
}

// ================= LOOP =================
void loop() {
  // Handle OTA updates
  ArduinoOTA.handle();
  
  // Handle web server
  server.handleClient();
  
  // Handle mDNS
  MDNS.update();
  
  // Handle Telnet clients
  if (telnetEnabled) {
    handleTelnetClients();
  }
  
  // Check AP timeout
  checkAPTimeout();
  
  // Handle pending restart
  handlePendingRestart();
  
  // Check Wi-Fi connection
  checkWiFiConnection();
  
  // Check NTP sync
  checkTimeSync();

  unsigned long now = millis();

  // Update time every second
  if (now - lastTimeUpdate >= 1000) {
    lastTimeUpdate = now;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      curHour = timeinfo.tm_hour;
      curMinute = timeinfo.tm_min;
      curSecond = timeinfo.tm_sec;
      timeOK = true;
      
      if (!ntpSynced) {
        ntpSynced = true;
        logMessage("NTP time synchronized");
      }
    } else {
      timeOK = false;
      if (ntpSynced) {
        ntpSynced = false;
        logMessage("NTP sync lost", true);
      }
    }
  }

  // PWM dimming
  if (now - lastPWMStep >= PWM_STEP_MS) {
    lastPWMStep = now;
    pwmStep = (pwmStep + 1) % PWM_STEPS;

    bool visible = true;
    if (nightMode) {
      // Apply brightness in night mode
      // Calculate visibility based on brightness and PWM step
      int brightnessThreshold = (int)(brightness * PWM_STEPS / 100.0);
      visible = (pwmStep < brightnessThreshold);
    } else {
      // Apply brightness in normal mode
      int brightnessThreshold = (int)(brightness * PWM_STEPS / 100.0);
      visible = (pwmStep < brightnessThreshold);
    }

    if (timeOK) {
      updateDisplay(curHour, curMinute, curSecond, visible);
    } else {
      // Show dash pattern when no time
      updateDisplay(0, 0, 0, visible);
    }
  }

  yield();
}
