#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "config.h"  // Incluindo o arquivo de configuração

// Variável para armazenar o IP anterior
String previousIP = "";

// Variáveis para controle de tempo
unsigned long lastCheckTime = 0;
unsigned long lastEmailTime = 0;
const unsigned long checkInterval = 60000;  // Intervalo de verificação do IP (1 minuto)
const unsigned long emailInterval = 1000;   // Intervalo para enviar e-mail (ajustar conforme necessário)
bool hasRestartedToday = false;

// Configuração de NTP
WiFiUDP ntpUDP;
int utcOffsetInSeconds = -3 * 3600;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds, 60000);  // Sincroniza a cada minuto

// Endereço da EEPROM
const int EEPROM_SIZE = 512;
const int IP_ADDRESS_START = 0;
const int RESTART_FLAG_ADDRESS = 100;  // Endereço para armazenar o flag de reinício

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

// Função para salvar o estado do reinício na EEPROM
void saveRestartFlag(bool hasRestarted) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(RESTART_FLAG_ADDRESS, hasRestarted ? 1 : 0);
  EEPROM.commit();
}

// Função para carregar o estado do reinício da EEPROM
bool loadRestartFlag() {
  EEPROM.begin(EEPROM_SIZE);
  return EEPROM.read(RESTART_FLAG_ADDRESS) == 1;
}

// Função para enviar e-mail
void sendEmail(String subject, String body) {
  WiFiClientSecure client;
  client.setInsecure();  // Ignorar verificação de certificado

  if (!client.connect(smtp_server, smtp_port)) {
    Serial.println("Falha ao conectar ao servidor SMTP");
    return;
  }

  // Enviar comandos SMTP
  client.println("EHLO smtp.gmail.com");
  client.println("AUTH LOGIN");
  client.println(base64::encode(smtp_user));  // Enviar usuário em base64
  client.println(base64::encode(smtp_pass));  // Enviar senha em base64
  client.println("MAIL FROM: <" + String(smtp_user) + ">");
  client.println("RCPT TO: <" + String(smtp_user) + ">");  // Estou enviando para mim mesmo
  client.println("DATA");
  client.println("Subject: " + subject);
  client.println("Content-Type: text/plain");
  client.println();
  client.println(body);
  client.println(".");
  client.println("QUIT");
  client.stop();

  Serial.println("E-mail enviado!");
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

// Função para conectar ao Wi-Fi
void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.println("Tentando conectar ao Wi-Fi...");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // Tenta conectar por 20 tentativas
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado ao Wi-Fi!");
  } else {
    Serial.println("\nFalha ao conectar ao Wi-Fi. Tentando novamente...");
  }
}

void setup() {
  Serial.begin(115200);
  connectToWiFi();  // Conectar ao Wi-Fi

  // Conectar ao Wi-Fi
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Conectando ao Wi-Fi...");
  }

  Serial.println("Conectado!");

  // Iniciar o cliente NTP
  timeClient.begin();

  // Carregar o IP anterior e o flag de reinício
  previousIP = loadPreviousIP();
  bool hasRestartedToday = loadRestartFlag();

  Serial.println("IP anterior carregado: " + previousIP);
}

void loop() {
  unsigned long currentMillis = millis();
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();

  // Verificar a conexão Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();  // Tentar reconectar se a conexão for perdida
  } else { // explicitamente sera executado somente se estiver conectado a rede.

    // Verificar se é 4h30 da manhã para reiniciar o dispositivo
    if (currentHour == 4 && currentMinute == 30 && !hasRestartedToday) {
      Serial.println("Reiniciando o dispositivo...");
      sendEmail("Reinicio Programado", "ESP8266 sera reiniciado");
      hasRestartedToday = true;  // Marca que o reinício já foi feito hoje
      saveRestartFlag(true);     // Marca que o reinício foi feito hoje
      ESP.restart();
    }

    // Se já passou o horário de reinício, permitir reinício novamente no próximo dia
    if (currentHour != 4 || currentMinute != 30 && hasRestartedToday) {
      hasRestartedToday = false;
      saveRestartFlag(false);  // Marca que o reinício foi feito hoje
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
          sendEmail("IP Alterado", "Novo IP: " + currentIP);
          lastEmailTime = currentMillis;
        }
        previousIP = currentIP;
        savePreviousIP(currentIP);  // Salvar novo IP na EEPROM
      }
    }
  }
}
