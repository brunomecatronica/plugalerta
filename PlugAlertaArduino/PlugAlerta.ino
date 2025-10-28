/*
 * ⚡ PLUG ALERTA - Monitor de Falta de Energia
 * 
 * Funcionalidades:
 * - Detecta queda de energia AC via optoacoplador
 * - Alimentação por bateria 18650 quando AC falha
 * - Envia alertas via MQTT
 * - Heartbeat a cada minuto (configurável)
 * - LEDs indicadores (AC, Bateria, MQTT)
 * - Configuração WiFi via portal
 * - Economia de energia com deep sleep
 * 
 * Hardware:
 * - ESP32 DevKit
 * - Fonte AC/DC isolada (ex: HLK-PM01)
 * - Bateria 18650 + TP4056 + Boost 5V
 * - Optoacoplador 4N35 para detecção AC
 * - LEDs indicadores
 * - Botão para configuração WiFi
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>

// ==================== CONFIGURAÇÕES ====================

// Intervalo de heartbeat em minutos (facilmente configurável)
#define HEARTBEAT_INTERVAL_MINUTES 1

// ==================== CONFIGURAÇÃO DE BATERIA ====================
// Escolha o tipo de bateria desejado:

// BATERIA 18650 Li-Ion (padrão)
#define BATTERY_TYPE_18650
#define BATTERY_VOLTAGE_MIN 3.2    // Tensão mínima (0%)
#define BATTERY_VOLTAGE_MAX 4.1    // Tensão máxima (100%)
#define BATTERY_DIVIDER_RATIO 2.0  // Divisor de tensão (1:1 = 2.0)

// BATERIA 9V Alcalina (descomente para usar)
// #define BATTERY_TYPE_9V
// #define BATTERY_VOLTAGE_MIN 6.0
// #define BATTERY_VOLTAGE_MAX 9.0
// #define BATTERY_DIVIDER_RATIO 3.0

// BATERIA 12V Chumbo-Ácido (descomente para usar)
// #define BATTERY_TYPE_12V
// #define BATTERY_VOLTAGE_MIN 10.0
// #define BATTERY_VOLTAGE_MAX 12.6
// #define BATTERY_DIVIDER_RATIO 4.0

// BATERIA 3.7V Li-Po (descomente para usar)
// #define BATTERY_TYPE_LIPO
// #define BATTERY_VOLTAGE_MIN 3.2
// #define BATTERY_VOLTAGE_MAX 4.2
// #define BATTERY_DIVIDER_RATIO 2.0

// Pinos do hardware
// IMPORTANTE: Optoacoplador 4N35/4N25 - Pino 5 (Coletor) conecta no GPIO21
#define PIN_AC_DETECTION 21        // Optoacoplador 4N35 (pino 5 = Coletor → GPIO21)
#define PIN_BATTERY_LEVEL 34       // ADC para nível da bateria
#define PIN_LED_AC 14             // LED Verde - Tensão AC
#define PIN_LED_BATTERY 27        // LED Vermelho - Bateria
#define PIN_LED_MQTT 26           // LED Azul - MQTT
#define PIN_BUTTON_CONFIG 23      // Botão Config: Um terminal → GPIO23, outro → GND
#define PIN_BUTTON_TEST 25        // Botão Teste: Um terminal → GPIO25, outro → GND (opcional)

// Configurações MQTT
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;

// Tópicos MQTT
const char* topic_alert = "plugalerta/alert";
const char* topic_heartbeat = "plugalerta/heartbeat";
const char* topic_status = "plugalerta/status";
const char* topic_battery = "plugalerta/battery";

// ==================== VARIÁVEIS GLOBAIS ====================

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

// Estados do sistema
bool acPowerPresent = false;
bool lastAcPowerState = false;
bool mqttConnected = false;
bool batteryLow = false;
bool batteryPresent = false;  // Flag para indicar se bateria está presente

// Timers
unsigned long lastHeartbeat = 0;
unsigned long lastBatteryCheck = 0;
unsigned long lastAcCheck = 0;
unsigned long lastDeepSleep = 0;

// Configurações de economia de energia
const unsigned long DEEP_SLEEP_INTERVAL = 30000;  // 30 segundos em modo normal
const unsigned long BATTERY_CHECK_INTERVAL = 60000; // 1 minuto para verificar bateria
const unsigned long AC_CHECK_INTERVAL = 1000;     // 1 segundo para verificar AC

// Buffer para mensagens MQTT
#define MSG_BUFFER_SIZE 100
char msg[MSG_BUFFER_SIZE];

// ==================== CONFIGURAÇÃO INICIAL ====================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("⚡ PLUG ALERTA - Iniciando...");
  
  // Configurar pinos
  pinMode(PIN_AC_DETECTION, INPUT_PULLUP);  // IMPORTANTE: Adicionar pull-up interno
  pinMode(PIN_BATTERY_LEVEL, INPUT);
  pinMode(PIN_LED_AC, OUTPUT);        // LED Verde - Tensão AC
  pinMode(PIN_LED_BATTERY, OUTPUT);   // LED Vermelho - Bateria
  pinMode(PIN_LED_MQTT, OUTPUT);      // LED Azul - MQTT
  pinMode(PIN_BUTTON_CONFIG, INPUT_PULLUP);
  pinMode(PIN_BUTTON_TEST, INPUT_PULLUP);
  
  // Inicializar LEDs
  // Verde (AC): Apagado
  // Vermelho (Bateria): Apagado
  // Azul (MQTT): ACESO (sinaliza desconexão inicial)
  digitalWrite(PIN_LED_AC, LOW);
  digitalWrite(PIN_LED_BATTERY, LOW);
  digitalWrite(PIN_LED_MQTT, HIGH);  // ACESO = desconectado
  
  // Verificar estado inicial da energia AC
  acPowerPresent = digitalRead(PIN_AC_DETECTION);
  lastAcPowerState = acPowerPresent;
  
  Serial.print("Estado inicial AC (RAW): ");
  Serial.println(acPowerPresent ? "HIGH" : "LOW");
  
  // IMPORTANTE: Se a lógica estiver invertida, inverter aqui
  // Com optoacoplador 4N25/4N35: LOW = energia presente, HIGH = sem energia
  acPowerPresent = !acPowerPresent;  // Inverter lógica
  
  Serial.print("Estado inicial AC (CORRIGIDO): ");
  Serial.println(acPowerPresent ? "PRESENTE" : "AUSENTE");
  
  // Informar modo de alimentação
  Serial.println("📱 MODO: Alimentação USB (bateria não presente)");
  Serial.println("ℹ️ Deep sleep desabilitado");
  
  // Configurar WiFi
  setupWiFi();
  
  // Configurar servidor web
  setupWebServer();
  
  // Configurar MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  
  // Conectar ao MQTT
  connectMQTT();
  
  // Configurar OTA
  setupOTA();
  
  // Enviar status inicial
  sendStatus();
  
  Serial.println("✅ Sistema inicializado com sucesso!");
}

// ==================== LOOP PRINCIPAL ====================

void loop() {
  // Verificar botão de configuração
  checkConfigButton();
  
  // Verificar energia AC
  checkAcPower();
  
  // Verificar nível da bateria
  // DESABILITADO: Não verificar bateria quando não está presente
  // Descomente quando conectar bateria
  // checkBatteryLevel();
  
  // Manter conexão MQTT
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();
  
  // Manter OTA ativo (com limite de tempo)
  static unsigned long lastOTACheck = 0;
  if (millis() - lastOTACheck > 10) {
    ArduinoOTA.handle();
    lastOTACheck = millis();
  }
  
  // Manter servidor web ativo (com limite de tempo)
  static unsigned long lastServerCheck = 0;
  if (millis() - lastServerCheck > 10) {
    server.handleClient();
    lastServerCheck = millis();
  }
  
  // Enviar heartbeat
  sendHeartbeat();
  
  // Atualizar LEDs
  updateLEDs();
  
  // Economia de energia - Deep Sleep quando em bateria
  // DESABILITADO: Não usar deep sleep quando alimentado por USB
  // Descomente quando implementar sistema de bateria
  /*
  if (!acPowerPresent && !batteryLow) {
    // Em modo bateria, usar deep sleep para economizar
    if (millis() - lastDeepSleep > DEEP_SLEEP_INTERVAL) {
      Serial.println("💤 Entrando em deep sleep (modo bateria)");
      esp_deep_sleep_start();
    }
  }
  */
  
  // Pequeno delay para estabilidade
  delay(100);
}

