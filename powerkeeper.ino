#include <WiFi.h>
#include "EmonLib.h"
#include <Firebase_ESP_Client.h>
#include <time.h>

// ---------- CONFIGURAÇÕES ----------
EnergyMonitor SCT013;

const char* ssid = "Augusto";
const char* password = "internet100";

const int idDispositivo = 1;
const int pinSCT = 35;
#define BOTAO 14

#define LED_220V 25
#define LED_127V 26
#define LED_OFF 27

// ---------- FIREBASE ----------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Seu host e auth, mantidos inalterados
#define FIREBASE_HOST "https://powerkeeper-synatec-default-rtdb.firebaseio.com/"
// CHAVE ATUALIZADA COM O VALOR FORNECIDO PELO USUÁRIO
#define FIREBASE_AUTH "gNcMVY25PGjzd1If4GX7OZiLENZsnxehj1JYmaRv"

// ---------- VARIÁVEIS ----------
int tensao = 127;
int estadoBotaoAnterior = HIGH;
int contadorTensao = 0; // Mantido o nome original para consistência

unsigned long intervaloLeitura = 1000;
unsigned long ultimoLeitura = 0;

unsigned long intervaloFirebase = 5000;
unsigned long ultimoFirebase = 0;

double Irms = 0.0;

unsigned long long ultimoTempoMicro = 0;

double energia_Wh = 0.0;
double consumoAtual_kWh = 0.0;
double consumoOntem_kWh = 0.0;

int diaAtual = -1;

unsigned long contadorLeitura = 1;

bool vermelhoDesligando = false; // Mantido o nome original para consistência
unsigned long tempoDesligamentoVermelho = 0; // Mantido o nome original para consistência

// Controle de fechamento (evita duplicação)
String ultimaDataFechada = "";

// ---------- PROTÓTIPOS ----------
String gerarTimestampISO();      // "YYYY-MM-DD HH:MM:SS"
String getDataHoje();           // "YYYY-MM-DD"
void conectarWiFi();
void alternarTensao();
void atualizarLEDs();
void atualizarEnergia();
void enviarDadosFirebase();
void verificarViradaDia();
void fechamentoDiario(double consumoDia_kWh);
void fechamentoRetroativoAoIniciar();
void esperarSincronizacaoNTP();

// ---------- IMPLEMENTAÇÕES ----------

// Retorna o timestamp no formato ISO "YYYY-MM-DD HH:MM:SS"
String gerarTimestampISO() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buffer[25];
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
            t->tm_year + 1900,
            t->tm_mon + 1,
            t->tm_mday,
            t->tm_hour,
            t->tm_min,
            t->tm_sec);
    return String(buffer);
}

// Retorna a data de hoje no formato "YYYY-MM-DD"
String getDataHoje() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buffer[12];
    sprintf(buffer, "%04d-%02d-%02d",
            t->tm_year + 1900,
            t->tm_mon + 1,
            t->tm_mday);
    return String(buffer);
}

// Função para esperar a sincronização do NTP (evita 1970)
void esperarSincronizacaoNTP() {
    Serial.print("Aguardando sincronizacao NTP");
    time_t now = time(nullptr);
    // Tempo maior que 2021 (1609459200) como indicador de sincronização.
    while (now < 1609459200) { 
        delay(500);
        now = time(nullptr);
        Serial.print(".");
    }
    Serial.println("\nNTP sincronizado com sucesso!");
}

// ---------- SETUP ----------
void setup() {
    Serial.begin(115200);
    delay(50);

    // iniciais
    SCT013.current(pinSCT, 1.45);   // fator calibração
    pinMode(BOTAO, INPUT_PULLUP);
    pinMode(LED_220V, OUTPUT);
    pinMode(LED_127V, OUTPUT);
    pinMode(LED_OFF, OUTPUT);
    atualizarLEDs();

    conectarWiFi();

    // NTP (fuso -3)
    configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
    if (WiFi.status() == WL_CONNECTED) {
        esperarSincronizacaoNTP(); // <--- CHAMA A FUNÇÃO DE ESPERA AQUI

        // registra dia atual APÓS a sincronização
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        diaAtual = t->tm_mday;
        Serial.printf("Data e hora de inicio: %s\n", gerarTimestampISO().c_str());

        // Refatoração: Centralizando a inicialização do Firebase aqui.
        Serial.println("Configurando Firebase...");
        config.database_url = FIREBASE_HOST;
        config.signer.tokens.legacy_token = FIREBASE_AUTH;
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(true);
        Serial.println("Firebase conectado!");

        // tenta fechar retroativamente caso necessário
        fechamentoRetroativoAoIniciar();
    } else {
        Serial.println("WiFi não conectado — inicializando sem Firebase (prototipo).");
        // Ainda tenta registrar o dia, mesmo que o NTP possa não ter funcionado
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        diaAtual = t->tm_mday;
    }
    
    ultimoTempoMicro = micros();
}

