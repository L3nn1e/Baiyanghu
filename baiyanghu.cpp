#include <ESP8266WiFi.h>
#include <time.h>

// ================= НАСТРОЙКИ WI-FI =================
const char* ssid     = "--------";     // <- ВПИШИТЕ ИМЯ ВАШЕГО WI-FI
const char* password = "--------"; // <- ВПИШИТЕ ПАРОЛЬ WI-FI

// ================= ПИНЫ СДВИГОВЫХ РЕГИСТРОВ 74HC595 =================
// Проверенные вами рабочие пины!
const int DATA_PIN  = 13; // GPIO13 (Data / DS)
const int CLOCK_PIN = 14; // GPIO14 (Clock / SH_CP)
const int LATCH_PIN = 12; // GPIO12 (Latch / ST_CP)

// ================= НАСТРОЙКИ ВРЕМЕНИ =================
const long gmtOffset_sec = 3 * 3600; // UTC+3 (Москва/МСК). Настройте под свой пояс при необходимости
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

const byte customDigitMap[10] = {
  0b01111110, // 0
  0b00001100, // 1
  0b10110110, // 2
  0b10011110, // 3
  0b11001100, // 4
  0b11011010, // 5
  0b11111010, // 6
  0b00001110, // 7
  0b11111110, // 8
  0b11011110  // 9 (A, B, C, D, F, G) - ПОЧИНЕНО!
};

void updateDisplay(int h, int m, int s) {
  byte digits[6];

  digits[0] = customDigitMap[h / 10]; 
  digits[1] = customDigitMap[h % 10]; 
  digits[2] = customDigitMap[m / 10]; 
  digits[3] = customDigitMap[m % 10]; 
  digits[4] = customDigitMap[s / 10]; 
  digits[5] = customDigitMap[s % 10]; 

  // Если секунда нечетная — включаем бит 0 (точку) НА ВСЕХ РАЗРЯДАХ
  if (s % 2 != 0) {
    digits[0] |= 0b00000001; // 1-й разряд
    digits[1] |= 0b00000001; // 2-й разряд
    digits[2] |= 0b00000001; // 3-й разряд
    digits[3] |= 0b00000001; // 4-й разряд
    digits[4] |= 0b00000001; // 5-й разряд
    digits[5] |= 0b00000001; // 6-й разряд
  }

  digitalWrite(LATCH_PIN, LOW);
  
  for (int i = 0; i < 6; i++) {
    shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, ~digits[i]);
  }
  
  digitalWrite(LATCH_PIN, HIGH);
}

void setup() {
  Serial.begin(115200);

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);

  // Подключение к Wi-Fi
  Serial.println("\nПодключение к Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi успешно подключен!");
  Serial.print("IP адрес: ");
  Serial.println(WiFi.localIP());

  // Настройка и синхронизация NTP времени
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Ожидание синхронизации времени по NTP...");
  
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    now = time(nullptr);
  }
  Serial.println("Время синхронизировано!");
}

void loop() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int hours   = timeinfo.tm_hour;
    int minutes = timeinfo.tm_min;
    int seconds = timeinfo.tm_sec;

    updateDisplay(hours, minutes, seconds);
  }
  delay(1000); // Обновление каждую секунду
}
