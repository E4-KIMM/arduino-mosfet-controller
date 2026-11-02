// Arduino Uno R4 WiFi — 웹(AP) 기반 모터 출력 제어 (L9110S)
//
// 보드가 WiFi 핫스팟을 만들고, 접속한 브라우저에서 http://192.168.4.1 로 모터 출력(0~100%)을 제어한다.
// 출력은 PWM duty로 조절한다 (50% ≈ 평균 6V @ 12V 전원).
// PWM 주파수(기본 150 Hz)와 최대 duty(기본 100%)는 웹에서 조절할 수 있다. 실측 결과 L9110S는 1 kHz 이상에서
// 스위칭을 못 따라가 모터가 돌지 않고, 100~200 Hz에서 가장 잘 돈다. 최대 duty 캡의 이유는 아래 주석 참고.
//
// 단계 프리셋 (SC3802PM-A 에어펌프 실측, 2026-11-02, 출력 낮은 순). 압력(Pa)은 설치 조건에 따라
// 달라지므로 기록하지 않는다. 모터/드라이버가 바뀌면 재튜닝 필요.
//   6단 : 100%            (상시 HIGH, 주파수 무관)
//   5단 :  99% /  500 Hz
//   4단 :  75% /   30 Hz
//   3단 :  50% /   30 Hz
//   2단 :  75% /  150 Hz
//   1단 :  50% /  150 Hz
//
// 배선 (L9110S 채널 A):
//   D3  → A-IA (PWM)
//   D4  → A-IB (실행 중 항상 LOW)
//   ※ A-IB를 GND에 고정하지 않는 이유: 모듈 입력에 VCC 풀업이 있어 부팅·전원 OFF 중 두 입력이
//     같은 레벨(H/H)이 되어야 모터가 정지한다. GND 고정 시 이 구간에 100%로 돈다.
//   12V 어댑터 (+) → VCC, (−) → GND  — 어댑터 GND와 Arduino GND 공통 연결 필수
//   Motor A 단자 → 모터

#include <WiFiS3.h>
#include "pwm.h"
#include "arduino_secrets.h"

// ===== 설정 =====
const uint8_t MOTOR_PIN = 3;  // A-IA, PWM
const uint8_t DIR_PIN = 4;    // A-IB, 실행 중 항상 LOW

// L9110S는 최대 PWM 주파수 스펙이 없다. 실측: 100~200 Hz 정상, 1 kHz 이상은 소리만 나고 모터가 안 돈다.
const uint32_t DEFAULT_PWM_FREQ_HZ = 150;
const uint32_t PWM_FREQ_MIN_HZ = 10;
const uint32_t PWM_FREQ_MAX_HZ = 30000;

// 100% duty는 스위칭 없는 상시 HIGH가 되어, 거기서 낮은 %로 바꾸면 모터가 멈췄다가 서서히 재시동된다.
// 최대 duty를 100% 미만으로 캡해 전 구간이 같은 주파수로 스위칭되게 한다. 100이면 캡 비활성.
// 단, OFF 구간이 너무 짧으면(수백 µs 미만) L9110S가 전환을 못 끝내 과전류 보호가 걸리므로 주파수와 함께 맞춘다.
const uint8_t DEFAULT_MAX_DUTY_PERCENT = 100;
const uint8_t MAX_DUTY_MIN_PERCENT = 50;

const unsigned long REQUEST_TIMEOUT_MS = 1000;

// ===== 상태 =====
uint8_t motorPercent = 0;
uint32_t pwmFreqHz = DEFAULT_PWM_FREQ_HZ;
uint8_t maxDutyPercent = DEFAULT_MAX_DUTY_PERCENT;

