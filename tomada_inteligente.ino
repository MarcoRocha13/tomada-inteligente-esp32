// ============================================================================
// Tomada inteligente com monitoramento e controle remoto (ESP32 + PZEM-004T)
// Firmware do protótipo do TCC. Antes de compilar, crie o arquivo secrets.h
// (copie de secrets.example.h) com os dados do seu broker MQTT.
// ============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <WiFiManager.h>   // https://github.com/tzapu/WiFiManager
#include <Preferences.h>
#include <esp_system.h>   // esp_reset_reason()

// Credenciais do broker MQTT: copie secrets.example.h para secrets.h e preencha.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Crie o arquivo secrets.h a partir de secrets.example.h (veja o README)."
#endif

// =============================
// VARIAVEIS GLOBAIS - LEITURAS
// =============================

float voltage = 0;
float current = 0;
float power = 0;
float energy = 0;
float frequency = 0;
float pf = 0;
bool mqttOK = false;

// Falha de comunicacao com o PZEM (a biblioteca devolve NaN quando nao ha resposta)
int falhasPzem = 0;
const int LIMITE_FALHAS_PZEM = 10;   // 10 leituras x 500 ms = ~5 s
bool medidorEmFalha = false;
bool falhaNotificada = false;

volatile bool configPendente = false;
String configRecebidaJson = "";

unsigned long ultimaLeitura = 0;
const unsigned long INTERVALO_LEITURA = 500; // ms

unsigned long ultimoOLED = 0;
String proxPub = "--:--";

unsigned long ultimaTentativaMQTT = 0;
const unsigned long intervaloReconexaoMQTT = 5000;

// =============================
// RELE, LEDS, BOTAO
// =============================

#define RELE_PIN         26
#define LED_VERDE_PIN    25
#define LED_VERMELHO_PIN 33
#define BOTAO_RESET_PIN  0

// Logica do rele: ATIVO EM HIGH (invertido). Para voltar ao ativo em LOW, troque os valores.
#define RELE_NIVEL_LIGADO    HIGH
#define RELE_NIVEL_DESLIGADO LOW

bool cargaLigada = false;

bool botaoPressionado = false;
unsigned long inicioPressionamentoBotao = 0;
const unsigned long TEMPO_SEGURAR_RESET = 3000;

// =====================================================
// OLED
// =====================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =====================================================
// PZEM
// =====================================================
PZEM004Tv30 pzem(Serial2, 16, 17);

// =====================================================
// Tensao fixa (padrao ANEEL para rede nominal de 127V)
// =====================================================
const float TENSAO_NOMINAL = 127.0;
const float TENSAO_MIN = 117.0;  // faixa adequada para 127 V: 117 V a 133 V (PRODIST, modulo 8)
const float TENSAO_MAX = 133.0;
// Disjuntor instalado no sistema: 20 A (corrente maxima de residencias).
// Potencia maxima configuravel = 20 A x 127 V = 2540 W.
const float CORRENTE_MAX_SISTEMA = 20.0;

// Qualquer corrente ACIMA deste valor e tratada como CURTO-CIRCUITO:
// desligamento imediato (sem esperar as 3 leituras de confirmacao).
const float CORRENTE_CURTO_CIRCUITO = 20.0;

// =====================================================
// Configuracao persistida
// =====================================================
Preferences prefs;

float maxPower = 1500.0;
float maxCurrent = 0.0;

void calcularLimites() {
    maxCurrent = maxPower / TENSAO_NOMINAL;
}

int contadorViolacao = 0;
const int LIMITE_VIOLACOES = 3;

// =====================================================
// NTP
// =====================================================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;

// =====================================================
// HiveMQ Cloud
// =====================================================
// Os dados de acesso ao broker ficam em secrets.h (nao versionado). Veja secrets.example.h.
const char* mqttServer = MQTT_SERVER;
const int mqttPort = MQTT_PORT;
const char* mqttUser = MQTT_USER;
const char* mqttPassword = MQTT_PASSWORD;

