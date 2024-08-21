#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "config.h"  // Incluindo o arquivo de configuração

String logMessages[] = { "Reinicio programado", "Dispositivo iniciado", "Falha ao conectar a Wi-Fi", "Email enviado", "Reinicio devido  falha", "Falha na conexao Wi-Fi", "Falha ao enviar email" };

// Variável para armazenar o IP anterior
String previousIP = "";

// Variáveis para controle de tempo
unsigned long lastCheckTime = 0;
unsigned long lastEmailTime = 0;
const unsigned long checkInterval = 60000;       // Intervalo de verificação do IP (1 minuto)
const unsigned long emailInterval = 1000;        // Intervalo para enviar e-mail (ajustar conforme necessário)
const unsigned long restartInterval = 14400000;  // Intervalo de reinicio do dispositivo (14400000 = 4h)

// Configuração de NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -3 * 3600, 60000);  // Fuso horário GMT-3 e atualização a cada 60 segundos

// Configuração do servidor web
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Endereço da EEPROM
const int EEPROM_SIZE = 512;
const int IP_ADDRESS_START = 0;
const int LOG_ADDRESS_START = 1;
const int LOG_SIZE = 5;   // 1 byte para índice + 4 bytes para timestamp
const int MAX_LOGS = 50;  // Máximo de 50 logs

// Função para salvar o IP anterior na EEPROM
void savePreviousIP(String ip) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < ip.length(); i++) {
    EEPROM.write(IP_ADDRESS_START + i, ip[i]);
  }
  EEPROM.write(IP_ADDRESS_START + ip.length(), '\0');  // Null-terminator
  EEPROM.commit();
}

// Função para carregar o IP anterior da EEPROM
String loadPreviousIP() {
  EEPROM.begin(EEPROM_SIZE);
  String ip = "";
  char c;
  for (int i = IP_ADDRESS_START; i < EEPROM_SIZE; i++) {
    c = EEPROM.read(i);
    if (c == '\0') break;
    ip += c;
  }
  return ip;
}

// Função para enviar e-mail
bool sendEmail(String subject, String body) {
  WiFiClientSecure client;
  client.setInsecure();  // Ignorar verificação de certificado

  if (!client.connect(smtp_server, smtp_port)) {
    Serial.println("Falha ao conectar ao servidor SMTP");
    return false;
  }

  // Função auxiliar para enviar comandos SMTP e verificar respostas
  auto sendCommand = [&](const String &command, int expectedCode) -> bool {
    client.println(command);
    delay(100);  // Aguardar uma resposta do servidor
    String response = client.readStringUntil('\n');
    int responseCode = response.substring(0, 3).toInt();
    return responseCode == expectedCode;
  };

  bool status = true;

  // Enviar comandos SMTP e verificar respostas
  if (!sendCommand("EHLO smtp.gmail.com", 250)) {
    Serial.println("Falha no EHLO");
    status = false;
  }
  if (!sendCommand("AUTH LOGIN", 334)) {
    Serial.println("Falha no AUTH LOGIN");
    status = false;
  }
  if (!sendCommand(base64::encode(smtp_user), 334)) {
    Serial.println("Falha no envio do usuário");
    status = false;
  }
  if (!sendCommand(base64::encode(smtp_pass), 235)) {
    Serial.println("Falha no envio da senha");
    status = false;
  }
  if (!sendCommand("MAIL FROM: <" + String(smtp_user) + ">", 250)) {
    Serial.println("Falha no MAIL FROM");
    status = false;
  }
  if (!sendCommand("RCPT TO: <" + String(smtp_user) + ">", 250)) {
    Serial.println("Falha no RCPT TO");
    status = false;
  }
  if (!sendCommand("DATA", 354)) {
    Serial.println("Falha no comando DATA");
    status = false;
  }

  // Enviar conteúdo do e-mail
  client.println("Subject: " + subject);
  client.println("Content-Type: text/plain");
  client.println();
  client.println(body);
  client.println(".");

  // Verificar resposta final do servidor
  if (!sendCommand("QUIT", 221)) {
    Serial.println("Falha ao encerrar a conexão SMTP");
    status = false;
  }

  if (!status) {
    logEvent(6);
    return false;
  }

  Serial.println("E-mail enviado com sucesso!");
  return true;
}

// Função para obter o IP público
String getPublicIP() {
  WiFiClientSecure client;
  client.setInsecure();  // Ignorar verificação de certificado

  HTTPClient http;
  http.begin(client, "https://myip-api.barcelos.dev");
  int httpCode = http.GET();

  String payload = "{}";
  if (httpCode > 0) {
    payload = http.getString();
    int index = payload.indexOf("\"ip\":\"") + 6;
    int endIndex = payload.indexOf("\"", index);
    payload = payload.substring(index, endIndex);
  }
  http.end();

  return payload;
}

void dateTimeNow(const char *label) {
  timeClient.update();

  time_t rawTime = timeClient.getEpochTime();
  struct tm *ti;
  ti = localtime(&rawTime);

  Serial.println("");
  Serial.printf("%s: %02d/%02d/%04d %02d:%02d:%02d\n",
                label,
                ti->tm_mday, ti->tm_mon + 1, ti->tm_year + 1900,
                ti->tm_hour, ti->tm_min, ti->tm_sec);
}

// --

int getCurrentLogIndex() {
  EEPROM.begin(EEPROM_SIZE);
  int currentIndex = EEPROM.read(EEPROM_SIZE - 1);
  if (currentIndex >= MAX_LOGS) {
    currentIndex = 0;
  }
  return currentIndex;
}