// ==================== DETECÇÃO DE ENERGIA AC ====================

void checkAcPower() {
  if (millis() - lastAcCheck < AC_CHECK_INTERVAL) return;
  lastAcCheck = millis();
  
  // Ler valor bruto e inverter lógica
  bool acPowerRaw = digitalRead(PIN_AC_DETECTION);
  acPowerPresent = !acPowerRaw;  // Inverter: LOW do opto = energia presente
  
  // Debug: Mostrar valores a cada 10 segundos
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 10000) {
    lastDebug = millis();
    Serial.print("⚡ Tensão: ");
    Serial.print(acPowerPresent ? "ATIVA" : "DESATIVADA");
    Serial.print(" | AC Debug - Raw: ");
    Serial.print(acPowerRaw ? "HIGH" : "LOW");
    Serial.print(" | Invertido: ");
    Serial.println(acPowerPresent ? "PRESENTE" : "AUSENTE");
  }
  
  // Detectou mudança de estado
  if (acPowerPresent != lastAcPowerState) {
    lastAcPowerState = acPowerPresent;
    
    if (acPowerPresent) {
      Serial.println("⚡ Tensão: ATIVA - Energia AC RESTAURADA");
      sendAlert("AC_RESTORED", "Energia AC foi restaurada");
    } else {
      Serial.println("⚠️ Tensão: DESATIVADA - FALTA DE ENERGIA AC DETECTADA!");
      sendAlert("AC_LOST", "Falta de energia AC detectada");
    }
    
    sendStatus();
  }
}

