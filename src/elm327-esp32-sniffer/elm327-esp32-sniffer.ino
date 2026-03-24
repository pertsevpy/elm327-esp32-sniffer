/**
 * ELM327 UART Sniffer / USB-UART Bridge for ESP32-C6
 * 
 * Функции:
 * - v0.1
 * - Замена FT232: USB (CDC) <-> UART (ELM327)
 * - Сниффер трафика с отображением в браузере по WiFi
 * - Управление скоростью UART через веб-интерфейс
 * - Отправка произвольных команд (текстовых или HEX) через веб-интерфейс
 * - Корректная обработка длинных ответов (буферизация до промпта '>')
 * - Опциональная поддержка датчика DS18B20 (температура салона)
 * - v0.2
 * - Увеличен буфер строки до 1024
 * - Добавлена диагностика ошибок UART
 * - Опция нормализации \n → \r
 * - Убран delay(1) в loop
 * - Добавлена отправка команд через WebSocket (cmd: и hex:)
 * - v0.21
 * - Добавлен отображение uptime (времени работы) в веб-интерфейсе
 * - v0.22
 * - Исправлено переключение форматов отображения (ASCII, HEX, ASCII+HEX) на стороне клиента
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
// Примечание: на ESP32-C6 с USB CDC скорость, заданная в begin(), игнорируется,
// хост сам устанавливает скорость. Оставляем для совместимости.

// Опция нормализации перевода строки (замена \n на \r перед отправкой в ELM327)
#define NORMALIZE_LF_TO_CR true   // true = заменять \n на \r

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
#define MAX_LINE 1024          // увеличенный размер буфера
uint8_t lineBuffer[MAX_LINE];
size_t linePos = 0;
unsigned long lastCharTime = 0;
const unsigned long LINE_TIMEOUT = 50; // 50 мс паузы = конец ответа

// Для диагностики ошибок UART
unsigned long lastUartErrorCheck = 0;
const unsigned long UART_ERROR_INTERVAL = 1000; // проверка раз в секунду

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
      
      // Команда смены скорости
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
      // Запрос uptime
      else if (cmd == "uptime") {
        unsigned long uptimeSec = millis() / 1000;
        char buf[32];
        snprintf(buf, sizeof(buf), "UPTIME:%lu", uptimeSec);
        webSocket.sendTXT(num, buf);
      }
      // Текстовая команда (добавляется \r)
      else if (cmd.startsWith("cmd:")) {
        String textCmd = cmd.substring(4);
        // Добавляем \r, если его нет
        if (!textCmd.endsWith("\r")) {
          textCmd += "\r";
        }
        // Сбрасываем буфер перед отправкой новой команды
        flushLine("WEB->ELM (partial)");
        
        // Отправляем в ELM327
        Serial1.print(textCmd);
        
        // Логируем отправленную команду
        char output[256];
        int pos = snprintf(output, sizeof(output), "[WEB->ELM] ");
        for (size_t i = 0; i < textCmd.length(); i++) {
          pos += snprintf(output + pos, sizeof(output) - pos, "%02X ", (uint8_t)textCmd[i]);
        }
        pos += snprintf(output + pos, sizeof(output) - pos, "| ");
        for (size_t i = 0; i < textCmd.length(); i++) {
          uint8_t c = textCmd[i];
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
        
        webSocket.sendTXT(num, "OK: Command sent\n");
      }
      // HEX-команда (байты, разделённые пробелами)
      else if (cmd.startsWith("hex:")) {
        String hexStr = cmd.substring(4);
        int count = 0;
        int start = 0;
        int end;
        uint8_t buf[128];
        
        while ((end = hexStr.indexOf(' ', start)) != -1) {
          String byteStr = hexStr.substring(start, end);
          if (byteStr.length() == 2) {
            buf[count++] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
          }
          start = end + 1;
        }
        // Последний байт (или единственный)
        String lastByte = hexStr.substring(start);
        if (lastByte.length() == 2) {
          buf[count++] = (uint8_t)strtol(lastByte.c_str(), NULL, 16);
        }
        
        if (count > 0) {
          flushLine("WEB->ELM (partial)");
          Serial1.write(buf, count);
          
          // Логирование
          char output[256];
          int pos = snprintf(output, sizeof(output), "[WEB->ELM] ");
          for (int i = 0; i < count; i++) {
            pos += snprintf(output + pos, sizeof(output) - pos, "%02X ", buf[i]);
          }
          pos += snprintf(output + pos, sizeof(output) - pos, "| ");
          for (int i = 0; i < count; i++) {
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
          
          webSocket.sendTXT(num, "OK: Hex command sent");
        } else {
          webSocket.sendTXT(num, "ERROR: No valid hex bytes");
        }
      }
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
        .controls, .command { margin: 10px 0; display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
        .controls select, .controls button { padding: 8px; font-size: 14px; }
        .command input[type="text"] { flex-grow: 1; padding: 8px; font-family: monospace; }
        .status { margin: 10px 0; padding: 8px; background: #ddd; border-radius: 5px; display: flex; justify-content: space-between; }
        .footer { margin-top: 20px; font-size: 12px; color: #777; }
    </style>
</head>
<body>
    <h1>ELM327 UART Sniffer</h1>
    <div class="status">
        <span id="connStatus">WebSocket: </span>
        <span id="uptimeDisplay">Uptime: --</span>
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
        <select id="format">
            <option value="ascii+hex">ASCII + HEX</option>
            <option value="hex">HEX only</option>
            <option value="ascii">ASCII only</option>
        </select>
    </div>
    <div class="command">
        <input type="text" id="cmdInput" placeholder="Enter command (e.g. ATZ)">
        <button onclick="sendCommand()">Send</button>
        <label>
            <input type="checkbox" id="hexMode"> Hex mode
        </label>
    </div>
    <div id="log"></div>
    <div class="footer">ESP32-C6 ELM327 Sniffer</div>

    <script>
        var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
        var logDiv = document.getElementById('log');
        var connSpan = document.getElementById('connStatus');
        var uptimeSpan = document.getElementById('uptimeDisplay');
        var currentFormat = 'ascii+hex';

        function formatLogLine(line, format) {
            var separatorIndex = line.indexOf('|');
            if (separatorIndex === -1) {
                return line;
            }
            var prefix = line.substring(0, separatorIndex).trim();
            var suffix = line.substring(separatorIndex + 1).trim();
            // Извлекаем направление (часть в квадратных скобках)
            var direction = '';
            var bracketClose = prefix.indexOf(']');
            if (bracketClose !== -1) {
                direction = prefix.substring(0, bracketClose + 1);
            } else {
                direction = prefix;
            }

            switch (format) {
                case 'hex':
                    return prefix + '\n';
                case 'ascii':
                    return direction + ' ' + suffix + '\n';
                default:
                    return prefix + ' | ' + suffix + '\n';
            }
        }

        ws.onopen = function() {
            connSpan.innerHTML = 'WebSocket: <span style="color:green">Connected</span>';
            addLogLine('[System] Connected to sniffer');
            setInterval(function() {
                if (ws.readyState === WebSocket.OPEN) {
                    ws.send("uptime");
                }
            }, 2000);
        };
        ws.onclose = function() {
            connSpan.innerHTML = 'WebSocket: <span style="color:red">Disconnected</span>';
            addLogLine('[System] Disconnected');
        };
        ws.onmessage = function(event) {
            var data = event.data;
            if (data.startsWith("UPTIME:")) {
                var seconds = data.split(":")[1];
                uptimeSpan.innerHTML = "Uptime: " + seconds + " s";
            } else {
                var formatted = formatLogLine(data, currentFormat);
                logDiv.innerHTML += formatted;
                logDiv.scrollTop = logDiv.scrollHeight;
            }
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
            currentFormat = document.getElementById('format').value;
        }

        function sendCommand() {
            var cmd = document.getElementById('cmdInput').value.trim();
            if (cmd === '') return;
            var hexMode = document.getElementById('hexMode').checked;
            
            if (hexMode) {
                var bytes = cmd.split(/\s+/);
                var valid = true;
                for (var i = 0; i < bytes.length; i++) {
                    if (!/^[0-9A-Fa-f]{2}$/.test(bytes[i])) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    alert('Invalid hex format. Use bytes like "22 00 01 0D"');
                    return;
                }
                ws.send('hex:' + cmd);
            } else {
                ws.send('cmd:' + cmd);
            }
            document.getElementById('cmdInput').value = '';
        }

        document.getElementById('format').addEventListener('change', changeFormat);
        currentFormat = document.getElementById('format').value;
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", index_html);
}

// Проверка и логирование ошибок UART
void checkUartErrors() {
  // Проверяем ошибки записи
  if (Serial1.getWriteError()) {
    const char* err = "[UART] Write error detected\n";
    broadcastData(err, strlen(err));
    addToLog(err, strlen(err));
    Serial1.clearWriteError();
  }

  // Проверяем переполнение буфера передачи (если свободно меньше 16 байт)
  if (Serial1.availableForWrite() < 16) {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 5000) { // предупреждаем не чаще 1 раза в 5 секунд
      const char* err = "[UART] TX buffer low (possible congestion)\n";
      broadcastData(err, strlen(err));
      addToLog(err, strlen(err));
      lastWarn = millis();
    }
  }
}

// ========== SETUP ==========
void setup() {
  // Инициализация USB CDC (для связи с ПК). Скорость для CDC не важна, но указываем для совместимости.
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
    
    // Опциональная нормализация \n → \r
    if (NORMALIZE_LF_TO_CR) {
      for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') buf[i] = '\r';
      }
    }
    
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

  // Периодическая проверка ошибок UART
  if (millis() - lastUartErrorCheck > UART_ERROR_INTERVAL) {
    checkUartErrors();
    lastUartErrorCheck = millis();
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

  // delay(1) убрать для максимальной производительности
}