const char* topicTelemetry     = "casa/quadrogeral/telemetria";
const char* topicCommand       = "casa/quadrogeral/comando";
const char* topicStatus        = "casa/quadrogeral/status";
const char* topicAlert         = "casa/quadrogeral/alerta";
const char* topicLimites       = "casa/quadrogeral/limites";
const char* topicConfigSet     = "casa/quadrogeral/config/set";    // NOVO: recebe nova potencia
const char* topicConfigStatus  = "casa/quadrogeral/config/status"; // NOVO: responde sucesso/erro
const char* topicEvento        = "casa/quadrogeral/evento";         // NOVO: registro de ligar/desligar com a origem

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

unsigned long ultimaPublicacao = 0;
const unsigned long intervaloPublicacao = 10000;

//=====================================================
// Controle da carga
//=====================================================

// Publica o evento de acionamento (usado pela pagina de registros do Node-RED).
// origem: manual | dashboard | whatsapp | temporizador | protecao | botao_fisico
// detalhe: texto curto (ex.: causa do desligamento automatico)
void publicarEvento(const char* evento, const char* origem, const char* detalhe) {
    if (!mqttClient.connected()) return;
    char payload[200];
    snprintf(
        payload, sizeof(payload),
        "{\"evento\":\"%s\",\"origem\":\"%s\",\"detalhe\":\"%s\"}",
        evento, origem, detalhe
    );
    mqttClient.publish(topicEvento, payload);
}

void ligarCarga(const char* origem, const char* detalhe) {
    bool mudou = !cargaLigada;
    digitalWrite(RELE_PIN, RELE_NIVEL_LIGADO);
    digitalWrite(LED_VERDE_PIN, HIGH);
    digitalWrite(LED_VERMELHO_PIN, LOW);
    cargaLigada = true;
    contadorViolacao = 0;
    if (mqttClient.connected()) {
        mqttClient.publish(topicStatus, "ligada");
        if (mudou) publicarEvento("ligada", origem, detalhe);
    }
}

void desligarCarga(const char* origem, const char* detalhe) {
    bool mudou = cargaLigada;
    digitalWrite(RELE_PIN, RELE_NIVEL_DESLIGADO);
    digitalWrite(LED_VERDE_PIN, LOW);
    digitalWrite(LED_VERMELHO_PIN, HIGH);
    cargaLigada = false;
    if (mqttClient.connected()) {
        mqttClient.publish(topicStatus, "desligada");
        if (mudou) publicarEvento("desligada", origem, detalhe);
    }
}

// =====================================================
// OLED
// =====================================================
void oledBootScreen() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("ESP32 + PZEM + OLED");
    display.println("Inicializando...");
    display.display();
}

void oledStatus(const String& l1, const String& l2 = "", const String& l3 = "", const String& l4 = "") {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(l1);
    if (l2.length()) display.println(l2);
    if (l3.length()) display.println(l3);
    if (l4.length()) display.println(l4);
    display.display();
}

void oledReadings(float voltage, float current, float power, float energy,
                   float frequency, float pf, bool publishedOk, String proximaPublicacao) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(medidorEmFalha ? "FALHA NO MEDIDOR!" : "Quadro Geral");
    display.print("V: "); display.print(voltage, 1); display.println(" V");
    display.print("I: "); display.print(current, 2); display.println(" A");
    display.print("P: "); display.print(power, 1); display.println(" W");
    display.print("E: "); display.print(energy, 3); display.println(" kWh");
    display.print("Carga: "); display.println(cargaLigada ? "Ligada" : "Deslig.");
    display.print("Prox: "); display.println(proximaPublicacao);
    display.println(publishedOk ? "MQTT OK" : "MQTT ERR");
    display.display();
}