// ==================== VERIFICAÇÃO DE BATERIA ====================

void checkBatteryLevel() {
  // Verificar se bateria está presente
  if (!batteryPresent) {
    Serial.println("⚠️ Bateria não está presente no sistema");
    return;
  }
  
  if (millis() - lastBatteryCheck < BATTERY_CHECK_INTERVAL) return;
  lastBatteryCheck = millis();
  
  // Fazer múltiplas leituras para maior precisão
  int totalReadings = 0;
  for (int i = 0; i < 10; i++) {
    totalReadings += analogRead(PIN_BATTERY_LEVEL);
    delay(10);
  }
  int batteryRaw = totalReadings / 10;
  
  // Debug: Mostrar valor RAW do ADC
  Serial.print("🔋 ADC Raw: ");
  Serial.print(batteryRaw);
  
  // Converter para tensão usando configuração selecionada
  float batteryVoltage = (batteryRaw / 4095.0) * 3.3 * BATTERY_DIVIDER_RATIO;
  
  // Se a bateria estiver conectada mas mostrando tensão muito baixa (< 2V),
  // pode ser problema de conexão ou divisor de tensão
  if (batteryVoltage < 2.0 && batteryVoltage > 0.5) {
    Serial.print(" ⚠️ Tensão muito baixa! Verifique conexões e divisor.");
  }
  
  // Calcular porcentagem usando tensões configuradas
  int batteryPercent = map(
    constrain(batteryVoltage * 100, BATTERY_VOLTAGE_MIN * 100, BATTERY_VOLTAGE_MAX * 100), 
    BATTERY_VOLTAGE_MIN * 100, 
    BATTERY_VOLTAGE_MAX * 100, 
    0, 
    100
  );
  
  // Verificar se bateria está baixa (< 20%)
  // DESABILITAR temporariamente se não tiver bateria conectada
  bool newBatteryLow = false;  // Desabilitado temporariamente
  // bool newBatteryLow = (batteryPercent < 20);  // Descomente quando conectar bateria
  
  if (newBatteryLow != batteryLow) {
    batteryLow = newBatteryLow;
    if (batteryLow) {
      Serial.println("🔋 BATERIA BAIXA!");
      sendAlert("BATTERY_LOW", "Bateria baixa: " + String(batteryPercent) + "%");
    }
  }
  
  // Enviar nível da bateria
  snprintf(msg, MSG_BUFFER_SIZE, "{\"voltage\":%.2f,\"percent\":%d}", batteryVoltage, batteryPercent);
  client.publish(topic_battery, msg);
  
  Serial.print("🔋 Bateria: ");
  Serial.print(batteryVoltage);
  Serial.print("V (");
  Serial.print(batteryPercent);
  Serial.println("%)");
}

// ==================== SISTEMA DE ALERTAS ====================