void logEvent(int index) {
  EEPROM.begin(EEPROM_SIZE);
  timeClient.update();

  int currentIndex = getCurrentLogIndex();

  int address = currentIndex * LOG_SIZE;

  // Gravar o índice como um byte
  EEPROM.write(address, index);

  // Obter o timestamp atual
  time_t timestamp = timeClient.getEpochTime();

  // Gravar o timestamp como 4 bytes
  EEPROM.write(address + 1, (timestamp >> 24) & 0xFF);
  EEPROM.write(address + 2, (timestamp >> 16) & 0xFF);
  EEPROM.write(address + 3, (timestamp >> 8) & 0xFF);
  EEPROM.write(address + 4, timestamp & 0xFF);

  // Incrementar o índice e salvá-lo no último byte da EEPROM
  currentIndex++;
  if (currentIndex >= MAX_LOGS) {
    currentIndex = 0;
  }
  EEPROM.write(EEPROM_SIZE - 1, currentIndex);

  EEPROM.commit();

  // Formatar a mensagem do log e enviar via WebSocket
  String logMessage = logMessages[index] + " - " + ctime(&timestamp);
  ws.textAll(logMessage);

  Serial.printf("Log gravado: Índice %d, Timestamp %ld\n", index, timestamp);
}

// Função para ler todos os logs da EEPROM e enviar ao cliente WebSocket
void sendAllLogsToClient(AsyncWebSocketClient *client) {
  int currentIndex = getCurrentLogIndex();
  int address;
  byte logIndex;
  time_t timestamp;

  for (int i = 0; i < MAX_LOGS; i++) {
    address = i * LOG_SIZE;

    // Ler o índice do log
    logIndex = EEPROM.read(address);

    // Ler o timestamp como 4 bytes e reconstruí-lo
    timestamp = EEPROM.read(address + 1) << 24;
    timestamp |= EEPROM.read(address + 2) << 16;
    timestamp |= EEPROM.read(address + 3) << 8;
    timestamp |= EEPROM.read(address + 4);

    if (logIndex < sizeof(logMessages) / sizeof(logMessages[0])) {
      // Enviar a mensagem do log para o cliente WebSocket
      String logMessage = logMessages[logIndex] + " - " + ctime(&timestamp);
      client->text(logMessage);
    }
  }
}

// Função para configurar WebSocket
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.println("Cliente WebSocket conectado");
    sendAllLogsToClient(client);  // Enviar todos os logs ao novo cliente
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.println("Cliente WebSocket desconectado");
  }
}

// Função para servir a página web com WebSocket
void handleRoot(AsyncWebServerRequest *request) {
  String html = "<html><body><h1>Logs do Dispositivo</h1><pre id='log'></pre>"
                "<script>"
                "var ws = new WebSocket('ws://' + window.location.hostname + '/ws');"
                "ws.onmessage = function(event) {"
                "  document.getElementById('log').innerHTML += event.data + '\\n';"
                "};"
                "</script></body></html>";
  request->send(200, "text/html", html);
}

// void clearEpprom() {
//   EEPROM.begin(EEPROM_SIZE);
//   for (int i = EEPROM_SIZE; i < 0; i--) {
//     EEPROM.write(i, 0);
//     EEPROM.commit();
//   }
//   ESP.deepSleep(0);
// }

void setup() {
  Serial.begin(115200);
  delay(1000);  // Pequeno atraso para ignorar bytes de inicialização

  WiFi.begin(ssid, password);
  Serial.println("");
  Serial.printf("Wi-Fi Connecting...");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Falha ao conectar! Reiniciando dispositivo");
    logEvent(2);
    logEvent(4);
    ESP.restart();
  }

  // Iniciar o cliente NTP
  timeClient.begin();
  dateTimeNow("Data e Hora de Início");
  Serial.printf("Local IP: %s\n", WiFi.localIP().toString().c_str());

  // --------------------------------------------------------------------

  // Carregar o IP anterior e o flag de reinício
  previousIP = loadPreviousIP();

  if (previousIP) {
    Serial.println("IP anterior carregado: " + previousIP);
  }

  // Configurar WebSocket
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  // Iniciar o servidor web e definir a rota
  server.on("/", HTTP_GET, handleRoot);
  server.begin();
  Serial.println("Servidor web iniciado!");

  logEvent(1);
}

void loop() {
  unsigned long currentMillis = millis();
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();

  // Manter o WebSocket em execução
  ws.cleanupClients();

  // Verificar a conexão Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Falha na conexao Wi-Fi! Reiniciando dispositivo");
    logEvent(5);
    logEvent(4);
    ESP.restart();
  } else {  // explicitamente sera executado somente se estiver conectado a rede.

    if (currentMillis >= restartInterval) {
      Serial.println("Reiniciando o dispositivo...");
      if (sendEmail("Reinicio Programado", "ESP8266 sera reiniciado")) {
        logEvent(3);
      }
      logEvent(0);
      ESP.restart();
    }

    // ---

    // Verificar o IP a cada intervalo
    if (currentMillis - lastCheckTime >= checkInterval) {
      lastCheckTime = currentMillis;
      String currentIP = getPublicIP();

      // Verificar se o IP mudou
      if (currentIP != previousIP && currentIP != "") {
        Serial.println("IP mudou para: " + currentIP);
        if (currentMillis - lastEmailTime >= emailInterval) {
          if (sendEmail("IP Alterado", "Novo IP: " + currentIP)) {
            logEvent(3);
          }
          lastEmailTime = currentMillis;
        }
        previousIP = currentIP;
        savePreviousIP(currentIP);  // Salvar novo IP na EEPROM
      }
    }
  }
}