// =====================================================
// Script do portal (calculo de corrente em tempo real)
// =====================================================
const char* SCRIPT_VALIDACAO_CORRENTE = R"rawliteral(
<script>
window.addEventListener('load', function() {
    var input = document.getElementById('maxPower');
    if (!input) return;
    var msg = document.createElement('p');
    msg.style.fontWeight = 'bold';
    input.parentNode.insertBefore(msg, input.nextSibling);
    function atualizar() {
        var p = parseFloat(input.value);
        if (isNaN(p)) { msg.innerHTML = ''; return; }
        var i = p / 127.0;
        if (i > 20.0) {
            msg.style.color = 'red';
            msg.innerHTML = 'Corrente estimada: ' + i.toFixed(1) +
                ' A - acima do limite de 20 A (disjuntor). Avalie a potencia informada ou verifique a compatibilidade do equipamento.';
        } else {
            msg.style.color = 'green';
            msg.innerHTML = 'Corrente estimada: ' + i.toFixed(1) + ' A (dentro do limite de 20 A).';
        }
    }
    input.addEventListener('input', atualizar);
    atualizar();
});
</script>
)rawliteral";

// =====================================================
// WiFiManager - conexao + potencia inicial
// =====================================================
char maxPowerStr[8];

void setupWiFiManager() {
    prefs.begin("config", false);
    maxPower = prefs.getFloat("maxPower", 1500.0);
    prefs.end();

    bool configValida = false;

    while (!configValida) {
        dtostrf(maxPower, 1, 1, maxPowerStr);

        WiFiManagerParameter texto_tensao(
            "<p><b>Tensao de operacao:</b> 127V (padrao fixo)<br>"
            "Faixa de seguranca monitorada: 117V a 133V (conforme ANEEL)</p>"
        );
        WiFiManagerParameter custom_maxPower(
            "maxPower", "Potencia maxima do equipamento (W)", maxPowerStr, 8
        );

        WiFiManager wm;
        wm.setCustomHeadElement(SCRIPT_VALIDACAO_CORRENTE);
        wm.addParameter(&texto_tensao);
        wm.addParameter(&custom_maxPower);

        static WiFiManagerParameter* pParamPower;
        pParamPower = &custom_maxPower;

        wm.setSaveParamsCallback([&]() {
            maxPower = atof(pParamPower->getValue());
        });

        wm.setConfigPortalTimeout(180);

        oledStatus("Configuracao", "Conecte no WiFi:", "TomadaInteligente", "Config...");

        bool ok = wm.autoConnect("TomadaInteligente-Config");

        if (!ok) {
            Serial.println("Falha ao conectar. Reiniciando...");
            ESP.restart();
        }

        calcularLimites();

        if (maxCurrent > CORRENTE_MAX_SISTEMA) {
            oledStatus("Potencia invalida!", "Corrente > 20A", "Reconfigure...");
            delay(3000);
            WiFi.disconnect(true);
            configValida = false;
        } else {
            configValida = true;
        }
    }

    prefs.begin("config", false);
    prefs.putFloat("maxPower", maxPower);
    prefs.end();

    oledStatus("Wi-Fi conectado", WiFi.localIP().toString());
}

// =====================================================
// Botao de reset (a qualquer momento, nao bloqueante)
// =====================================================
void verificarBotaoReset() {
    if (digitalRead(BOTAO_RESET_PIN) == LOW) {
        if (!botaoPressionado) {
            botaoPressionado = true;
            inicioPressionamentoBotao = millis();
        } else if (millis() - inicioPressionamentoBotao >= TEMPO_SEGURAR_RESET) {
            desligarCarga("botao_fisico", "reset de configuracao");
            oledStatus("Resetando...", "WiFi e config", "serao apagados");
            delay(1000);

            WiFi.disconnect(true, true);
            WiFiManager wm;
            wm.resetSettings();

            prefs.begin("config", false);
            prefs.clear();
            prefs.end();

            oledStatus("Configuracoes", "apagadas!", "Reiniciando...");
            delay(1500);
            ESP.restart();
        }
    } else {
        botaoPressionado = false;
    }
}