// MOTOR_PIN은 begin() 이후 GPT 타이머가 잡으므로 digitalWrite/analogWrite를 섞어 쓰지 않는다.
PwmOut motorPwm(MOTOR_PIN);

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
  #freq { font-size: 0.95rem; color: #52525b; margin: -12px 0 20px; }
  h2 { font-size: 1rem; margin: 28px 0 4px; }
  .hint { font-size: 0.8rem; color: #71717a; margin: 0 0 10px; }
  .presets { display: flex; flex-wrap: wrap; gap: 8px; justify-content: center; max-width: 480px; margin: 0 auto 20px; }
  .presets button { flex: 1; padding: 16px 0; font-size: 1.1rem; font-weight: bold; border: 2px solid #2563eb; border-radius: 12px; background: #fff; color: #2563eb; }
  .presets button.active { background: #2563eb; color: #fff; }
  .presets.freq button { flex: 1 1 20%; padding: 12px 0; font-size: 1rem; }
  .presets.pa button { flex: 1 1 28%; padding: 10px 0; line-height: 1.3; }
  .presets.pa small { display: block; font-size: 0.75rem; font-weight: normal; opacity: 0.8; }
  .custom { display: flex; gap: 8px; justify-content: center; max-width: 480px; margin: 0 auto; }
  .custom input { flex: 1; min-width: 0; padding: 14px; font-size: 1.2rem; border: 1px solid #a1a1aa; border-radius: 12px; text-align: center; }
  .custom button { padding: 14px 20px; font-size: 1.1rem; font-weight: bold; border: 0; border-radius: 12px; background: #18181b; color: #fff; }
</style>
</head>
<body>
<h1>Motor Control</h1>
<div id="state">-</div>
<div id="freq">-</div>
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

<h2>단계 프리셋</h2>
<p class="hint">출력과 주파수를 함께 적용 (실측 튜닝값)</p>
<div class="presets pa" id="paPresets"></div>

<h2>PWM 주파수</h2>
<p class="hint">낮을수록 100% 근처가 안정되지만 맥동이 커짐. 1 kHz 이상은 안 돎</p>
<div class="presets freq" id="freqPresets">
  <button data-hz="20">20</button>
  <button data-hz="30">30</button>
  <button data-hz="50">50</button>
  <button data-hz="75">75</button>
  <button data-hz="100">100</button>
  <button data-hz="150">150</button>
</div>
<form class="custom" id="freqForm">
  <input id="freqInput" type="number" min="10" max="30000" step="1" inputmode="numeric" placeholder="10~30000 Hz">
  <button type="submit">적용</button>
</form>

<h2>최대 duty (%)</h2>
<p class="hint">100이면 캡 없음(100% = 상시 HIGH). OFF 구간이 짧아지면 끊김이 생길 수 있음</p>
<div class="presets freq" id="capPresets">
  <button data-cap="90">90</button>
  <button data-cap="95">95</button>
  <button data-cap="98">98</button>
  <button data-cap="100">100</button>
</div>
<form class="custom" id="capForm">
  <input id="capInput" type="number" min="50" max="100" step="1" inputmode="numeric" placeholder="50~100">
  <button type="submit">적용</button>
</form>
<script>
  const stateEl = document.getElementById('state');
  const freqEl = document.getElementById('freq');
  const inputEl = document.getElementById('customInput');
  const freqInputEl = document.getElementById('freqInput');
  const capInputEl = document.getElementById('capInput');
  const presetBtns = document.querySelectorAll('#presets button');
  const freqBtns = document.querySelectorAll('#freqPresets button');
  const capBtns = document.querySelectorAll('#capPresets button');

  // 실측 단계 프리셋 (출력 낮은 순). hz 가 없으면 주파수는 바꾸지 않는다 (100% = 상시 HIGH라 무관).
  const PA_PRESETS = [
    { p: 50,  hz: 150 },
    { p: 75,  hz: 150 },
    { p: 50,  hz: 30 },
    { p: 75,  hz: 30 },
    { p: 99,  hz: 500 },
    { p: 100 },
  ];
  const paBox = document.getElementById('paPresets');
  const paBtns = PA_PRESETS.map((pr, i) => {
    const b = document.createElement('button');
    b.innerHTML = (i + 1) + '단<small>' + pr.p + '%' + (pr.hz ? ' · ' + pr.hz + ' Hz' : '') + '</small>';
    b.onclick = () => call('/set?p=' + pr.p + '&cap=100' + (pr.hz ? '&hz=' + pr.hz : ''));
    paBox.appendChild(b);
    return b;
  });

  function render(s) {
    stateEl.textContent = s.percent + '%';
    stateEl.className = s.percent > 0 ? 'on' : 'off';
    freqEl.textContent = 'PWM ' + s.freq + ' Hz · 최대 ' + s.cap + '%';
    presetBtns.forEach(b => b.classList.toggle('active', Number(b.dataset.p) === s.percent));
    freqBtns.forEach(b => b.classList.toggle('active', Number(b.dataset.hz) === s.freq));
    capBtns.forEach(b => b.classList.toggle('active', Number(b.dataset.cap) === s.cap));
    paBtns.forEach((b, i) => {
      const pr = PA_PRESETS[i];
      b.classList.toggle('active', pr.p === s.percent && (!pr.hz || pr.hz === s.freq));
    });
    // 입력 중인 값은 폴링으로 덮어쓰지 않는다.
    if (document.activeElement !== inputEl) inputEl.value = s.percent;
    if (document.activeElement !== freqInputEl) freqInputEl.value = s.freq;
    if (document.activeElement !== capInputEl) capInputEl.value = s.cap;
  }

  // 숫자 입력 폼을 submit 핸들러로 묶는다.
  function bindForm(formId, input, apply) {
    document.getElementById(formId).onsubmit = (e) => {
      e.preventDefault();
      const n = parseFloat(input.value);
      if (isNaN(n)) return;
      input.blur();
      apply(n);
    };
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

  function setFreq(hz) {
    hz = Math.round(Math.min(30000, Math.max(10, hz)));
    return call('/freq?hz=' + hz);
  }

  function setCap(p) {
    p = Math.round(Math.min(100, Math.max(50, p)));
    return call('/cap?p=' + p);
  }

  presetBtns.forEach(b => b.onclick = () => setPercent(Number(b.dataset.p)));
  freqBtns.forEach(b => b.onclick = () => setFreq(Number(b.dataset.hz)));
  capBtns.forEach(b => b.onclick = () => setCap(Number(b.dataset.cap)));

  bindForm('customForm', inputEl, setPercent);
  bindForm('freqForm', freqInputEl, setFreq);
  bindForm('capForm', capInputEl, setCap);

  call('/status');
  setInterval(() => call('/status'), 2000);
</script>
</body>
</html>
)rawliteral";

// 모터 핀 출력은 이 함수에서만 갱신한다.
void applyOutput() {
  motorPwm.pulse_perc(min(motorPercent, maxDutyPercent));

  Serial.print("Motor ");
  Serial.print(motorPercent);
  Serial.println("%");
}

// get_freq_hz()는 타이머 클럭을 돌려주므로 주기 카운트로 나눠야 실제 PWM 주파수가 된다.
void printPwmFreq() {
  FspTimer *t = motorPwm.get_timer();
  Serial.print("PWM ");
  Serial.print(t->get_freq_hz() / t->get_period_raw());
  Serial.println(" Hz");
}

// 주파수를 바꾸면 타이머가 닫혔다 다시 열리므로 duty를 반드시 다시 적용한다.
void applyFreq() {
  motorPwm.get_timer()->set_frequency(pwmFreqHz);
  applyOutput();
  printPwmFreq();
}

void sendStatus(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print("{\"percent\":");
  client.print(motorPercent);
  client.print(",\"freq\":");
  client.print(pwmFreqHz);
  client.print(",\"cap\":");
  client.print(maxDutyPercent);
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
    // p 는 필수, hz / cap 은 선택. 프리셋처럼 셋을 한 요청으로 함께 바꿀 수 있다.
    String value;
    if (!getQueryParam(query, "p", value)) {
      sendNotFound(client);
      return;
    }
    motorPercent = constrain(value.toInt(), 0, 100);
    if (getQueryParam(query, "cap", value)) {
      maxDutyPercent = constrain(value.toInt(), (long)MAX_DUTY_MIN_PERCENT, 100L);
    }
    if (getQueryParam(query, "hz", value)) {
      pwmFreqHz = constrain(value.toInt(), (long)PWM_FREQ_MIN_HZ, (long)PWM_FREQ_MAX_HZ);
      applyFreq();
    } else {
      applyOutput();
    }
    sendStatus(client);
  } else if (path == "/freq") {
    String value;
    if (!getQueryParam(query, "hz", value)) {
      sendNotFound(client);
      return;
    }
    pwmFreqHz = constrain(value.toInt(), (long)PWM_FREQ_MIN_HZ, (long)PWM_FREQ_MAX_HZ);
    applyFreq();
    sendStatus(client);
  } else if (path == "/cap") {
    String value;
    if (!getQueryParam(query, "p", value)) {
      sendNotFound(client);
      return;
    }
    maxDutyPercent = constrain(value.toInt(), (long)MAX_DUTY_MIN_PERCENT, 100L);
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
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(DIR_PIN, LOW);

  Serial.begin(115200);
  delay(500);

  // duty 0%로 PWM 타이머를 시작한다 (핀이 GPIO에서 GPT 출력으로 전환됨).
  if (!motorPwm.begin((float)pwmFreqHz, 0.0f)) {
    Serial.println("PWM init failed");
    while (true);
  }
  printPwmFreq();

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
