#include <WiFi.h>
#include <WebServer.h>
#include "EmonLib.h"




EnergyMonitor SCT013;




const char* ssid = "Luiz";
const char* password = "f845eeab";
const char* username = "admin";
const char* userpass = "1234";




const int pinSCT = 35;    // Pino ADC onde o sensor está conectado
#define BOTAO 14          // Pino do botão para alternar tensão




// Pinos dos LEDs
#define LED_220V 25
#define LED_110V 26
#define LED_OFF 27




// Variáveis para controle da tensão
int tensao = 110;         // Valor inicial (110V)
int estadoBotaoAnterior = HIGH;
int contadorTensao = 0;




// Preço do kWh em reais
const double preco_kWh = 0.80;




#define MAX_PONTOS 60




double Irms = 0;
double potencia = 0;
double energia_kWh = 0;




double historicoPotencia[MAX_PONTOS];
double historicoEnergia[MAX_PONTOS];
int indiceAtual = 0;
bool autenticado = false;




WebServer server(80);




unsigned long ultimoUpdate = 0;
unsigned long intervaloUpdate = 1000; // default 1 segundo
unsigned long tempoAnterior = 0;




// Variáveis adicionadas para controle do LED vermelho
unsigned long tempoDesligamentoVermelho = 0;
bool vermelhoDesligando = false;




void handleLogin();
void handleRoot();
void handleDados();
void handleReset();
void handleLogout();
void alternarTensao();
void handleSetTensao();
void atualizarLEDs();




void setup() {
  Serial.begin(115200);
  SCT013.current(pinSCT, 1.45); // Configura sensor SCT013 com offset manual 1.65V
  pinMode(BOTAO, INPUT_PULLUP); // Configura botão com pull-up interno
 
  // Configura os LEDs
  pinMode(LED_220V, OUTPUT);
  pinMode(LED_110V, OUTPUT);
  pinMode(LED_OFF, OUTPUT);
  atualizarLEDs(); // Inicializa os LEDs




  for (int i = 0; i < MAX_PONTOS; i++) {
    historicoPotencia[i] = 0;
    historicoEnergia[i] = 0;
  }




  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFalha ao conectar WiFi.");
  }




  server.on("/", handleLogin);




  server.on("/login", HTTP_POST, []() {
    if (server.arg("usuario") == username && server.arg("senha") == userpass) {
      autenticado = true;
      server.sendHeader("Location", "/painel");
      server.send(303);
    } else {
      server.send(401, "text/html", "<p>Usuário ou senha inválidos.</p><a href='/'>Voltar</a>");
    }
  });




  server.on("/painel", handleRoot);
  server.on("/dados", handleDados);
  server.on("/reset", handleReset);
  server.on("/logout", handleLogout);
  server.on("/setIntervalo", []() {
    if (!autenticado) {
      server.send(401, "text/plain", "Não autenticado");
      return;
    }
    if (server.hasArg("intervalo")) {
      int novoIntervalo = server.arg("intervalo").toInt();
      if (novoIntervalo == 1000 || novoIntervalo == 5000) {
        intervaloUpdate = novoIntervalo;
        server.send(200, "text/plain", "Intervalo alterado");
        return;
      }
    }
    server.send(400, "text/plain", "Intervalo inválido");
  });
 
  server.on("/setTensao", handleSetTensao);




  server.begin();
  tempoAnterior = millis();
}




void loop() {
  unsigned long agora = millis();




  // Verifica botão para alternar tensão
  alternarTensao();




  // Verifica se o LED vermelho está no processo de desligamento
  if (vermelhoDesligando && (agora - tempoDesligamentoVermelho >= 10000)) {
    digitalWrite(LED_OFF, LOW); // Desliga o LED
    vermelhoDesligando = false;
    Serial.println("LED vermelho desligado após 10 segundos para evitar sobrecarga");
  }




  if (agora - ultimoUpdate >= intervaloUpdate) {
    ultimoUpdate = agora;
    Irms = SCT013.calcIrms(4096);




    unsigned long tempoAtual = millis();
    unsigned long deltaTempo = tempoAtual - tempoAnterior;
    tempoAnterior = tempoAtual;




    if (Irms < 0.160) { // Ruído desconsiderado MANUAL
      Irms = 0;
      potencia = 0;
    } else {
      potencia = Irms * tensao;
      energia_kWh += (potencia * (deltaTempo / 3600000000.0)); // kWh (W * h)
      if (energia_kWh < 0) energia_kWh = 0; // Segurança
    }




    historicoPotencia[indiceAtual] = potencia;
    historicoEnergia[indiceAtual] = energia_kWh;
    indiceAtual = (indiceAtual + 1) % MAX_PONTOS;




    Serial.printf("Tensão: %dV | I=%.3f A | P=%.2f W | E=%.6f kWh\n", tensao, Irms, potencia, energia_kWh);
  }




  server.handleClient();
}