void sendAlert(String type, String message) {
  if (!client.connected()) return;
  
  snprintf(msg, MSG_BUFFER_SIZE, 
    "{\"type\":\"%s\",\"message\":\"%s\",\"timestamp\":%ld,\"ac_power\":%s,\"battery_low\":%s}",
    type.c_str(), message.c_str(), millis(), 
    acPowerPresent ? "true" : "false",
    batteryLow ? "true" : "false"
  );
  
  client.publish(topic_alert, msg);
  Serial.println("📤 Alerta enviado: " + message);
}

void sendHeartbeat() {
  if (millis() - lastHeartbeat < (HEARTBEAT_INTERVAL_MINUTES * 60000)) return;
  lastHeartbeat = millis();
  
  if (!client.connected()) return;
  
  snprintf(msg, MSG_BUFFER_SIZE, 
    "{\"status\":\"online\",\"ac_power\":%s,\"battery_low\":%s,\"uptime\":%ld}",
    acPowerPresent ? "true" : "false",
    batteryLow ? "true" : "false",
    millis() / 1000
  );
  
  client.publish(topic_heartbeat, msg, true);  // true = retained message
  
  // Mostrar status no serial
  Serial.print("💓 Heartbeat: AC=");
  Serial.print(acPowerPresent ? "ATIVA" : "DESATIVADA");
  Serial.print(" | Bateria=");
  Serial.print(batteryLow ? "BAIXA" : "OK");
  Serial.print(" | Uptime=");
  Serial.print(millis() / 1000);
  Serial.println("s");
}

void sendStatus() {
  if (!client.connected()) return;
  
  snprintf(msg, MSG_BUFFER_SIZE, 
    "{\"ac_power\":%s,\"battery_low\":%s,\"mqtt_connected\":%s}",
    acPowerPresent ? "true" : "false",
    batteryLow ? "true" : "false",
    mqttConnected ? "true" : "false"
  );
  
  client.publish(topic_status, msg);
  
  // Também enviar heartbeat para apps receberem status atualizado
  snprintf(msg, MSG_BUFFER_SIZE, 
    "{\"status\":\"online\",\"ac_power\":%s,\"battery_low\":%s,\"uptime\":%ld}",
    acPowerPresent ? "true" : "false",
    batteryLow ? "true" : "false",
    millis() / 1000
  );
  client.publish(topic_heartbeat, msg, true);  // true = retained
  
  // Mostrar status no serial quando mudar estado
  Serial.print("📊 Status: AC=");
  Serial.print(acPowerPresent ? "ATIVA" : "DESATIVADA");
  Serial.print(" | MQTT=");
  Serial.println(mqttConnected ? "Conectado" : "Desconectado");
}

// ==================== SISTEMA DE LEDs ====================

void updateLEDs() {
  // LED Verde (GPIO14) - Tensão AC presente
  digitalWrite(PIN_LED_AC, acPowerPresent ? HIGH : LOW);
  
  // LED Vermelho (GPIO27) - Bateria
  // Apagado = OK ou não presente, ACESO = Bateria baixa
  if (!batteryPresent) {
    digitalWrite(PIN_LED_BATTERY, LOW);  // Apagado se bateria não presente
  } else {
    digitalWrite(PIN_LED_BATTERY, batteryLow ? HIGH : LOW);  // ACESO = bateria baixa
  }
  
  // LED Azul (GPIO26) - MQTT
  // Pisca quando conectado, ACESO quando desconectado
  if (mqttConnected) {
    static unsigned long lastMqttBlink = 0;
    if (millis() - lastMqttBlink > 1000) {
      digitalWrite(PIN_LED_MQTT, !digitalRead(PIN_LED_MQTT));
      lastMqttBlink = millis();
    }
  } else {
    digitalWrite(PIN_LED_MQTT, HIGH);  // ACESO quando desconectado
  }
}

// ==================== CONFIGURAÇÃO WIFI ====================