// =====================================================
// MQTT
// =====================================================
void publicarLimites() {
    if (!mqttClient.connected()) return;
    char payload[220];
    snprintf(
        payload, sizeof(payload),
        "{\"tensaoNominal\":%.1f,\"tensaoMin\":%.1f,\"tensaoMax\":%.1f,"
        "\"maxPower\":%.1f,\"maxCurrent\":%.1f}",
        TENSAO_NOMINAL, TENSAO_MIN, TENSAO_MAX, maxPower, maxCurrent
    );
    mqttClient.publish(topicLimites, payload, true);
}

// NOVO: responde ao Node-RED se a config foi aceita ou recusada
void publicarStatusConfig(bool sucesso, String mensagem) {
    if (!mqttClient.connected()) return;
    char payload[300];
    snprintf(
        payload, sizeof(payload),
        "{\"sucesso\":%s,\"mensagem\":\"%s\",\"maxPower\":%.1f,\"maxCurrent\":%.1f}",
        sucesso ? "true" : "false", mensagem.c_str(), maxPower, maxCurrent
    );
    mqttClient.publish(topicConfigStatus, payload);
}

// NOVO: extrai um valor numerico simples de um JSON de uma camada
// (evita adicionar a biblioteca ArduinoJson so para isso)
float extrairValorJSON(String json, String chave) {
    int idx = json.indexOf("\"" + chave + "\"");
    if (idx == -1) return NAN;
    int doisPontos = json.indexOf(":", idx);
    if (doisPontos == -1) return NAN;
    int i = doisPontos + 1;
    while (i < (int)json.length() && json[i] == ' ') i++;
    int inicio = i;
    while (i < (int)json.length() && (isDigit(json[i]) || json[i] == '.' || json[i] == '-')) i++;
    String valorStr = json.substring(inicio, i);
    if (valorStr.length() == 0) return NAN;
    return valorStr.toFloat();
}

// NOVO: processa pedido de nova potencia vindo do Node-RED
void processarNovaConfiguracao(String mensagemJson) {
    Serial.println(">>> [1] Entrou em processarNovaConfiguracao <<<");
    Serial.print(">>> Estado MQTT no inicio: "); Serial.println(mqttClient.state());

    float novaPotencia = extrairValorJSON(mensagemJson, "maxPower");
    Serial.print(">>> [2] Valor extraido: "); Serial.println(novaPotencia);

    if (isnan(novaPotencia) || novaPotencia <= 0) {
        Serial.println("Config invalida recebida.");
        publicarStatusConfig(false, "Valor de potencia invalido.");
        return;
    }

    float novaCorrente = novaPotencia / TENSAO_NOMINAL;
    Serial.print(">>> [3] Corrente calculada: "); Serial.println(novaCorrente);

    if (novaCorrente > CORRENTE_MAX_SISTEMA) {
        publicarStatusConfig(false, "Corrente acima do limite do disjuntor (20 A).");
        return;
    }

    maxPower = novaPotencia;
    calcularLimites();
    Serial.println(">>> [4] Antes de gravar Preferences <<<");

    prefs.begin("config", false);
    prefs.putFloat("maxPower", maxPower);
    prefs.end();

    Serial.println(">>> [5] Preferences gravado <<<");
    Serial.print(">>> Estado MQTT apos gravar prefs: "); Serial.println(mqttClient.state());

    oledStatus("Config atualizada", String(maxPower, 0) + " W", String(maxCurrent, 1) + " A max");
    Serial.println(">>> [6] OLED atualizado <<<");

    Serial.println(">>> [7] Antes de publicarStatusConfig <<<");
    publicarStatusConfig(true, "Potencia atualizada com sucesso.");
    Serial.print(">>> Estado MQTT apos publicarStatusConfig: "); Serial.println(mqttClient.state());

    Serial.println(">>> [8] Antes de publicarLimites <<<");
    publicarLimites();
    Serial.print(">>> Estado MQTT apos publicarLimites: "); Serial.println(mqttClient.state());

    Serial.println(">>> [9] Fim de processarNovaConfiguracao <<<");
}