void alternarTensao() {
  int estadoBotaoAtual = digitalRead(BOTAO);




  // Detecta transição de HIGH para LOW (botão pressionado)
  if (estadoBotaoAnterior == HIGH && estadoBotaoAtual == LOW) {
    contadorTensao++;
    if (contadorTensao > 2) contadorTensao = 0;
   
    // Alterna os valores da variável 'tensao'
    switch (contadorTensao) {
      case 0: tensao = 110; break;
      case 1: tensao = 220; break;
      case 2: tensao = 0; break;  // Modo desligado
    }




    atualizarLEDs(); // Atualiza os LEDs conforme a tensão selecionada
   
    Serial.print("Botão pressionado. Nova tensão: ");
    Serial.print(tensao);
    Serial.println("V");




    // Se foi para o modo desligado (LED vermelho)
    if (tensao == 0) {
      vermelhoDesligando = true;
      tempoDesligamentoVermelho = millis();
      Serial.println("LED vermelho ficará aceso por 10 segundos");
    } else {
      vermelhoDesligando = false;
    }




    delay(300); // Debounce simples
  }




  estadoBotaoAnterior = estadoBotaoAtual;
}




void atualizarLEDs() {
  // Desliga todos os LEDs primeiro
  digitalWrite(LED_220V, LOW);
  digitalWrite(LED_110V, LOW);
  digitalWrite(LED_OFF, LOW);
 
  // Acende apenas o LED correspondente à tensão selecionada
  switch(tensao) {
    case 110:
      digitalWrite(LED_110V, HIGH);
      break;
    case 220:
      digitalWrite(LED_220V, HIGH);
      break;
    case 0:
      digitalWrite(LED_OFF, HIGH);
      break;
  }
}




void handleSetTensao() {
  if (!autenticado) {
    server.send(401, "text/plain", "Não autenticado");
    return;
  }
 
  if (server.hasArg("tensao")) {
    int novaTensao = server.arg("tensao").toInt();
    if (novaTensao == 0 || novaTensao == 110 || novaTensao == 220) {
      tensao = novaTensao;
      // Atualiza contador para manter sincronia com botão físico
      if (tensao == 110) contadorTensao = 0;
      else if (tensao == 220) contadorTensao = 1;
      else contadorTensao = 2;
     
      atualizarLEDs(); // Atualiza os LEDs conforme a nova tensão
     
      // Se foi para o modo desligado (LED vermelho)
      if (tensao == 0) {
        vermelhoDesligando = true;
        tempoDesligamentoVermelho = millis();
        Serial.println("LED vermelho ficará aceso por 10 segundos (via web)");
      } else {
        vermelhoDesligando = false;
      }
     
      server.send(200, "text/plain", "Tensão alterada");
      return;
    }
  }
  server.send(400, "text/plain", "Tensão inválida");
}