void setupWiFi() {
  WiFiManager wifiManager;
  
  // Callback para modo AP
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  
  // Tentar conectar ou criar portal
  if (!wifiManager.autoConnect("PlugAlerta_Config", "12345678")) {
    Serial.println("❌ Falha ao conectar WiFi");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("✅ WiFi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void checkConfigButton() {
  static unsigned long lastPress = 0;
  static bool buttonPressed = false;
  static unsigned long pressStart = 0;
  
  // Detectar quando botão é pressionado
  if (digitalRead(PIN_BUTTON_CONFIG) == LOW && !buttonPressed) {
    buttonPressed = true;
    pressStart = millis();
  }
  
  // Detectar quando botão é solto
  if (digitalRead(PIN_BUTTON_CONFIG) == HIGH && buttonPressed) {
    buttonPressed = false;
    unsigned long pressDuration = millis() - pressStart;
    
    if (pressDuration >= 2000) {
      Serial.println("🔧 Botão pressionado! Iniciando configuração WiFi...");
      Serial.println("⏳ Aguarde, parando serviços...");
      
      // Parar OTA
      ArduinoOTA.end();
      delay(100);
      
      // Parar servidor web
      server.stop();
      delay(100);
      
      // Parar cliente MQTT
      client.disconnect();
      delay(100);
      
      Serial.println("✅ Serviços parados!");
      delay(500);
      
      // Iniciar portal de configuração
      startConfigPortal();
    }
  }
}

void startConfigPortal() {
  Serial.println("🔧 Iniciando portal de configuração...");
  
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalTimeout(180);
  
  // Personalizar interface do portal com tema Plug Alerta
  String customStyle = "<style>";
  customStyle += "body{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);font-family:Arial;color:#fff;margin:0;padding:0;}";
  customStyle += "h1{color:#fff;text-align:center;padding:20px;font-size:32px;margin:0;text-shadow:2px 2px 4px rgba(0,0,0,0.3);}";
  customStyle += ".form-group{background:rgba(255,255,255,0.95);color:#333;padding:20px;border-radius:12px;margin:15px 0;box-shadow:0 5px 15px rgba(0,0,0,0.2);}";
  customStyle += "button{background:#667eea;color:#fff;padding:15px 30px;border:none;border-radius:10px;cursor:pointer;width:100%;font-size:16px;font-weight:bold;margin:10px 0;transition:all 0.3s;}";
  customStyle += "button:hover{background:#5568d3;transform:translateY(-2px);box-shadow:0 5px 15px rgba(0,0,0,0.3);}";
  customStyle += "input{padding:12px;border-radius:8px;border:2px solid #ddd;width:calc(100% - 24px);margin:8px 0;font-size:14px;box-sizing:border-box;}";
  customStyle += "input:focus{border-color:#667eea;outline:none;}";
  customStyle += ".title{border-bottom:3px solid #fff;padding-bottom:10px;margin-bottom:20px;}";
  customStyle += "</style>";
  wifiManager.setCustomHeadElement(customStyle.c_str());
  
  Serial.println("📡 Criando rede WiFi: PlugAlerta_Config");
  Serial.println("🔑 Senha: 12345678");
  Serial.println("🌐 Conecte-se a essa rede e abra: http://192.168.4.1");
  Serial.println("⏱️  Timeout: 3 minutos");
  
  if (!wifiManager.startConfigPortal("PlugAlerta_Config", "12345678")) {
    Serial.println("❌ Timeout ou falha na configuração");
    Serial.println("🔄 Reiniciando em 3 segundos...");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("✅ WiFi configurado com sucesso!");
  Serial.print("📶 Conectado a: ");
  Serial.println(WiFi.SSID());
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.localIP());
  
  delay(2000);
  
  // Reiniciar todos os serviços
  Serial.println("🔄 Reiniciando serviços...");
  
  // Reiniciar servidor web
  server.begin();
  
  // Reiniciar OTA
  ArduinoOTA.begin();
  
  // Reconectar MQTT
  connectMQTT();
  
  Serial.println("✅ Sistema totalmente reiniciado!");
}

// ==================== MQTT ====================

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("🔌 Conectando MQTT...");
    
    String clientId = "PlugAlerta-";
    clientId += String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("✅ MQTT conectado!");
      mqttConnected = true;
      
      // Enviar heartbeat imediato (retained) para atualizar apps conectados
      snprintf(msg, MSG_BUFFER_SIZE, 
        "{\"status\":\"online\",\"ac_power\":%s,\"battery_low\":%s,\"uptime\":%ld}",
        acPowerPresent ? "true" : "false",
        batteryLow ? "true" : "false",
        millis() / 1000
      );
      client.publish(topic_heartbeat, msg, true);  // true = retained
      
      // Mostrar heartbeat inicial no serial
      Serial.print("📤 Heartbeat inicial: AC=");
      Serial.print(acPowerPresent ? "ATIVA" : "DESATIVADA");
      Serial.print(" | Bateria=");
      Serial.print(batteryLow ? "BAIXA" : "OK");
      Serial.println(" (retained)");
      
      // Subscrever tópicos se necessário
      // client.subscribe("plugalerta/command");
      
    } else {
      Serial.print("❌ Falha MQTT, rc=");
      Serial.print(client.state());
      Serial.println(" - Tentando novamente em 5s");
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("📨 MQTT recebido [");
  Serial.print(topic);
  Serial.print("]: ");
  
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  
  // Processar comandos recebidos
  if (String(topic) == "plugalerta/command") {
    if (message == "status") {
      sendStatus();
    } else if (message == "heartbeat") {
      sendHeartbeat();
    }
  }
}

// ==================== CALLBACKS ====================

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("🔧 Modo configuração ativado");
  Serial.println("IP do AP: " + WiFi.softAPIP().toString());
  Serial.println("SSID: " + myWiFiManager->getConfigPortalSSID());
}