// Motivo do ultimo reinicio do ESP32 (vai para o registro de eventos)
const char* motivoReinicio() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "energia religada";
        case ESP_RST_SW:       return "reinicio por software";
        case ESP_RST_PANIC:    return "falha de software";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "queda de tensao (brownout)";
        default:               return "motivo desconhecido";
    }
}

bool primeiraConexaoMQTT = true;

void connectMQTT() {
    Serial.println(">>> Tentando conectar ao MQTT agora <<<");

    secureClient.stop(); // NOVO: garante que nao ha socket "preso" de uma tentativa anterior

    String clientId = "esp32-controle-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    unsigned long inicioTentativa = millis();
    bool conectou = mqttClient.connect(clientId.c_str(), mqttUser, mqttPassword);
    Serial.printf(">>> mqttClient.connect() retornou em %lu ms <<<\n", millis() - inicioTentativa);

    if (conectou) {
        mqttOK = true;
        mqttClient.subscribe(topicCommand);
        mqttClient.subscribe(topicConfigSet);
        mqttClient.publish(topicStatus, cargaLigada ? "ligada" : "desligada");
        publicarLimites();

        // Primeira conexao apos ligar = reinicio do sistema (carga volta desligada).
        // Conexoes seguintes = reconexao (o Node-RED usa para ressincronizar o temporizador).
        if (primeiraConexaoMQTT) {
            publicarEvento("reinicio", "sistema", motivoReinicio());
            primeiraConexaoMQTT = false;
        } else {
            publicarEvento("reconexao", "sistema", "mqtt");
        }
    } else {
        mqttOK = false;
        Serial.print("Falha MQTT. Estado: ");
        Serial.println(mqttClient.state());
    }
}

// Mantem apenas letras, numeros e "_" (evita quebrar o JSON do evento)
String sanitizarOrigem(String o) {
    o.trim();
    String r = "";
    for (unsigned int i = 0; i < o.length() && r.length() < 20; i++) {
        char c = o[i];
        if (isAlphaNumeric(c) || c == '_') r += c;
    }
    if (r.length() == 0) r = "manual";
    return r;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String mensagem = "";
    for (unsigned int i = 0; i < length; i++) mensagem += (char)payload[i];
    mensagem.trim();

    String topicStr = String(topic);

    // NOVO: mensagens de configuracao (JSON) sao tratadas separadamente,
    // sem passar por toLowerCase(), para nao corromper o JSON
   if (topicStr == topicConfigSet) {
    configRecebidaJson = mensagem;
    configPendente = true; // sinaliza para o loop() processar depois, fora do callback
    return;
}

    if (topicStr == topicCommand) {
        String comando = mensagem;
        comando.toLowerCase();

        // Formato aceito: "ligar" | "desligar" | "ligar|origem" | "desligar|origem"
        // (a origem e informada pelo Node-RED: dashboard, whatsapp, temporizador)
        String origem = "manual";
        int sep = comando.indexOf('|');
        if (sep >= 0) {
            origem = sanitizarOrigem(comando.substring(sep + 1));
            comando = comando.substring(0, sep);
            comando.trim();
        }

        if (comando == "ligar") {
            ligarCarga(origem.c_str(), "");
        } else if (comando == "desligar") {
            desligarCarga(origem.c_str(), "");
        } else {
            Serial.println("Comando desconhecido");
        }
    }
}

