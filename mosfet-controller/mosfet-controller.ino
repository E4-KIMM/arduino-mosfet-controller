// Arduino Uno R4 WiFi — 웹(AP) 기반 모터 출력 제어 (L9110S)
//
// 보드가 WiFi 핫스팟을 만들고, 접속한 브라우저에서 http://192.168.4.1 로 모터 출력(0~100%)을 제어한다.
// 출력은 PWM duty로 조절한다 (50% ≈ 평균 6V @ 12V 전원).
//
// 배선 (L9110S 채널 A):
//   D3  → A-IA (PWM)
//   GND → A-IB (방향 고정)
//   12V 어댑터 (+) → VCC, (−) → GND  — 어댑터 GND와 Arduino GND 공통 연결 필수
//   Motor A 단자 → 모터

#include <WiFiS3.h>
#include "arduino_secrets.h"

// ===== 설정 =====
const uint8_t MOTOR_PIN = 3;

const unsigned long REQUEST_TIMEOUT_MS = 1000;

// ===== 상태 =====
uint8_t motorPercent = 0;

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
  #state { font-size: 2.4rem; font-weight: bold; margin-bottom: 20px; }
  #state.on { color: #16a34a; }
  #state.off { color: #dc2626; }
  .presets { display: flex; gap: 8px; justify-content: center; max-width: 480px; margin: 0 auto 20px; }
  .presets button { flex: 1; padding: 16px 0; font-size: 1.1rem; font-weight: bold; border: 2px solid #2563eb; border-radius: 12px; background: #fff; color: #2563eb; }
  .presets button.active { background: #2563eb; color: #fff; }
  .custom { display: flex; gap: 8px; justify-content: center; max-width: 480px; margin: 0 auto; }
  .custom input { flex: 1; min-width: 0; padding: 14px; font-size: 1.2rem; border: 1px solid #a1a1aa; border-radius: 12px; text-align: center; }
  .custom button { padding: 14px 20px; font-size: 1.1rem; font-weight: bold; border: 0; border-radius: 12px; background: #18181b; color: #fff; }
</style>
</head>
<body>
<h1>Motor Control</h1>
<div id="state">-</div>
<div class="presets" id="presets">
  <button data-p="0">0%</button>
  <button data-p="25">25%</button>
  <button data-p="50">50%</button>
  <button data-p="75">75%</button>
  <button data-p="100">100%</button>
</div>
<form class="custom" id="customForm">
  <input id="customInput" type="number" min="0" max="100" step="1" inputmode="numeric" placeholder="0~100">
  <button type="submit">적용</button>
</form>
<script>
  const stateEl = document.getElementById('state');
  const inputEl = document.getElementById('customInput');
  const presetBtns = document.querySelectorAll('#presets button');

  function render(s) {
    stateEl.textContent = s.percent + '%';
    stateEl.className = s.percent > 0 ? 'on' : 'off';
    presetBtns.forEach(b => b.classList.toggle('active', Number(b.dataset.p) === s.percent));
    // 입력 중인 값은 폴링으로 덮어쓰지 않는다.
    if (document.activeElement !== inputEl) inputEl.value = s.percent;
  }

  function call(path) {
    return fetch(path).then(r => r.json()).then(render).catch(() => {
      stateEl.textContent = '연결 끊김';
      stateEl.className = '';
    });
  }

  function setPercent(n) {
    n = Math.round(Math.min(100, Math.max(0, n)));
    return call('/set?p=' + n);
  }

  presetBtns.forEach(b => b.onclick = () => setPercent(Number(b.dataset.p)));

  document.getElementById('customForm').onsubmit = (e) => {
    e.preventDefault();
    const n = parseFloat(inputEl.value);
    if (isNaN(n)) return;
    inputEl.blur();
    setPercent(n);
  };

  call('/status');
  setInterval(() => call('/status'), 2000);
</script>
</body>
</html>
)rawliteral";

// 모터 핀 출력은 이 함수에서만 갱신한다.
void applyOutput() {
  analogWrite(MOTOR_PIN, map(motorPercent, 0, 100, 0, 255));

  Serial.print("Motor ");
  Serial.print(motorPercent);
  Serial.println("%");
}

void sendStatus(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print("{\"percent\":");
  client.print(motorPercent);
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

// "a=1&p=50" 같은 query 에서 key 의 값을 꺼낸다. 없으면 false.
bool getQueryParam(const String &query, const String &key, String &value) {
  String prefix = key + "=";
  int pos = 0;
  while (pos <= (int)query.length()) {
    int amp = query.indexOf('&', pos);
    if (amp < 0) amp = query.length();
    String pair = query.substring(pos, amp);
    if (pair.startsWith(prefix)) {
      value = pair.substring(prefix.length());
      return true;
    }
    pos = amp + 1;
  }
  return false;
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
  } else if (path == "/set") {
    String value;
    if (!getQueryParam(query, "p", value)) {
      sendNotFound(client);
      return;
    }
    motorPercent = constrain(value.toInt(), 0, 100);
    applyOutput();
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