void saveConfigCallback() {
  Serial.println("💾 Configuração WiFi salva");
}

// ==================== SERVIDOR WEB ====================

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/api/status", handleAPIStatus);
  server.on("/config", handleConfig);
  server.on("/reset", handleReset);
  server.begin();
  Serial.println("Servidor web iniciado!");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Plug Alerta</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#667eea;padding:20px;margin:0;}";
  html += ".card{background:white;border-radius:15px;padding:30px;max-width:800px;margin:0 auto;box-shadow:0 10px 30px rgba(0,0,0,0.2);}";
  html += "h1{color:#667eea;margin-bottom:10px;}";
  html += ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:15px;margin-top:20px;}";
  html += ".status-item{background:#f8f9fa;padding:20px;border-radius:10px;text-align:center;}";
  html += ".status-label{color:#666;font-size:14px;margin-bottom:8px;}";
  html += ".status-value{font-size:24px;font-weight:bold;}";
  html += ".on{color:#28a745;}";
  html += ".off{color:#dc3545;}";
  html += ".pulse{animation:pulse 2s infinite;}";
  html += "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}";
  html += ".btn{background:#667eea;color:white;padding:12px 24px;border:none;border-radius:8px;cursor:pointer;font-size:16px;margin-top:15px;}";
  html += ".btn:hover{background:#5568d3;}";
  html += ".info{background:#e7f3ff;padding:15px;border-radius:8px;margin-top:15px;border-left:4px solid #667eea;}";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>Plug Alerta</h1>";
  html += "<p style='color:#666'>Monitor de Falta de Energia</p>";
  html += "<div class='status-grid'>";
  html += "<div class='status-item'><div class='status-label'>Tensao AC</div><div class='status-value' id='ac'>-</div></div>";
  html += "<div class='status-item'><div class='status-label'>Bateria</div><div class='status-value' id='bat'>-</div></div>";
  html += "<div class='status-item'><div class='status-label'>MQTT</div><div class='status-value' id='mqtt'>-</div></div>";
  html += "</div>";
  html += "<button class='btn' onclick='update()' style='margin-right:10px'>Atualizar</button>";
  html += "<button class='btn' onclick=\"window.location.href='/config'\" style='background:#dc3545'>Configurar WiFi</button>";
  html += "<div class='info'>";
  html += "<strong>IP:</strong> <span id='ip'>-</span><br>";
  html += "<strong>WiFi:</strong> <span id='ssid'>-</span><br>";
  html += "<strong>Uptime:</strong> <span id='time'>-</span>";
  html += "</div>";
  html += "</div>";
  html += "<script>";
  html += "async function update(){const r=await fetch('/api/status');const d=await r.json();";
  html += "document.getElementById('ac').innerText=d.ac_power?'ATIVA':'DESATIVADA';";
  html += "document.getElementById('ac').className='status-value '+(d.ac_power?'on':'off');";
  html += "document.getElementById('bat').innerText=d.battery_present?d.battery_percent+'%':'N/A';";
  html += "document.getElementById('bat').className='status-value '+(d.battery_low?'off':'on');";
  html += "document.getElementById('mqtt').innerText=d.mqtt_connected?'Conectado':'Desconectado';";
  html += "document.getElementById('mqtt').className='status-value '+(d.mqtt_connected?'on pulse':'off');";
  html += "document.getElementById('ip').innerText=d.ip||'-';";
  html += "document.getElementById('ssid').innerText=d.ssid||'-';";
  html += "document.getElementById('time').innerText=d.uptime+'s';";
  html += "}update();setInterval(update,5000);";
  html += "</script></body></html>";
  
  server.send(200, "text/html", html);
}