void handleLogin() {
  if (autenticado) {
    server.sendHeader("Location", "/painel");
    server.send(303);
    return;
  }
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Login - PowerKeeper</title>
<style>
body { font-family: sans-serif; background: #121212; color: #fff; display: flex; align-items: center; justify-content: center; height: 100vh; margin: 0; }
form { background: #1e1e1e; padding: 30px; border-radius: 10px; box-shadow: 0 0 10px #000; }
input { display: block; width: 100%; margin-bottom: 15px; padding: 10px; border: none; border-radius: 5px; font-size: 16px; }
button { width: 100%; padding: 10px; background: orange; border: none; font-size: 16px; border-radius: 5px; cursor: pointer; }
</style></head><body>
<form method="POST" action="/login">
<h2>Login</h2>
<input type="text" name="usuario" placeholder="Usuário" required>
<input type="password" name="senha" placeholder="Senha" required>
<button type="submit">Entrar</button>
</form></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}




void handleRoot() {
  if (!autenticado) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
 
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>PowerKeeper</title>
<style>
  body {
    font-family: Arial, sans-serif;
    margin: 0; padding: 0;
    background-color: #121212;
    color: #eee;
  }
  header {
    background: orange;
    color: black;
    padding: 15px;
    position: relative;
    text-align: center;
    font-weight: bold;
    font-size: 1.5em;
  }
  header a {
    position: absolute;
    left: 10px; top: 15px;
    color: #121212;
    font-weight: bold;
    text-decoration: none;
  }
  main {
    padding: 15px;
  }
  .dados {
    display: flex;
    justify-content: space-around;
    margin-bottom: 15px;
    flex-wrap: wrap;
  }
  .dados div {
    flex: 1 1 120px;
    margin: 10px;
    background: #222;
    padding: 10px;
    border-radius: 8px;
    text-align: center;
  }
  select, button {
    margin: 5px;
    padding: 6px 12px;
    border-radius: 6px;
    border: none;
    background-color: orange;
    color: black;
    font-weight: bold;
    cursor: pointer;
  }
  .tensao-container {
    display: flex;
    justify-content: center;
    margin: 15px 0;
  }
  .btn-tensao {
    padding: 10px 15px;
    margin: 0 5px;
    background: #333;
    color: white;
    border: none;
    border-radius: 5px;
    cursor: pointer;
    transition: all 0.3s;
  }
  .btn-tensao.active {
    background: orange;
    color: black;
    font-weight: bold;
    transform: scale(1.05);
  }
  .btn-tensao:hover {
    opacity: 0.9;
  }
  canvas {
    background: #333;
    border-radius: 10px;
    width: 100% !important;
    max-height: 300px;
    margin-top: 15px;
  }
  @media (max-width: 480px) {
    .dados div { width: 100%; margin-bottom: 15px; }
    .tensao-container { flex-direction: column; }
    .btn-tensao { margin: 5px 0; }
  }
</style>
</head>
<body>
<header>
PowerKeeper
<a href="/logout">Sair</a>
</header>
<main>
<div class="dados">
  <div><strong>Corrente (A)</strong><br><span id="corrente">0.00</span></div>
  <div><strong>Potência (W)</strong><br><span id="potencia">0.00</span></div>
  <div><strong>Consumo Total (kWh)</strong><br><span id="energia">0.000000</span></div>
  <div><strong>Custo (R$)</strong><br><span id="custo">0.00</span></div>
  <div><strong>Tensão (V)</strong><br><span id="tensao">0</span></div>
</div>




<div class="tensao-container">
  <button class="btn-tensao" data-tensao="110">110V</button>
  <button class="btn-tensao" data-tensao="220">220V</button>
  <button class="btn-tensao" data-tensao="0">Desligado</button>
</div>




<label for="intervalo">Intervalo Atualização:</label>
<select id="intervalo">
  <option value="1000">1 segundo</option>
  <option value="5000">5 segundos</option>
</select>
<button id="btnReset">Zerar Energia</button>




<canvas id="graficoPotencia"></canvas>
<canvas id="graficoEnergia"></canvas>




<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script>
  let correnteEl = document.getElementById("corrente");
  let potenciaEl = document.getElementById("potencia");
  let energiaEl = document.getElementById("energia");
  let custoEl = document.getElementById("custo");
  let tensaoEl = document.getElementById("tensao");
  let intervaloSel = document.getElementById("intervalo");
  let btnReset = document.getElementById("btnReset");
  let btnsTensao = document.querySelectorAll(".btn-tensao");




  // Chart.js setup
  const ctxPot = document.getElementById('graficoPotencia').getContext('2d');
  const ctxEner = document.getElementById('graficoEnergia').getContext('2d');




  const dadosPotencia = {
    labels: Array(60).fill(''),
    datasets: [{
      label: 'Potência (W)',
      data: Array(60).fill(0),
      borderColor: 'orange',
      backgroundColor: 'rgba(255,165,0,0.3)',
      fill: true,
      tension: 0.3,
    }]
  };




  const dadosEnergia = {
    labels: Array(60).fill(''),
    datasets: [{
      label: 'Energia (kWh)',
      data: Array(60).fill(0),
      borderColor: 'yellow',
      backgroundColor: 'rgba(255,255,0,0.3)',
      fill: true,
      tension: 0.3,
    }]
  };




  const configPot = {
    type: 'line',
    data: dadosPotencia,
    options: {
      animation: false,
      responsive: true,
      scales: {
        y: { beginAtZero: true }
      }
    }
  };




  const configEner = {
    type: 'line',
    data: dadosEnergia,
    options: {
      animation: false,
      responsive: true,
      scales: {
        y: { beginAtZero: true }
      }
    }
  };




  const chartPot = new Chart(ctxPot, configPot);
  const chartEner = new Chart(ctxEner, configEner);




  // Buscar dados do ESP a cada intervalo
  let intervaloAtual = 1000;
  intervaloSel.value = intervaloAtual;




  function atualizarDados() {
    fetch('/dados')
      .then(res => {
        if (!res.ok) throw new Error("Falha na resposta");
        return res.json();
      })
      .then(data => {
        correnteEl.textContent = data.Irms.toFixed(3);
        potenciaEl.textContent = data.potencia.toFixed(2);
        energiaEl.textContent = data.energia_kWh.toFixed(6);
        custoEl.textContent = data.custo.toFixed(2);
        tensaoEl.textContent = data.tensao;




        // Atualiza estado dos botões de tensão
        btnsTensao.forEach(btn => {
          btn.classList.remove('active');
          if (parseInt(btn.dataset.tensao) === data.tensao) {
            btn.classList.add('active');
          }
        });




        // Atualiza gráficos
        dadosPotencia.datasets[0].data.shift();
        dadosPotencia.datasets[0].data.push(data.potencia);
        dadosEnergia.datasets[0].data.shift();
        dadosEnergia.datasets[0].data.push(data.energia_kWh);




        chartPot.update();
        chartEner.update();
      })
      .catch(err => {
        console.error("Erro ao obter dados: ", err);
      });
  }




  let timer = setInterval(atualizarDados, intervaloAtual);




  // Configura botões de tensão
  btnsTensao.forEach(btn => {
    btn.addEventListener('click', () => {
      const tensaoSelecionada = parseInt(btn.dataset.tensao);
      fetch('/setTensao?tensao=' + tensaoSelecionada)
        .then(res => {
          if (res.ok) {
            // A atualização visual será feita no próximo fetch de dados
          } else {
            alert("Erro ao alterar tensão");
          }
        });
    });
  });




  intervaloSel.onchange = () => {
    let val = parseInt(intervaloSel.value);
    fetch('/setIntervalo?intervalo=' + val)
      .then(res => {
        if (res.ok) {
          clearInterval(timer);
          intervaloAtual = val;
          timer = setInterval(atualizarDados, intervaloAtual);
        } else {
          alert("Erro ao alterar intervalo");
        }
      });
  };




  btnReset.onclick = () => {
    if (confirm("Confirma zerar a energia acumulada?")) {
      fetch('/reset')
        .then(res => {
          if (res.ok) {
            energiaEl.textContent = "0.000000";
            custoEl.textContent = "0.00";
          }
        });
    }
  };




  atualizarDados(); // Atualiza ao carregar a página
</script>
</body>
</html>
)rawliteral";




  server.send(200, "text/html", html);
}




void handleDados() {
  if (!autenticado) {
    server.send(401, "application/json", "{\"erro\":\"Não autenticado\"}");
    return;
  }
  double custo = energia_kWh * preco_kWh;




  // Retorna os dados em JSON
  String json = "{";
  json += "\"Irms\":" + String(Irms, 3);
  json += ",\"potencia\":" + String(potencia, 2);
  json += ",\"energia_kWh\":" + String(energia_kWh, 6);
  json += ",\"custo\":" + String(custo, 2);
  json += ",\"tensao\":" + String(tensao);




  // Historico (ordenado do mais antigo para o mais recente)
  json += ",\"historicoPotencia\":[";
  for (int i = 0; i < MAX_PONTOS; i++) {
    int idx = (indiceAtual + i) % MAX_PONTOS;
    json += String(historicoPotencia[idx], 2);
    if (i < MAX_PONTOS - 1) json += ",";
  }
  json += "]";




  json += ",\"historicoEnergia\":[";
  for (int i = 0; i < MAX_PONTOS; i++) {
    int idx = (indiceAtual + i) % MAX_PONTOS;
    json += String(historicoEnergia[idx], 6);
    if (i < MAX_PONTOS - 1) json += ",";
  }
  json += "]";




  json += "}";




  server.send(200, "application/json", json);
}




void handleReset() {
  if (!autenticado) {
    server.send(401, "text/plain", "Não autenticado");
    return;
  }
  energia_kWh = 0;
  for (int i = 0; i < MAX_PONTOS; i++) {
    historicoEnergia[i] = 0;
  }
  server.send(200, "text/plain", "Energia zerada");
}




void handleLogout() {
  autenticado = false;
  server.sendHeader("Location", "/");
  server.send(303);
}

