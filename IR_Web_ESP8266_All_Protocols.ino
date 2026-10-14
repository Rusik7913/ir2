/*
  IR Web Remote for ESP8266 — decoded + raw replay
  Library: IRremoteESP8266 by crankyoldgit
  Board: NodeMCU 1.0 / Wemos D1 mini

  Raw capture/replay can reproduce many protocols that don't have a dedicated
  send function, but no single receiver/transmitter can guarantee every IR system.
  Receiver wiring must be identified first. Typical receiver: VCC->3V3, GND->GND,
  OUT->D5 (GPIO14), only if the specific receiver supports 3.3 V.
  IR LED driver input -> D2 (GPIO4). Use a transistor driver/current-limiting
  resistor for the IR LED; don't drive a bare LED directly from a GPIO.
*/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";

const uint16_t RECV_PIN = D5;
const uint16_t SEND_PIN = D2;
const uint16_t CAPTURE_BUFFER_SIZE = 2048;
const uint8_t TIMEOUT = 50;
const uint16_t RAW_MAX = 600;

ESP8266WebServer server(80);
IRrecv irrecv(RECV_PIN, CAPTURE_BUFFER_SIZE, TIMEOUT, true);
IRsend irsend(SEND_PIN);
decode_results results;

String lastCode = "";
String lastProtocol = "";
uint16_t lastBits = 0;
uint16_t rawData[RAW_MAX];
uint16_t rawCount = 0;
bool hasCode = false;
bool receiving = false;
unsigned long receiveUntil = 0;
String logText = "Ready.";

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IR Remote ESP8266</title>
<style>
body{font-family:system-ui,sans-serif;background:#10131b;color:#f3f5ff;margin:0;padding:22px}
main{max-width:600px;margin:auto}h1{font-size:26px}p{color:#b8c0d8}
button{width:100%;padding:17px;margin:8px 0;border:0;border-radius:14px;font-size:18px;font-weight:700}
#receive{background:#3b82f6;color:white}#send{background:#22c55e;color:#07140b}
.card{background:#1b2030;border:1px solid #30384d;border-radius:14px;padding:15px;margin-top:14px}
code{overflow-wrap:anywhere;color:#a5f3fc}a{color:#93c5fd}small{color:#aeb7ce}
</style></head><body><main>
<h1>Универсальный ИК-пульт</h1>
<p>Приём декодированных команд и сохранение временных интервалов для повторной передачи разных протоколов.</p>
<button id="receive" onclick="receiveIR()">1 · ПРИНЯТЬ ИК-СИГНАЛ</button>
<button id="send" onclick="sendIR()">2 · ОТПРАВИТЬ ПОСЛЕДНИЙ СИГНАЛ</button>
<div class="card"><b>Статус</b><p id="status">Подключение…</p><b>Протокол</b><p id="proto">—</p><b>Код</b><p><code id="code">нет</code></p><small id="meta"></small><p><a href="/download">Скачать файл с кодом и raw-данными</a></p></div>
<script>
async function refresh(){try{let r=await fetch('/status');let j=await r.json();status.textContent=j.status;code.textContent=j.code||'нет';proto.textContent=j.protocol||'не определён';meta.textContent='Биты: '+j.bits+' · raw-импульсов: '+j.rawCount;}catch(e){status.textContent='Нет связи с ESP8266';}}
async function receiveIR(){status.textContent='Нажми кнопку пульта и удерживай/повтори её в течение 20 секунд…';await fetch('/receive');let n=0;let t=setInterval(async()=>{await refresh();n++;if(n>=22)clearInterval(t)},1000)}
async function sendIR(){let r=await fetch('/send',{method:'POST'});let j=await r.json();status.textContent=j.status;refresh();}
const status=document.getElementById('status'),code=document.getElementById('code'),proto=document.getElementById('proto'),meta=document.getElementById('meta');
refresh();setInterval(refresh,2000);
</script></main></body></html>
)HTML";

String jsonEscape(String s) {
  s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
  s.replace("\n", "\\n"); s.replace("\r", "");
  return s;
}

void saveCode() {
  File f = LittleFS.open("/last_ir.txt", "w");
  if (!f) { logText = "Не удалось сохранить файл"; return; }
  f.println("protocol=" + lastProtocol);
  f.println("bits=" + String(lastBits));
  f.println("code=" + lastCode);
  f.println("raw_count=" + String(rawCount));
  f.println("raw_microseconds=");
  for (uint16_t i = 0; i < rawCount; i++) {
    f.print(rawData[i]);
    if (i + 1 < rawCount) f.print(',');
  }
  f.println();
  f.close();
}

void handleStatus() {
  String statusText = logText;
  if (receiving && millis() < receiveUntil) statusText = "Ожидаю сигнал от пульта…";
  String j = "{\"status\":\"" + jsonEscape(statusText) + "\",\"code\":\"" +
    jsonEscape(lastCode) + "\",\"protocol\":\"" + jsonEscape(lastProtocol) +
    "\",\"bits\":" + String(lastBits) + ",\"rawCount\":" + String(rawCount) + "}";
  server.send(200, "application/json; charset=utf-8", j);
}
void handleReceive() {
  receiving = true;
  receiveUntil = millis() + 20000;
  logText = "Ожидаю ИК-сигнал 20 секунд";
  server.send(200, "text/plain; charset=utf-8", "OK");
}
void handleSend() {
  if (!hasCode || rawCount < 2) {
    server.send(400, "application/json; charset=utf-8", "{\"status\":\"Сначала прими сигнал\"}");
    return;
  }
  // Raw replay is the fallback for protocols without a dedicated sender.
  // 38 kHz is common; some devices use 36, 40, or 56 kHz and may need adjustment.
  irsend.sendRaw(rawData, rawCount, 38);
  logText = "Отправлена raw-запись на несущей 38 кГц";
  server.send(200, "application/json; charset=utf-8",
    "{\"status\":\"Raw-сигнал отправлен (38 кГц). Если устройство не реагирует, может потребоваться другая частота/повтор записи.\"}");
}
void handleDownload() {
  if (!LittleFS.exists("/last_ir.txt")) {
    server.send(404, "text/plain; charset=utf-8", "Пока нет сохранённого кода. Сначала нажми «Принять».");
    return;
  }
  File f = LittleFS.open("/last_ir.txt", "r");
  server.sendHeader("Content-Disposition", "attachment; filename=last_ir.txt");
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}
void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  irrecv.enableIRIn();
  irsend.begin();

  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("IR-ESP8266", "irremote8266");
    logText = "Подключись к Wi-Fi IR-ESP8266, пароль irremote8266. Адрес: 192.168.4.1";
  } else {
    logText = "Открой в браузере: http://" + WiFi.localIP().toString();
  }

  server.on("/", HTTP_GET, [](){ server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/receive", HTTP_GET, handleReceive);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/download", HTTP_GET, handleDownload);
  server.begin();
  Serial.println(logText);
}
void loop() {
  server.handleClient();
  if (receiving) {
    if (irrecv.decode(&results)) {
      lastProtocol = typeToString(results.decode_type);
      lastBits = results.bits;
      lastCode = uint64ToString(results.value, 16);
      rawCount = 0;
      // rawbuf[0] is the leading gap; convert ticks to microseconds and skip it.
      for (uint16_t i = 1; i < results.rawlen && rawCount < RAW_MAX; i++) {
        uint32_t us = (uint32_t)results.rawbuf[i] * kRawTick;
        if (us > 65535) us = 65535;
        rawData[rawCount++] = (uint16_t)us;
      }
      hasCode = rawCount >= 2;
      saveCode();
      logText = "Сигнал принят и сохранён (декодированный + raw)";
      receiving = false;
      irrecv.resume();
    } else if (millis() >= receiveUntil) {
      receiving = false;
      logText = "Время ожидания истекло. Попробуй ещё раз и нажимай кнопку пульта несколько раз.";
    }
  } else if (irrecv.decode(&results)) {
    irrecv.resume();
  }
}
