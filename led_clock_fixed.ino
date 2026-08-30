#include <ESP8266WiFi.h>
#include <time.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// ================= ТОЧКА ДОСТУПА ДЛЯ НАСТРОЙКИ =================
const char* apSSID = "Clock-Setup";
const char* apPassword = "12345678"; // Пароль от точки доступа (минимум 8 символов)
const unsigned long apTimeout = 180000;   // AP отключится через 3 минуты БЕЗДЕЙСТВИЯ

// Статический IP ТОЛЬКО для точки доступа (чтобы всегда знать адрес настройки)
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ================= ПИНЫ СДВИГОВЫХ РЕГИСТРОВ 74HC595 =================
const int DATA_PIN  = 13;
const int CLOCK_PIN = 14;
const int LATCH_PIN = 12;

// ================= НАСТРОЙКИ ВРЕМЕНИ =================
long gmtOffsetSec = 3 * 3600;           // теперь не константа — задаётся через веб-интерфейс
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

// ================= ХРАНИЛИЩЕ НАСТРОЕК В EEPROM =================
const uint32_t CONFIG_MAGIC = 0xC10CFEED;

struct WiFiConfig {
  uint32_t magic;
  char ssid[33];
  char password[65];
  bool configured;
  bool nightMode;
  bool blinkDots;
  long gmtOffsetSec;
};

WiFiConfig wifiConfig;
const int EEPROM_SIZE = 512;

// ================= ГЛОБАЛЬНЫЕ НАСТРОЙКИ =================
bool blinkDots = true;
bool nightMode = false;

const byte customDigitMap[10] = {
  0b01111110, 0b00001100, 0b10110110, 0b10011110, 0b11001100,
  0b11011010, 0b11111010, 0b00001110, 0b11111110, 0b11011110
};

ESP8266WebServer server(80);

unsigned long apStartTime = 0;
bool apActive = false;

// Отложенная (неблокирующая) перезагрузка
bool pendingRestart = false;
unsigned long restartAtMillis = 0;

// ================= ФУНКЦИИ EEPROM =================
void saveWiFiConfig() {
  wifiConfig.magic = CONFIG_MAGIC;
  EEPROM.put(0, wifiConfig);
  EEPROM.commit();
  Serial.println("Настройки сохранены в EEPROM");
}

void loadWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiConfig);

  if (wifiConfig.magic != CONFIG_MAGIC) {
    // Первая загрузка или повреждённые данные — используем значения по умолчанию
    Serial.println("Конфигурация не найдена. Использую значения по умолчанию.");
    wifiConfig.ssid[0] = '\0';
    wifiConfig.password[0] = '\0';
    wifiConfig.configured = false;
    wifiConfig.nightMode = false;
    wifiConfig.blinkDots = true;
    wifiConfig.gmtOffsetSec = 3 * 3600;
    saveWiFiConfig();
  }

  nightMode   = wifiConfig.nightMode;
  blinkDots   = wifiConfig.blinkDots;
  gmtOffsetSec = wifiConfig.gmtOffsetSec;

  if (!wifiConfig.configured || wifiConfig.ssid[0] == '\0') {
    Serial.println("Настройки Wi-Fi не найдены. Будет запущена точка доступа.");
    wifiConfig.configured = false;
  } else {
    Serial.print("Загружены настройки Wi-Fi: ");
    Serial.println(wifiConfig.ssid);
  }
}

void resetWiFiConfig() {
  wifiConfig.configured = false;
  wifiConfig.ssid[0] = '\0';
  wifiConfig.password[0] = '\0';
  saveWiFiConfig();
  Serial.println("Настройки Wi-Fi сброшены");
}

// ================= JSON-ЭКРАНИРОВАНИЕ =================
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

// ================= НЕБЛОКИРУЮЩАЯ ПЕРЕЗАГРУЗКА =================
void scheduleRestart(unsigned long delayMs) {
  pendingRestart = true;
  restartAtMillis = millis() + delayMs;
}

void handlePendingRestart() {
  if (pendingRestart && (long)(millis() - restartAtMillis) >= 0) {
    ESP.restart();
  }
}

// ================= АКТИВНОСТЬ СБРАСЫВАЕТ ТАЙМАУТ AP =================
void touchAP() {
  if (apActive) {
    apStartTime = millis();
  }
}