// ---------- LOOP ----------
void loop() {
    unsigned long agora = millis();

    alternarTensao();
    verificarViradaDia();

    // controle LED OFF temporizado (10s)
    if (vermelhoDesligando && (agora - tempoDesligamentoVermelho >= 10000UL)) {
        digitalWrite(LED_OFF, LOW);
        vermelhoDesligando = false;
    }

    // leitura a cada 1s
    if (agora - ultimoLeitura >= intervaloLeitura) {
        ultimoLeitura = agora;
        atualizarEnergia();
    }

    // envio ao Firebase a cada intervaloFirebase
    if (agora - ultimoFirebase >= intervaloFirebase) {
        ultimoFirebase = agora;
        if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
            enviarDadosFirebase();
        } else if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Tentando enviar, mas WiFi desconectado.");
        }
    }
}

// ---------- FUNÇÕES PRINCIPAIS ----------
void alternarTensao() {
    int estadoBotaoAtual = digitalRead(BOTAO);

    if (estadoBotaoAnterior == HIGH && estadoBotaoAtual == LOW) {
        contadorTensao++;
        if (contadorTensao > 2) contadorTensao = 0;

        switch (contadorTensao) {
            case 0: tensao = 127; break;
            case 1: tensao = 220; break;
            case 2: tensao = 0; break;
        }

        atualizarLEDs();

        if (tensao == 0) {
            vermelhoDesligando = true;
            tempoDesligamentoVermelho = millis();
            Serial.println("LED OFF - aceso por 10s");
        } else {
            vermelhoDesligando = false;
            digitalWrite(LED_OFF, LOW);
        }

        Serial.printf("Botao pressionado. Nova tensao: %d V\n", tensao);
        delay(250);     // debounce
    }

    estadoBotaoAnterior = estadoBotaoAtual;
}

void atualizarLEDs() {
    digitalWrite(LED_220V, LOW);
    digitalWrite(LED_127V, LOW);
    digitalWrite(LED_OFF, LOW);

    if (tensao == 127) digitalWrite(LED_127V, HIGH);
    else if (tensao == 220) digitalWrite(LED_220V, HIGH);
    else digitalWrite(LED_OFF, HIGH);
}

void atualizarEnergia() {
    // calcula Irms e energia com precisão por microssegundos
    unsigned long long agoraMicro = micros();
    unsigned long long deltaMicro = agoraMicro - ultimoTempoMicro;
    ultimoTempoMicro = agoraMicro;

    // Chamada principal para medição do SCT013
    Irms = SCT013.calcIrms(2048);
    
    // Se a corrente for muito baixa (ruído), zera o valor
    if (Irms < 0.16) Irms = 0.0;

    double potencia = Irms * tensao;    // W

    if (potencia > 0) {
        // Wh = W * horas ; deltaMicro / 3.600.000.000 => horas
        energia_Wh += (potencia * (deltaMicro / 3600000000.0));
        consumoAtual_kWh = energia_Wh / 1000.0;
    }

    Serial.printf("Tensao: %d V | Irms: %.3f A | P: %.2f W | Hoje: %.6f kWh\n",
                  tensao, Irms, potencia, consumoAtual_kWh);
}

// ---------- FIREBASE ----------
void conectarWiFi() {
    Serial.print("Conectando no WiFi ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000UL) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi conectado");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nFalha ao conectar (prototipo).");
    }
}

void enviarDadosFirebase() {
    // prepara JSON
    FirebaseJson json;
    double potenciaW = Irms * tensao;

    json.set("tensao", tensao);
    json.set("corrente", Irms);
    json.set("potencia", potenciaW); 
    json.set("consumoAtual_kWh", consumoAtual_kWh);
    json.set("idDispositivo", idDispositivo);
    json.set("timestamp", gerarTimestampISO());

    // Caminho ajustado para usar underscore: /leituras/leitura_1, leitura_2, etc.
    String pathLeitura = "/leituras/leitura_" + String(contadorLeitura++);

    Serial.print("Enviando leitura para ");
    Serial.println(pathLeitura);

    // Envia leitura para lista normal (Log)
    if (Firebase.RTDB.setJSON(&fbdo, pathLeitura.c_str(), &json)) {
        Serial.println("Leitura enviada com sucesso.");
    } else {
        Serial.println("Falha ao enviar leitura: " + fbdo.errorReason());
    }

    // Atualiza último valor (Snapshot)
    if (!Firebase.RTDB.setJSON(&fbdo, "/ultima_leitura", &json)) {
        Serial.println("Falha ao atualizar /ultima_leitura: " + fbdo.errorReason());
    }
}