// =====================================================
// Leitura PZEM
// =====================================================
void lerPZEM() {
    voltage = pzem.voltage();
    current = pzem.current();
    power = pzem.power();
    energy = pzem.energy();
    frequency = pzem.frequency();
    pf = pzem.pf();

    // Deteccao de falha do medidor (varias leituras seguidas sem resposta)
    if (isnan(voltage)) falhasPzem++;
    else falhasPzem = 0;

    if (falhasPzem >= LIMITE_FALHAS_PZEM) medidorEmFalha = true;
    else if (falhasPzem == 0) medidorEmFalha = false;

    if (isnan(voltage)) voltage = 0;
    if (isnan(current)) current = 0;
    if (isnan(power)) power = 0;
    if (isnan(energy)) energy = 0;
    if (isnan(frequency)) frequency = 0;
    if (isnan(pf)) pf = 0;
}

// Avisa (alerta + registro de evento) quando o medidor para de responder e quando volta.
// Nao desliga a carga: apenas informa. Reenvia ate conseguir publicar (MQTT pode estar fora).
void notificarMedidor() {
    if (!mqttClient.connected()) return;

    if (medidorEmFalha && !falhaNotificada) {
        char alerta[160];
        snprintf(
            alerta, sizeof(alerta),
            "{\"device\":\"quadrogeral\",\"alerta\":\"falha_medidor\",\"carga\":\"%s\"}",
            cargaLigada ? "ligada" : "desligada"
        );
        mqttClient.publish(topicAlert, alerta);
        publicarEvento("falha_medidor", "sistema", "sem resposta do PZEM");
        falhaNotificada = true;
    } else if (!medidorEmFalha && falhaNotificada) {
        publicarEvento("medidor_ok", "sistema", "comunicacao restabelecida");
        falhaNotificada = false;
    }
}

// =====================================================
// Seguranca
// =====================================================
// A tensao fora da faixa (117 V a 133 V) NAO desliga mais a carga: ela continua
// sendo publicada na telemetria e aparece em "Leituras Recentes" no dashboard.
// O desligamento automatico ocorre apenas por:
//   - curto-circuito       -> imediato (corrente > CORRENTE_CURTO_CIRCUITO = 20 A)
//   - sobrecarga_corrente  -> corrente acima de maxCurrent (3 leituras seguidas)
//   - sobrecarga_potencia  -> potencia acima de maxPower   (3 leituras seguidas)
void verificarSeguranca() {
    if (!cargaLigada) {
        contadorViolacao = 0;
        return;
    }

    String causa = "";
    bool disparoImediato = false;

    if (current > CORRENTE_CURTO_CIRCUITO) {
        causa = "curto_circuito";
        disparoImediato = true;
    }
    else if (power > maxPower) causa = "sobrecarga_potencia";
    else if (current > maxCurrent) causa = "sobrecarga_corrente";

    if (causa != "") contadorViolacao++;
    else contadorViolacao = 0;

    if (disparoImediato || contadorViolacao >= LIMITE_VIOLACOES) {
        desligarCarga("protecao", causa.c_str());

        if (mqttClient.connected()) {
            char alerta[280];
            snprintf(
                alerta, sizeof(alerta),
                "{\"device\":\"quadrogeral\",\"alerta\":\"%s\","
                "\"voltage\":%.1f,\"current\":%.2f,\"power\":%.1f,"
                "\"tensaoMin\":%.1f,\"tensaoMax\":%.1f,"
                "\"maxPower\":%.1f,\"maxCurrent\":%.1f}",
                causa.c_str(), voltage, current, power,
                TENSAO_MIN, TENSAO_MAX, maxPower, maxCurrent
            );
            mqttClient.publish(topicAlert, alerta);
        }

        oledStatus("ALERTA!", causa, "Carga desligada", "Envie: ligar");
        delay(2000);
        contadorViolacao = 0;
    }
}