// ================= ФУНКЦИЯ ОБНОВЛЕНИЯ ДИСПЛЕЯ =================
// visible=false используется для программного ШИМ (диммирования) в ночном режиме:
// на части кадров дисплей полностью гасится, создавая иллюзию сниженной яркости.
void updateDisplay(int h, int m, int s, bool visible) {
  digitalWrite(LATCH_PIN, LOW);

  if (!visible) {
    for (int i = 0; i < 6; i++) {
      shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, 0xFF); // все сегменты выключены
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

  if (blinkDots && (s % 2 != 0)) {
    digits[0] |= 0b00000001;
    digits[1] |= 0b00000001;
    digits[2] |= 0b00000001;
    digits[3] |= 0b00000001;
    digits[4] |= 0b00000001;
    digits[5] |= 0b00000001;
  }

  for (int i = 0; i < 6; i++) {
    shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, ~digits[i]);
  }

  digitalWrite(LATCH_PIN, HIGH);
}

// ================= ВЕБ-ИНТЕРФЕЙС =================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Управление часами</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
           background-color: #121212; color: #e0e0e0; text-align: center; padding: 20px; margin: 0; }
    h1 { color: #4CAF50; }
    .card { background: #1e1e1e; border-radius: 12px; padding: 20px; margin: 15px auto; max-width: 400px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    .switch { position: relative; display: inline-block; width: 60px; height: 34px; margin: 10px; }
    .switch input { opacity: 0; width: 0; height: 0; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: .4s; border-radius: 34px; }
    .slider:before { position: absolute; content: ""; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }
    input:checked + .slider { background-color: #4CAF50; }
    input:checked + .slider:before { transform: translateX(26px); }
    button { background-color: #4CAF50; color: white; border: none; padding: 12px 24px; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 10px; }
    button:hover { background-color: #45a049; }
    button.danger { background-color: #ff6b6b; }
    button.danger:hover { background-color: #ee5a52; }
    input[type="text"], input[type="password"], select { width: 90%; padding: 10px; margin: 8px 0; border-radius: 6px; border: 1px solid #333; background: #2a2a2a; color: #fff; font-size: 14px; box-sizing: border-box; }
    .status { font-size: 14px; color: #888; margin-top: 10px; }
    .info { font-size: 12px; color: #aaa; margin-top: 5px; }
    hr { border: none; border-top: 1px solid #333; margin: 20px auto; max-width: 400px; }
  </style>
</head>
<body>
  <h1>⏰ LED Часы</h1>

  <div class="card">
    <h3>🌙 Ночной режим (яркость ×0.5)</h3>
    <label class="switch">
      <input type="checkbox" id="nightMode" onchange="toggleSetting('night', this.checked)">
      <span class="slider"></span>
    </label>
  </div>

  <div class="card">
    <h3>✨ Мигающие точки</h3>
    <label class="switch">
      <input type="checkbox" id="blinkDots" checked onchange="toggleSetting('blink', this.checked)">
      <span class="slider"></span>
    </label>
  </div>

  <div class="card">
    <h3>🌍 Часовой пояс</h3>
    <select id="tzSelect" onchange="setTimezone(this.value)"></select>
  </div>

  <hr>

  <div class="card">
    <h3>📶 Настройка Wi-Fi</h3>
    <p class="info">Текущая сеть: <span id="currentSSID">-</span></p>
    <input type="text" id="newSSID" placeholder="Имя Wi-Fi сети (SSID)" maxlength="32">
    <input type="password" id="newPassword" placeholder="Пароль Wi-Fi" maxlength="64">
    <button onclick="saveWiFi()">💾 Сохранить и перезагрузить</button>
    <p id="wifiStatus" class="status"></p>
  </div>

  <div class="card">
    <button onclick="syncTime()">🔄 Синхронизировать время</button>
    <p id="syncStatus" class="status"></p>
  </div>

  <hr>

  <div class="card" style="background: #3d1f1f;">
    <h3 style="color: #ff6b6b;">⚠️ Сброс Wi-Fi настроек</h3>
    <p class="info">Часы перезагрузятся и создадут точку доступа <b>Clock-Setup</b> (пароль: 12345678, адрес: 192.168.4.1)</p>
    <button class="danger" onclick="resetWiFi()">🔄 Сбросить Wi-Fi</button>
    <p id="resetStatus" class="status"></p>
  </div>

  <p class="status">IP устройства: <span id="ipAddr"></span></p>

  <script>
    // Заполняем список часовых поясов UTC-12 .. UTC+14
    const tzSelect = document.getElementById('tzSelect');
    for (let h = -12; h <= 14; h++) {
      const opt = document.createElement('option');
      opt.value = h * 3600;
      opt.text = 'UTC' + (h >= 0 ? '+' : '') + h;
      tzSelect.appendChild(opt);
    }

    fetch('/status')
      .then(response => response.json())
      .then(data => {
        document.getElementById('nightMode').checked = data.night;
        document.getElementById('blinkDots').checked = data.blink;
        document.getElementById('ipAddr').innerText = data.ip;
        document.getElementById('currentSSID').innerText = data.ssid || 'Не настроено';
        tzSelect.value = data.tz;
      });

    function toggleSetting(type, value) {
      fetch('/set?' + type + '=' + (value ? 1 : 0))
        .then(response => response.text())
        .then(text => console.log(text));
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
        document.getElementById('wifiStatus').innerText = "❌ Введите имя сети!";
        return;
      }
      if (ssid.length > 32) {
        document.getElementById('wifiStatus').innerText = "❌ SSID длиннее 32 символов!";
        return;
      }
      if (password.length > 64) {
        document.getElementById('wifiStatus').innerText = "❌ Пароль длиннее 64 символов!";
        return;
      }

      document.getElementById('wifiStatus').innerText = "⏳ Сохранение... Часы перезагрузятся через несколько секунд";

      fetch('/wifi?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password))
        .then(response => response.text())
        .then(text => {
          document.getElementById('wifiStatus').innerText = text;
        });
    }

    function syncTime() {
      document.getElementById('syncStatus').innerText = "⏳ Синхронизация...";
      fetch('/sync')
        .then(response => response.text())
        .then(text => {
          document.getElementById('syncStatus').innerText = text;
          setTimeout(() => { document.getElementById('syncStatus').innerText = ""; }, 3000);
        });
    }

    function resetWiFi() {
      if (confirm("Вы уверены? Часы сбросят настройки Wi-Fi и создадут точку доступа Clock-Setup")) {
        document.getElementById('resetStatus').innerText = "⏳ Сброс... Перезагрузка";
        fetch('/reset-wifi')
          .then(response => response.text())
          .then(text => {
            document.getElementById('resetStatus').innerText = text;
          });
      }
    }
  </script>
</body>
</html>
)rawliteral";

// ================= ОБРАБОТЧИКИ ВЕБ-ЗАПРОСОВ =================
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
      configTime(gmtOffsetSec, daylightOffset_sec, ntpServer);
      changed = true;
    }
  }

  if (changed) {
    wifiConfig.nightMode = nightMode;
    wifiConfig.blinkDots = blinkDots;
    wifiConfig.gmtOffsetSec = gmtOffsetSec;
    saveWiFiConfig();
  }

  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  touchAP();
  String ssid = jsonEscape(String(wifiConfig.ssid));
  String ip = "не подключен";
  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP().toString();
  } else if (apActive) {
    ip = WiFi.softAPIP().toString();
  }
  String json = "{\"night\":" + String(nightMode ? "true" : "false") +
                ",\"blink\":" + String(blinkDots ? "true" : "false") +
                ",\"tz\":" + String(gmtOffsetSec) +
                ",\"ip\":\"" + ip + "\"" +
                ",\"ssid\":\"" + ssid + "\"}";
  server.send(200, "application/json", json);
}

void handleSync() {
  touchAP();
  configTime(gmtOffsetSec, daylightOffset_sec, ntpServer);
  server.send(200, "text/plain", "Время синхронизировано!");
}

void handleWiFi() {
  touchAP();
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");

    if (newSSID.length() == 0) {
      server.send(400, "text/plain", "Ошибка: имя сети не может быть пустым");
      return;
    }
    if (newSSID.length() > sizeof(wifiConfig.ssid) - 1) {
      server.send(400, "text/plain", "Ошибка: SSID слишком длинный (макс. " + String(sizeof(wifiConfig.ssid) - 1) + " симв.)");
      return;
    }
    if (newPassword.length() > sizeof(wifiConfig.password) - 1) {
      server.send(400, "text/plain", "Ошибка: пароль слишком длинный (макс. " + String(sizeof(wifiConfig.password) - 1) + " симв.)");
      return;
    }

    newSSID.toCharArray(wifiConfig.ssid, sizeof(wifiConfig.ssid));
    newPassword.toCharArray(wifiConfig.password, sizeof(wifiConfig.password));
    wifiConfig.configured = true;

    saveWiFiConfig();

    server.send(200, "text/plain", "✅ Сохранено! Перезагрузка через 3 сек...");
    scheduleRestart(3000);
  } else {
    server.send(400, "text/plain", "Ошибка: не указаны SSID или пароль");
  }
}

void handleResetWiFi() {
  touchAP();
  resetWiFiConfig();
  server.send(200, "text/plain", "✅ Настройки Wi-Fi сброшены! Перезагрузка...");
  scheduleRestart(2000);
}

// ================= ПОДКЛЮЧЕНИЕ К WI-FI (DHCP) =================
bool connectToWiFi() {
  if (!wifiConfig.configured || strlen(wifiConfig.ssid) == 0) {
    Serial.println("Wi-Fi не настроен. Пропускаем подключение.");
    return false; // Сразу возвращаем false, чтобы запустить AP
  }

  Serial.print("Подключение к ");
  Serial.println(wifiConfig.ssid);

  // СТРОГО DHCP
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi подключен!");
    Serial.print("IP (DHCP): ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nНе удалось подключиться к Wi-Fi");
    return false;
  }
}

// ================= ЗАПУСК ТОЧКИ ДОСТУПА =================
void setupAP() {
  Serial.println("Запуск точки доступа Clock-Setup...");
  WiFi.mode(WIFI_AP_STA);

  // Статический IP ТОЛЬКО для точки доступа
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(apSSID, apPassword);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  apActive = true;
  apStartTime = millis();
}

// ================= ПРОВЕРКА ТАЙМАУТА ТОЧКИ ДОСТУПА =================
void checkAPTimeout() {
  if (apActive && (millis() - apStartTime > apTimeout)) {
    Serial.println("Таймаут точки доступа (нет активности) — отключаем AP");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive = false;
    Serial.println("Точка доступа отключена");
  }
}

// ================= ПЕРЕМЕННЫЕ ГЛАВНОГО ЦИКЛА =================
unsigned long lastTimeUpdate = 0;
unsigned long lastPWMStep = 0;
int pwmStep = 0;
const int PWM_STEPS = 20;             // шагов на полный цикл диммирования
const unsigned long PWM_STEP_MS = 1;  // 1 мс/шаг -> полный цикл 20 мс (50 Гц, глазом не улавливается)

int curHour = 0, curMinute = 0, curSecond = 0;
bool timeOK = false;

// ================= ОСНОВНЫЕ ФУНКЦИИ =================
void setup() {
  Serial.begin(115200);

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);

  loadWiFiConfig();

  // Пытаемся подключиться. Если не удалось (или не настроено) — запускаем AP
  if (!connectToWiFi()) {
    setupAP();
  }

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/status", handleStatus);
  server.on("/sync", handleSync);
  server.on("/wifi", handleWiFi);
  server.on("/reset-wifi", handleResetWiFi);
  server.begin();
  Serial.println("Веб-сервер запущен!");

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffsetSec, daylightOffset_sec, ntpServer);
    Serial.println("Ожидание синхронизации времени...");

    time_t now = time(nullptr);
    int attempts = 0;
    while (now < 946684800 && attempts < 20) {
      delay(500);
      now = time(nullptr);
      attempts++;
    }
    Serial.println("Время синхронизировано!");
  }
}

void loop() {
  server.handleClient();
  checkAPTimeout();
  handlePendingRestart();

  unsigned long now = millis();

  // Обновляем текущее время раз в секунду (не блокируя остальной цикл)
  if (now - lastTimeUpdate >= 1000) {
    lastTimeUpdate = now;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      curHour = timeinfo.tm_hour;
      curMinute = timeinfo.tm_min;
      curSecond = timeinfo.tm_sec;
      timeOK = true;
    } else {
      timeOK = false;
    }
  }

  // Программный ШИМ дисплея: в ночном режиме дисплей "мигает" с частотой 50 Гц,
  // будучи включённым только половину времени — глаз воспринимает это как
  // снижение яркости примерно вдвое, а не как настоящее выключение.
  if (now - lastPWMStep >= PWM_STEP_MS) {
    lastPWMStep = now;
    pwmStep = (pwmStep + 1) % PWM_STEPS;

    bool visible = true;
    if (nightMode) {
      visible = (pwmStep < PWM_STEPS / 2); // 50% скважность
    }

    if (timeOK) {
      updateDisplay(curHour, curMinute, curSecond, visible);
    } else {
      updateDisplay(0, 0, 0, visible);
    }
  }

  yield();
}