// ---------- FECHAMENTO DIÁRIO SEGURO ----------
void verificarViradaDia() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);

    // Verifica se o dia do mês mudou
    if (t->tm_mday != diaAtual) {
        // grava consumo do dia anterior
        fechamentoDiario(consumoAtual_kWh);

        // reseta contadores locais
        consumoOntem_kWh = consumoAtual_kWh;
        consumoAtual_kWh = 0.0;
        energia_Wh = 0.0;

        // atualiza diaAtual
        diaAtual = t->tm_mday;

        Serial.println("----- NOVO DIA ----- Consumo zerado.");
    }
}

// Fechamento que evita duplicidade via /ultimo_fechamento/data
void fechamentoDiario(double consumoDia_kWh) {
    // calcula data de ontem (YYYY-MM-DD)
    time_t agora = time(nullptr);
    struct tm tm_ontem = *localtime(&agora);
    tm_ontem.tm_mday -= 1;
    mktime(&tm_ontem);

    char buffer[12];
    sprintf(buffer, "%04d-%02d-%02d",
            tm_ontem.tm_year + 1900,
            tm_ontem.tm_mon + 1,
            tm_ontem.tm_mday);

    String dataOntem = String(buffer);

    // pega ultimaDataFechada local se ainda não tiver sido lida
    if (ultimaDataFechada == "") {
        // tenta ler do Firebase /ultimo_fechamento/data (string simples)
        if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
            if (Firebase.RTDB.getString(&fbdo, "/ultimo_fechamento/data")) {
                ultimaDataFechada = fbdo.stringData();
                Serial.println("ultimo_fechamento/data lido: " + ultimaDataFechada);
            } else {
                Serial.println("Nenhum /ultimo_fechamento/data encontrado no Firebase.");
            }
        }
    }

    // evita duplicar
    if (dataOntem == ultimaDataFechada) {
        Serial.println("Fechamento diário já realizado para: " + dataOntem);
        return;
    }

    // cria JSON de fechamento
    FirebaseJson json;
    json.set("consumo_kWh", consumoDia_kWh);
    json.set("idDispositivo", idDispositivo);
    json.set("timestamp", gerarTimestampISO());

    String path = "/consumos_diarios/" + dataOntem;

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
            Serial.println("Fechamento diário enviado: " + path);
            ultimaDataFechada = dataOntem;
            // também atualiza um marcador simples para evitar duplicidade futura
            if (!Firebase.RTDB.setString(&fbdo, "/ultimo_fechamento/data", dataOntem)) {
                Serial.println("Aviso: não foi possível atualizar /ultimo_fechamento/data: " + fbdo.errorReason());
            }
        } else {
            Serial.println("Falha ao enviar fechamento diário: " + fbdo.errorReason());
        }
    } else {
        Serial.println("WiFi/Firebase indisponível: não foi possível enviar fechamento diário agora.");
    }
}

// Ao iniciar, fecha retroativamente se necessário usando /ultimo_fechamento/data
void fechamentoRetroativoAoIniciar() {
    Serial.println("Verificando fechamento retroativo ao iniciar...");

    if (!(WiFi.status() == WL_CONNECTED && Firebase.ready())) {
        Serial.println("Firebase não disponível no boot; pulando verificação retroativa.");
        return;
    }

    // ler string simples do Firebase com a última data fechada
    if (Firebase.RTDB.getString(&fbdo, "/ultimo_fechamento/data")) {
        ultimaDataFechada = fbdo.stringData();
        Serial.println("Ultima data fechada (firebase): " + ultimaDataFechada);
    } else {
        Serial.println("Nenhum /ultimo_fechamento/data encontrado (firebase).");
        ultimaDataFechada = "";
    }

    // data de ontem
    time_t agora = time(nullptr);
    struct tm tm_ontem = *localtime(&agora);
    tm_ontem.tm_mday -= 1;
    mktime(&tm_ontem);

    char buffer[12];
    sprintf(buffer, "%04d-%02d-%02d",
            tm_ontem.tm_year + 1900,
            tm_ontem.tm_mon + 1,
            tm_ontem.tm_mday);

    String dataOntem = String(buffer);

    if (dataOntem != ultimaDataFechada) {
        Serial.println("Fechamento retroativo necessário para: " + dataOntem);
        // usa o consumo atual armazenado localmente
        fechamentoDiario(consumoAtual_kWh);
    } else {
        Serial.println("Nenhum fechamento retroativo necessário.");
    }
}