void handleAPIStatus() {
  String json = "{";
  json += "\"ac_power\":" + String(acPowerPresent ? "true" : "false") + ",";
  json += "\"battery_present\":" + String(batteryPresent ? "true" : "false") + ",";
  json += "\"battery_low\":" + String(batteryLow ? "true" : "false") + ",";
  json += "\"battery_percent\":0,";
  json += "\"mqtt_connected\":" + String(mqttConnected ? "true" : "false") + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleConfig() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>Configurar WiFi</title>";
  html += "<style>body{font-family:Arial;background:#667eea;padding:20px;}";
  html += ".card{background:white;border-radius:15px;padding:30px;max-width:600px;margin:0 auto;}";
  html += "h1{color:#667eea;}";
  html += ".btn{background:#667eea;color:white;padding:12px 24px;border:none;border-radius:8px;cursor:pointer;font-size:16px;margin-top:15px;}";
  html += ".btn:hover{background:#5568d3;}";
  html += ".info{background:#fff3cd;padding:15px;border-radius:8px;margin-bottom:20px;border-left:4px solid #ffc107;}";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>Configurar WiFi</h1>";
  html += "<div class='info'>";
  html += "<strong>Para configurar WiFi:</strong><br>";
  html += "1. Pressione o botao fisico de configuracao (GPIO23) por 2 segundos<br>";
  html += "2. Conecte-se a rede 'PlugAlerta_Config' (senha: 12345678)<br>";
  html += "3. Uma pagina de configuracao abrira automaticamente<br>";
  html += "4. Escolha sua rede WiFi e digite a senha<br>";
  html += "5. Apos conectar, volte aqui e veja o status!";
  html += "</div>";
  html += "<button class='btn' onclick=\"window.location.href='/'\" style='background:#6c757d'>Voltar</button>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleReset() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>Reset</title>";
  html += "<style>body{font-family:Arial;background:#667eea;padding:20px;}";
  html += ".card{background:white;border-radius:15px;padding:30px;max-width:600px;margin:0 auto;text-align:center;}";
  html += ".btn{background:#dc3545;color:white;padding:12px 24px;border:none;border-radius:8px;cursor:pointer;font-size:16px;}";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>Reset WiFi</h1>";
  html += "<p>Pressionar o botao abaixo ira reiniciar o ESP32.</p>";
  html += "<p>Depois reinicie, pressione o botao de configuracao fisico para configurar a rede novamente.</p>";
  html += "<button class='btn' onclick='location.href=\"/doreset\"'>Reiniciar ESP32</button>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

// ==================== OTA (Over-The-Air Updates) ====================

void setupOTA() {
  // Configurar hostname
  ArduinoOTA.setHostname("PlugAlerta");
  
  // Configurar senha (opcional, mas recomendado)
  ArduinoOTA.setPassword("plugalerta123");
  
  // Callbacks para monitoramento
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("🔄 Iniciando atualização OTA: " + type);
    
    // Piscar LED Azul (MQTT) durante atualização
    digitalWrite(PIN_LED_MQTT, HIGH);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("✅ Atualização OTA concluída!");
    digitalWrite(PIN_LED_MQTT, LOW);
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("📊 Progresso OTA: %u%%\r", (progress / (total / 100)));
    
    // Piscar LED Verde (AC) durante progresso
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 200) {
      digitalWrite(PIN_LED_AC, !digitalRead(PIN_LED_AC));
      lastBlink = millis();
    }
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("❌ Erro OTA [%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Falha de autenticação");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Falha ao iniciar");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Falha de conexão");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Falha ao receber");
    else if (error == OTA_END_ERROR) Serial.println("Falha ao finalizar");
    
    // Piscar LED Vermelho (Bateria) em caso de erro
    for (int i = 0; i < 5; i++) {
      digitalWrite(PIN_LED_BATTERY, HIGH);
      delay(100);
      digitalWrite(PIN_LED_BATTERY, LOW);
      delay(100);
    }
  });
  
  // Iniciar OTA
  ArduinoOTA.begin();
  
  Serial.println("🔄 OTA configurado!");
  Serial.println("📱 Hostname: PlugAlerta");
  Serial.println("🔑 Senha: plugalerta123");
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.localIP());
}