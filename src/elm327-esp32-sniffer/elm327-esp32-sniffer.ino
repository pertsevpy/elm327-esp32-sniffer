/**
 * ELM327 UART Sniffer / USB-UART Bridge for ESP32-C6
 * 
 * Функции:
 * - Замена FT232: USB (CDC) <-> UART (ELM327)
 * - Сниффер трафика с отображением в браузере по WiFi
 * - Управление скоростью UART через веб-интерфейс
 * - Корректная обработка длинных ответов (буферизация до промпта '>')
 * - Опциональная поддержка датчика DS18B20 (температура салона)
 * 
 * Подключение:
 *   ELM327 TX  -> GPIO4 (RX1)
 *   ELM327 RX  -> GPIO5 (TX1)
 *   GND -> GND
 *   (для DS18B20: DATA -> GPIO18, VCC -> 3.3V, GND -> GND, подтяжка 4.7к)
 * 
 * USB подключение к маршрутному компьютеру (ПК, планшет)
 * 
 * WiFi точка доступа: ELM327-Sniffer / 12345678
 * Веб-интерфейс: http://192.168.4.1
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

// ================== НАСТРОЙКИ ==================
// WiFi AP
const char* ap_ssid = "ELM327-Sniffer";
const char* ap_password = "12345678";

// UART для ELM327 (скорость по умолчанию)
#define ELM_UART_BAUD 38400
#define ELM_RX_PIN 4
#define ELM_TX_PIN 5

// USB Serial (CDC) для связи с ПК - используется стандартный объект Serial

// Опция: включить поддержку DS18B20 (раскомментировать для использования)
//#define USE_DS18B20
#ifdef USE_DS18B20
#include <OneWire.h>
#include <DallasTemperature.h>
#define ONE_WIRE_BUS 18
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
float lastTemperature = 0;
unsigned long lastTempRead = 0;
const long tempInterval = 2000; // читаем каждые 2 секунды
#endif

// ================================================

WebServer server(80);
WebSocketsServer webSocket(81);

// Кольцевой буфер для лога (история для новых клиентов)
#define LOG_BUFFER_SIZE 4096
char logBuffer[LOG_BUFFER_SIZE];
volatile int logHead = 0;
volatile int logTail = 0;

// Буферизация входящих строк от ELM327
#define MAX_LINE 512
uint8_t lineBuffer[MAX_LINE];
size_t linePos = 0;
unsigned long lastCharTime = 0;
const unsigned long LINE_TIMEOUT = 50; // 50 мс паузы = конец ответа

// ========== Функции работы с логом ==========
void addToLog(const char* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    logBuffer[logHead] = data[i];
    logHead = (logHead + 1) % LOG_BUFFER_SIZE;
    if (logHead == logTail) {
      logTail = (logTail + 1) % LOG_BUFFER_SIZE; // переполнение
    }
  }
}

void broadcastData(const char* data, size_t len) {
  webSocket.broadcastTXT(data, len);
}

// Сброс накопленной строки с указанием направления
void flushLine(const char* direction) {
  if (linePos == 0) return;
  
  char output[MAX_LINE * 4 + 50];
  int pos = snprintf(output, sizeof(output), "[%s] ", direction);
  
  // HEX
  for (size_t i = 0; i < linePos; i++) {
    pos += snprintf(output + pos, sizeof(output) - pos, "%02X ", lineBuffer[i]);
  }
  
  // разделитель
  pos += snprintf(output + pos, sizeof(output) - pos, "| ");
  
  // ASCII (с escap-последовательностями для управляющих символов)
  for (size_t i = 0; i < linePos; i++) {
    uint8_t c = lineBuffer[i];
    if (c == '\r') {
      output[pos++] = '\\';
      output[pos++] = 'r';
    } else if (c == '\n') {
      output[pos++] = '\\';
      output[pos++] = 'n';
    } else if (c == '\t') {
      output[pos++] = '\\';
      output[pos++] = 't';
    } else if (c >= 32 && c <= 126) {
      output[pos++] = (char)c;
    } else {
      output[pos++] = '.';
    }
  }
  output[pos++] = '\n';
  output[pos] = '\0';
  
  broadcastData(output, pos);
  addToLog(output, pos);
  
  linePos = 0;
}

// ========== Обработка WebSocket ==========
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] WebSocket disconnected\n", num);
      break;
      
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[%u] WebSocket connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      
      // Отправляем историю логов новому клиенту
      if (logTail != logHead) {
        int size = (logHead - logTail + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
        char temp[LOG_BUFFER_SIZE];
        if (logTail + size <= LOG_BUFFER_SIZE) {
          memcpy(temp, &logBuffer[logTail], size);
        } else {
          int first = LOG_BUFFER_SIZE - logTail;
          memcpy(temp, &logBuffer[logTail], first);
          memcpy(temp + first, logBuffer, size - first);
        }
        webSocket.sendTXT(num, temp, size);
      }
      break;
    }
      
    case WStype_TEXT: {
      String cmd = String((char*)payload);
      if (cmd.startsWith("baud ")) {
        int newBaud = cmd.substring(5).toInt();
        if (newBaud == 9600 || newBaud == 19200 || newBaud == 38400 || newBaud == 115200 || newBaud == 230400) {
          Serial1.end();
          Serial1.begin(newBaud, SERIAL_8N1, ELM_RX_PIN, ELM_TX_PIN);
          webSocket.sendTXT(num, "OK: Baud rate changed to " + String(newBaud));
          Serial.printf("Baud rate changed to %d\n", newBaud);
        } else {
          webSocket.sendTXT(num, "ERROR: Unsupported baud rate");
        }
      }
      // Можно добавить другие команды
      break;
    }
  }
}

// ========== Веб-страница ==========
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ELM327 UART Sniffer</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; margin: 20px; background: #f0f0f0; }
        h1 { color: #333; }
        #log { background: black; color: #0f0; padding: 10px; height: 400px; overflow-y: scroll; font-family: monospace; white-space: pre-wrap; font-size: 12px; border-radius: 5px; }
        .controls { margin: 10px 0; display: flex; gap: 10px; flex-wrap: wrap; }
        .controls select, .controls button { padding: 8px; font-size: 14px; }
        .status { margin: 10px 0; padding: 8px; background: #ddd; border-radius: 5px; }
        .footer { margin-top: 20px; font-size: 12px; color: #777; }
    </style>
</head>
<body>
    <h1>ELM327 UART Sniffer</h1>
    <div class="status">
        <span id="connStatus">WebSocket: </span>
    </div>
    <div class="controls">
        <select id="baud">
            <option value="9600">9600</option>
            <option value="19200">19200</option>
            <option value="38400" selected>38400</option>
            <option value="115200">115200</option>
            <option value="230400">230400</option>
        </select>
        <button onclick="changeBaud()">Set Baud</button>
        <button onclick="clearLog()">Clear Log</button>
        <select id="format" onchange="changeFormat()">
            <option value="ascii+hex">ASCII + HEX</option>
            <option value="hex">HEX only</option>
            <option value="ascii">ASCII only</option>
        </select>
    </div>
    <div id="log"></div>
    <div class="footer">ESP32-C6 ELM327 Sniffer</div>

    <script>
        var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
        var logDiv = document.getElementById('log');
        var connSpan = document.getElementById('connStatus');
        var currentFormat = 'ascii+hex';

        ws.onopen = function() {
            connSpan.innerHTML = 'WebSocket: <span style="color:green">Connected</span>';
            addLogLine('[System] Connected to sniffer');
        };
        ws.onclose = function() {
            connSpan.innerHTML = 'WebSocket: <span style="color:red">Disconnected</span>';
            addLogLine('[System] Disconnected');
        };
        ws.onmessage = function(event) {
            logDiv.innerHTML += event.data;
            logDiv.scrollTop = logDiv.scrollHeight;
        };

        function addLogLine(text) {
            logDiv.innerHTML += text + '\n';
            logDiv.scrollTop = logDiv.scrollHeight;
        }

        function changeBaud() {
            var baud = document.getElementById('baud').value;
            ws.send('baud ' + baud);
        }

        function clearLog() {
            logDiv.innerHTML = '';
        }

        function changeFormat() {
            var format = document.getElementById('format').value;
            ws.send('format ' + format);
        }
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", index_html);
}

// ========== SETUP ==========
void setup() {
  // Инициализация USB CDC (для связи с ПК)
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(1000);
  Serial.println("\n=== ELM327 Sniffer Bridge starting ===");

  // Инициализация UART для ELM327
  Serial1.begin(ELM_UART_BAUD, SERIAL_8N1, ELM_RX_PIN, ELM_TX_PIN);
  Serial.printf("UART for ELM327 initialized at %d baud\n", ELM_UART_BAUD);

#ifdef USE_DS18B20
  sensors.begin();
  Serial.println("DS18B20 initialized");
#endif

  // Настройка WiFi точки доступа
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("WiFi AP started. IP: ");
  Serial.println(myIP);

  // Запуск веб-сервера
  server.on("/", handleRoot);
  server.begin();
  Serial.println("HTTP server started");

  // Запуск WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81");

  Serial.println("Ready! Connect to WiFi and open http://192.168.4.1");
}

// ========== LOOP ==========
void loop() {
  webSocket.loop();
  server.handleClient();

  // Пересылка данных: USB (ПК) -> ELM327
  if (Serial.available()) {
    // Перед отправкой новой команды сбрасываем буфер предыдущего ответа ELM327
    flushLine("USB->ELM (partial)");
    
    size_t len = Serial.available();
    uint8_t buf[128];
    len = Serial.readBytes(buf, min(len, sizeof(buf)));
    
    // Отправляем в ELM327
    Serial1.write(buf, len);
    
    // Логируем отправленную команду
    char output[256];
    int pos = snprintf(output, sizeof(output), "[USB->ELM] ");
    for (size_t i = 0; i < len; i++) {
      pos += snprintf(output + pos, sizeof(output) - pos, "%02X ", buf[i]);
    }
    pos += snprintf(output + pos, sizeof(output) - pos, "| ");
    for (size_t i = 0; i < len; i++) {
      uint8_t c = buf[i];
      if (c == '\r') {
        output[pos++] = '\\';
        output[pos++] = 'r';
      } else if (c == '\n') {
        output[pos++] = '\\';
        output[pos++] = 'n';
      } else if (c >= 32 && c <= 126) {
        output[pos++] = (char)c;
      } else {
        output[pos++] = '.';
      }
    }
    output[pos++] = '\n';
    output[pos] = '\0';
    
    broadcastData(output, pos);
    addToLog(output, pos);
  }

  // Обработка данных от ELM327 -> USB с буферизацией
  if (Serial1.available()) {
    while (Serial1.available()) {
      uint8_t b = Serial1.read();
      
      // Передаём в USB (CDC) - это и есть функция моста
      Serial.write(b);
      
      // Добавляем в буфер для сниффера
      if (linePos < MAX_LINE - 1) {
        lineBuffer[linePos++] = b;
      } else {
        // Буфер переполнен - принудительно выводим
        flushLine("ELM->USB (overflow)");
        lineBuffer[linePos++] = b; // начинаем новый буфер с этого байта
      }
      
      // Если это промпт '>', значит ответ закончен
      if (b == '>') {
        flushLine("ELM->USB");
      }
      
      lastCharTime = millis();
    }
  }
  
  // Таймаут: если давно не было данных, считаем ответ завершенным
  if (linePos > 0 && (millis() - lastCharTime > LINE_TIMEOUT)) {
    flushLine("ELM->USB (timeout)");
  }

#ifdef USE_DS18B20
  // Чтение DS18B20
  if (millis() - lastTempRead >= tempInterval) {
    sensors.requestTemperatures();
    float temp = sensors.getTempCByIndex(0);
    if (temp != DEVICE_DISCONNECTED_C && temp != lastTemperature) {
      lastTemperature = temp;
      char msg[64];
      snprintf(msg, sizeof(msg), "[SENSOR] Cabin Temperature: %.1f°C\n", temp);
      broadcastData(msg, strlen(msg));
      addToLog(msg, strlen(msg));
    }
    lastTempRead = millis();
  }
#endif

  delay(1);
}