// =====================================================
// Publicacao MQTT (telemetria)
// =====================================================
void publicarMQTT() {
    if (!mqttClient.connected()) {
        mqttOK = false;
        return;
    }
    // Durante falha do medidor os valores seriam zeros falsos: nao polui o historico
    if (medidorEmFalha) return;
    char payload[320];
    snprintf(
        payload, sizeof(payload),
        "{\"device\":\"quadrogeral\",\"voltage\":%.1f,\"current\":%.2f,"
        "\"power\":%.1f,\"energy_kwh\":%.3f,\"frequency\":%.1f,\"power_factor\":%.2f}",
        voltage, current, power, energy, frequency, pf
    );
    mqttOK = mqttClient.publish(topicTelemetry, payload);
}

void verificarMQTT() {
    if (!mqttClient.connected()) {
        mqttOK = false;
        if (millis() - ultimaTentativaMQTT >= intervaloReconexaoMQTT) {
            ultimaTentativaMQTT = millis();
            connectMQTT();
        }
        return;
    }
    mqttOK = true;
    mqttClient.loop();
}

unsigned long ultimaTentativaWiFi = 0;
const unsigned long intervaloReconexaoWiFi = 10000;

void verificarWiFi() {
    if (WiFi.status() != WL_CONNECTED &&
        millis() - ultimaTentativaWiFi >= intervaloReconexaoWiFi) {
        ultimaTentativaWiFi = millis();
        WiFi.reconnect(); // usa as credenciais ja salvas pelo WiFiManager, sem reconfigurar
    }
}

// =====================================================
// Setup
// =====================================================
void setup() {
    Serial.begin(115200);

    digitalWrite(RELE_PIN, RELE_NIVEL_DESLIGADO);
    pinMode(RELE_PIN, OUTPUT);
    pinMode(LED_VERDE_PIN, OUTPUT);
    pinMode(LED_VERMELHO_PIN, OUTPUT);
    pinMode(BOTAO_RESET_PIN, INPUT_PULLUP);

    desligarCarga("boot", "");
    delay(500);

    Wire.begin(21, 22);

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        while (true) delay(100);
    }

    oledBootScreen();
    delay(1000);

    setupWiFiManager();
    Serial.println(">>> setupWiFiManager() concluido, seguindo para NTP <<<");

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    oledStatus("Sincronizando", "Horario NTP", "Aguarde...");

    struct tm timeinfo;
    while (!getLocalTime(&timeinfo)) delay(500);

    char horario[30];
    strftime(horario, sizeof(horario), "%d/%m/%Y %H:%M", &timeinfo);
    oledStatus("Horario OK", horario, "Sincronizado");
    delay(2000);

    secureClient.setInsecure();
    secureClient.setTimeout(10000);

    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setKeepAlive(120);
    mqttClient.setSocketTimeout(30);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);

    connectMQTT();
}

// =====================================================
// Loop
// =====================================================
void loop() {
    verificarBotaoReset();
    verificarWiFi();
    verificarMQTT();

    if (configPendente) {
    configPendente = false;
    processarNovaConfiguracao(configRecebidaJson);
}

    if (millis() - ultimaLeitura >= INTERVALO_LEITURA) {
        ultimaLeitura = millis();
        lerPZEM();
        verificarSeguranca();
        notificarMedidor();
    }

    if (millis() - ultimoOLED >= 1000) {
        ultimoOLED = millis();
        oledReadings(voltage, current, power, energy, frequency, pf, mqttOK, proxPub);
    }

    if (millis() - ultimaPublicacao >= intervaloPublicacao) {
        ultimaPublicacao = millis();
        publicarMQTT();

        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            time_t agora = mktime(&timeinfo) + (intervaloPublicacao / 1000);
            struct tm proximo = *localtime(&agora);
            char horarioPub[10];
            sprintf(horarioPub, "%02d:%02d:%02d", proximo.tm_hour, proximo.tm_min, proximo.tm_sec);
            proxPub = String(horarioPub);
        }
    }
}
