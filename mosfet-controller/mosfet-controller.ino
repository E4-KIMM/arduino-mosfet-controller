// Arduino Uno R4 WiFi — 웹(AP) 기반 MOSFET 모터 제어
//
// 보드가 WiFi 핫스팟을 만들고, 접속한 브라우저에서 http://192.168.4.1 로 모터를 제어한다.
// D3 → HW-548 MOSFET 모듈 신호 입력 (GND 공통 연결 필수, 모터 양단 플라이백 다이오드 권장)

#include <WiFiS3.h>
#include "arduino_secrets.h"

// ===== 설정 =====
const uint8_t MOTOR_PIN = 3;

// false: ON/OFF만 사용 (웹 슬라이더 비활성, ON = 100% 출력)
// true : ON 상태에서 PWM 듀티비(0~100%)로 속도 조절
const bool ENABLE_SPEED_CONTROL = false;

const uint8_t DEFAULT_SPEED_PCT = 100;

const unsigned long REQUEST_TIMEOUT_MS = 1000;

// ===== 상태 =====
bool motorOn = false;
uint8_t speedPct = DEFAULT_SPEED_PCT;

WiFiServer server(80);

const char PAGE_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Motor Control</title>
<style>
  body { font-family: sans-serif; margin: 0; padding: 24px 16px; background: #f4f4f5; color: #18181b; text-align: center; }
  h1 { font-size: 1.4rem; margin: 0 0 16px; }
  #state { font-size: 1.2rem; font-weight: bold; margin-bottom: 20px; }
  #state.on { color: #16a34a; }
  #state.off { color: #dc2626; }
  .buttons { display: flex; gap: 12px; justify-content: center; margin-bottom: 28px; }
  button { flex: 1; max-width: 160px; padding: 20px 0; font-size: 1.3rem; font-weight: bold; border: 0; border-radius: 12px; color: #fff; }
  #btnOn { background: #16a34a; }
  #btnOff { background: #dc2626; }
  .speed { background: #fff; border-radius: 12px; padding: 16px; max-width: 360px; margin: 0 auto; }
  .speed.disabled { opacity: 0.5; }
  input[type=range] { width: 100%; }
  #speedVal { font-size: 1.2rem; font-weight: bold; }
  #note { font-size: 0.9rem; color: #71717a; margin-top: 8px; }
</style>
</head>
<body>
<h1>Motor Control</h1>
<div id="state">-</div>
<div class="buttons">
  <button id="btnOn">ON</button>
  <button id="btnOff">OFF</button>
</div>
<div class="speed" id="speedBox">
  <div>속도 <span id="speedVal">-</span>%</div>
  <input type="range" id="speed" min="0" max="100" step="1">
  <div id="note"></div>
</div>
<script>
  const stateEl = document.getElementById('state');
  const speedEl = document.getElementById('speed');
  const speedValEl = document.getElementById('speedVal');
  const speedBox = document.getElementById('speedBox');
  const noteEl = document.getElementById('note');
  let dragging = false;

  function render(s) {
    stateEl.textContent = s.on ? 'ON' : 'OFF';
    stateEl.className = s.on ? 'on' : 'off';
    speedEl.disabled = !s.speedEnabled;
    speedBox.className = s.speedEnabled ? 'speed' : 'speed disabled';
    noteEl.textContent = s.speedEnabled ? '' : '속도 제어 비활성화됨 (ON = 100%)';
    if (!dragging) {
      speedEl.value = s.speed;
      speedValEl.textContent = s.speed;
    }
  }

  function call(path) {
    return fetch(path).then(r => r.json()).then(render).catch(() => {
      stateEl.textContent = '연결 끊김';
      stateEl.className = '';
    });
  }

  document.getElementById('btnOn').onclick = () => call('/on');
  document.getElementById('btnOff').onclick = () => call('/off');
  speedEl.oninput = () => { dragging = true; speedValEl.textContent = speedEl.value; };
  speedEl.onchange = () => { dragging = false; call('/speed?v=' + speedEl.value); };

  call('/status');
  setInterval(() => call('/status'), 2000);
</script>
</body>
</html>
)rawliteral";

// 모터 핀 출력은 이 함수에서만 갱신한다.
void applyOutput() {
  if (!motorOn) {
    digitalWrite(MOTOR_PIN, LOW);
  } else if (!ENABLE_SPEED_CONTROL || speedPct >= 100) {
    digitalWrite(MOTOR_PIN, HIGH);
  } else {
    analogWrite(MOTOR_PIN, map(speedPct, 0, 100, 0, 255));
  }

  Serial.print("Motor ");
  Serial.print(motorOn ? "ON" : "OFF");
  if (ENABLE_SPEED_CONTROL) {
    Serial.print(" (speed ");
    Serial.print(speedPct);
    Serial.print("%)");
  }
  Serial.println();
}

void sendStatus(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print("{\"on\":");
  client.print(motorOn ? "true" : "false");
  client.print(",\"speed\":");
  client.print(ENABLE_SPEED_CONTROL ? speedPct : 100);
  client.print(",\"speedEnabled\":");
  client.print(ENABLE_SPEED_CONTROL ? "true" : "false");
  client.println("}");
}

void sendPage(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(PAGE_HTML);
}

void sendNotFound(WiFiClient &client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println("Not Found");
}

// "GET /path?query HTTP/1.1" 에서 "/path?query" 부분을 꺼낸다.
String extractTarget(const String &requestLine) {
  int start = requestLine.indexOf(' ');
  if (start < 0) return "";
  int end = requestLine.indexOf(' ', start + 1);
  if (end < 0) end = requestLine.length();
  return requestLine.substring(start + 1, end);
}

void handleRequest(WiFiClient &client, const String &requestLine) {
  if (!requestLine.startsWith("GET ")) {
    sendNotFound(client);
    return;
  }

  String target = extractTarget(requestLine);
  int q = target.indexOf('?');
  String path = q >= 0 ? target.substring(0, q) : target;
  String query = q >= 0 ? target.substring(q + 1) : "";

  if (path == "/") {
    sendPage(client);
  } else if (path == "/on") {
    motorOn = true;
    applyOutput();
    sendStatus(client);
  } else if (path == "/off") {
    motorOn = false;
    applyOutput();
    sendStatus(client);
  } else if (path == "/speed") {
    int v = query.indexOf("v=");
    if (ENABLE_SPEED_CONTROL && v >= 0) {
      speedPct = constrain(query.substring(v + 2).toInt(), 0, 100);
      applyOutput();
    }
    sendStatus(client);
  } else if (path == "/status") {
    sendStatus(client);
  } else {
    sendNotFound(client);
  }
}

void serveClient(WiFiClient &client) {
  String requestLine = "";
  String currentLine = "";
  bool firstLineDone = false;
  unsigned long deadline = millis() + REQUEST_TIMEOUT_MS;

  // 요청 헤더를 빈 줄까지 읽는다. 첫 줄만 사용하고 나머지는 버린다.
  while (client.connected() && millis() < deadline) {
    if (!client.available()) continue;
    char c = client.read();
    if (c == '\r') continue;
    if (c != '\n') {
      currentLine += c;
      continue;
    }
    if (!firstLineDone) {
      requestLine = currentLine;
      firstLineDone = true;
    } else if (currentLine.length() == 0) {
      handleRequest(client, requestLine);
      break;
    }
    currentLine = "";
  }

  delay(1);
  client.stop();
}

void setup() {
  // 부팅 직후 모터가 돌지 않도록 가장 먼저 출력을 LOW로 고정한다.
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  Serial.begin(115200);
  delay(500);

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("WiFi module not found");
    while (true);
  }

  WiFi.config(IPAddress(192, 168, 4, 1));
  Serial.print("Starting access point: ");
  Serial.println(AP_SSID);

  if (WiFi.beginAP(AP_SSID, AP_PASS) != WL_AP_LISTENING) {
    Serial.println("Failed to start access point");
    while (true);
  }

  server.begin();

  Serial.print("Open http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  WiFiClient client = server.available();
  if (client) {
    serveClient(client);
  }
}
