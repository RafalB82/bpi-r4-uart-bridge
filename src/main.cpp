/*
 * BPI-R4 UART Bridge - ESP32 UART<->WiFi serial debug bridge
 *
 * First boot starts in AP mode (BPI-R4-Bridge, no password).
 * Connect, open http://192.168.4.1/, configure WiFi.
 * Config saves to flash.
 *
 * Connections:
 *   BPI-R4 (26pin)     ESP32
 *   Pin 8 (UART0 TX) -> GPIO16 (RX2)
 *   Pin 10 (UART0 RX)-> GPIO17 (TX2)
 *   Pin 9 (GND)      -> GND
 *
 * v1.1 - multi TCP clients, WiFi reconnect/events, no race on UART,
 *        proper restart after WiFi config change, NVS baud config in UI.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <vector>

#define FIRMWARE_VERSION "1.1"
#define TCP_PORT 8888
#define HTTP_PORT 80
#define WEBSOCKET_PORT 81
#define UART_BAUD_DEFAULT 115200
#define UART_RX_PIN 16
#define UART_TX_PIN 17
#define UART_BUF_SIZE 256
#define WIFI_TIMEOUT_MS 20000
#define MAX_TCP_CLIENTS 4

Preferences prefs;
WebServer server(HTTP_PORT);
WebSocketsServer webSocket(WEBSOCKET_PORT);
WiFiServer tcpServer(TCP_PORT);
std::vector<WiFiClient> tcpClients;

String wifi_ssid, wifi_pass;
unsigned long uart_baud = UART_BAUD_DEFAULT;
unsigned long wifiReconnectMs = 0;
bool wifiWasConnected = false;
bool pendingRestart = false;

// Queue for WebSocket -> UART data (avoids race in callback)
static RingbufHandle_t uart_tx_queue = NULL;

// ====== HTML ======
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BPI-R4 UART Bridge</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;font-family:sans-serif}
body{background:#1a1a2e;color:#eee;padding:20px;max-width:800px;margin:auto}
h1{color:#e94560}
.card{background:#16213e;border-radius:10px;padding:15px;margin:15px 0}
label{display:block;margin:8px 0 4px;color:#aaa}
input,select{width:100%;padding:8px;border:1px solid #0f3460;border-radius:5px;
  background:#1a1a2e;color:#eee;font-size:14px;box-sizing:border-box}
select option{background:#1a1a2e}
button{background:#e94560;color:#fff;border:none;padding:10px 20px;
  border-radius:5px;cursor:pointer;margin-top:10px}
button:hover{background:#c73650}
.success{color:#4ecca3;margin:10px 0}
#term{background:#000;color:#0f0;font-family:monospace;font-size:13px;
  padding:10px;height:300px;overflow-y:auto;border-radius:5px;margin-top:10px;
  white-space:pre-wrap;word-break:break-all}
.tabs{display:flex;gap:4px;margin-top:15px}
.tab{padding:8px 16px;border-radius:5px 5px 0 0;cursor:pointer;background:#0f3460;border:none;color:#eee}
.tab.active{background:#e94560}
.page{display:none}
.page.active{display:block}
</style>
</head><body>
<h1>BPI-R4 UART Bridge v""" FIRMWARE_VERSION R"""</h1>
<p>IP: <span id="ip">---</span> | TCP: <span id="tcp">port 8888</span></p>
<div class="tabs">
<button class="tab active" onclick="showPage('terminal')">Terminal</button>
<button class="tab" onclick="showPage('config')">Config</button>
</div>

<div id="page-terminal" class="page active">
<div class="card">
<div id="term">Waiting for serial data...<br></div>
<div style="display:flex;gap:5px;margin-top:5px">
<input id="cmd" placeholder="Type to send to BPI-R4">
<button onclick="sendCmd()">Send</button>
</div>
<button onclick="document.getElementById('term').innerHTML=''" style="background:#333;margin-top:5px">Clear</button>
</div>
</div>

<div id="page-config" class="page">
<div class="card">
<h3>WiFi Settings</h3>
<form id="wf">
<label>SSID</label><input id="ssid" name="ssid">
<label>Password</label><input id="pass" name="pass" type="password">
<button type="submit">Connect &amp; Reboot</button>
</form>
<div id="wifi-msg" class="success"></div>
</div>
<div class="card">
<h3>UART Baud Rate</h3>
<form id="bf">
<label>Baud</label>
<select id="baud" name="baud">
<option value="9600">9600</option>
<option value="19200">19200</option>
<option value="38400">38400</option>
<option value="57600">57600</option>
<option value="115200" selected>115200</option>
<option value="230400">230400</option>
<option value="460800">460800</option>
<option value="921600">921600</option>
</select>
<button type="submit">Set Baud &amp; Reboot</button>
</form>
<div id="baud-msg" class="success"></div>
</div>
</div>

<script>
var ws = new WebSocket('ws://'+location.hostname+':81/');
ws.onmessage = function(e) {
  var t = document.getElementById('term');
  t.innerHTML += e.data;
  t.scrollTop = t.scrollHeight;
};
function sendCmd() {
  var i = document.getElementById('cmd');
  if(i.value) { ws.send(i.value+'\r\n'); i.value=''; }
}
document.getElementById('cmd').addEventListener('keydown',function(e){
  if(e.key==='Enter') sendCmd();
});
function showPage(name) {
  document.querySelectorAll('.page').forEach(function(p){p.classList.remove('active')});
  document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active')});
  document.getElementById('page-'+name).classList.add('active');
  var tab = document.querySelector('.tab[onclick*="'+name+'"]');
  if(tab) tab.classList.add('active');
}
fetch('/status.json').then(function(r){return r.json()}).then(function(d){
  if(d.ssid) document.getElementById('ssid').value = d.ssid;
  var b = document.getElementById('baud');
  for(var i=0;i<b.options.length;i++) if(b.options[i].value==d.baud) b.selectedIndex=i;
});
document.getElementById('wf').addEventListener('submit', function(e){
  e.preventDefault();
  var d = new URLSearchParams(new FormData(this));
  fetch('/connect',{method:'POST',body:d}).then(function(r){return r.text()}).then(function(m){
    document.getElementById('wifi-msg').textContent = m;
  });
});
document.getElementById('bf').addEventListener('submit', function(e){
  e.preventDefault();
  var d = new URLSearchParams(new FormData(this));
  fetch('/baud',{method:'POST',body:d}).then(function(r){return r.text()}).then(function(m){
    document.getElementById('baud-msg').textContent = m;
  });
});
</script>
</body></html>
)rawliteral";

// ====== UART init ======
void initUart() {
  Serial2.setRxBufferSize(UART_BUF_SIZE * 4);
  Serial2.begin(uart_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.println("UART2 ready: " + String(uart_baud) + " baud");
}

// ====== AP mode ======
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BPI-R4-Bridge", NULL);
  Serial.println("AP started: BPI-R4-Bridge");
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
}

// ====== WiFi connect with timeout ======
bool connectWiFi(String ssid, String pass) {
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("Failed to connect");
  return false;
}

// ====== Web handlers ======
void handleRoot() { server.send_P(200, "text/html", index_html); }

void handleStatus() {
  String json = "{\"ssid\":\"" + wifi_ssid + "\",\"ip\":\"" +
    (WiFi.getMode() == WIFI_STA ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) +
    "\",\"mode\":\"" + String(WiFi.getMode() == WIFI_STA ? "STA" : "AP") +
    "\",\"baud\":\"" + String(uart_baud) + "\"}";
  server.send(200, "application/json", json);
}

void handleConnect() {
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID required");
    return;
  }

  prefs.begin("uart-bridge", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  server.send(200, "text/plain", "Saved. Rebooting...");
  delay(500);
  ESP.restart();
}

void handleBaud() {
  if (!server.hasArg("baud")) {
    server.send(400, "text/plain", "baud required");
    return;
  }
  unsigned long baud = server.arg("baud").toInt();
  if (baud < 1200) {
    server.send(400, "text/plain", "Invalid baud rate");
    return;
  }

  prefs.begin("uart-bridge", false);
  prefs.putUInt("baud", baud);
  prefs.end();

  server.send(200, "text/plain", "Baud saved. Rebooting...");
  delay(500);
  ESP.restart();
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ====== WebSocket event — queue data for loop, no direct UART write ======
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_TEXT && len > 0 && uart_tx_queue) {
    // Queue data; loop will drain it and write to Serial2
    xRingbufferSend(uart_tx_queue, payload, len, 0);
  }
}

// ====== Drain queued WebSocket data to UART ======
void flushUartTxQueue() {
  if (!uart_tx_queue) return;
  size_t item_size;
  while (true) {
    uint8_t* data = (uint8_t*)xRingbufferReceive(uart_tx_queue, &item_size, 0);
    if (!data) break;
    if (item_size > 0) {
      Serial2.write(data, item_size);
    }
    vRingbufferReturnItem(uart_tx_queue, (void*)data);
  }
}

// ====== Clean dead TCP clients ======
void pruneTcpClients() {
  for (int i = tcpClients.size() - 1; i >= 0; i--) {
    if (!tcpClients[i].connected()) {
      tcpClients[i].stop();
      tcpClients.erase(tcpClients.begin() + i);
    }
  }
}

// ====== Accept new TCP clients (up to MAX_TCP_CLIENTS) ======
void acceptTcpClients() {
  while (tcpClients.size() < MAX_TCP_CLIENTS) {
    WiFiClient c = tcpServer.available();
    if (!c) break;
    tcpClients.push_back(c);
    Serial.printf("TCP client #%d connected\n", tcpClients.size());
  }
}

// ====== Broadcast UART data to all WebSocket + TCP clients ======
void broadcastUart(const uint8_t* buf, size_t len) {
  webSocket.broadcastTXT((const char*)buf, len);
  for (auto& c : tcpClients) {
    if (c.connected()) c.write(buf, len);
  }
}

// ====== Read from all TCP clients and write to UART ======
void handleTcpToUart() {
  for (auto& c : tcpClients) {
    if (!c.connected()) continue;
    int available = c.available();
    if (available <= 0) continue;
    uint8_t buf[UART_BUF_SIZE];
    int len = c.read(buf, min(available, UART_BUF_SIZE));
    if (len > 0) {
      Serial2.write(buf, len);
    }
  }
}

// ====== WiFi event handler ======
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi disconnected, scheduling reconnect check");
      wifiReconnectMs = millis() + 5000;  // try reconnect in 5s
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiReconnectMs = 0;
      break;
    default:
      break;
  }
}

// ====== Try WiFi reconnect if we were connected before ======
void checkReconnect() {
  if (wifiReconnectMs == 0) return;
  if (millis() < wifiReconnectMs) return;
  wifiReconnectMs = 0;

  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("Attempting WiFi reconnect...");
  WiFi.reconnect();
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBPI-R4 UART Bridge v" FIRMWARE_VERSION);

  // Load config from flash
  prefs.begin("uart-bridge", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  uart_baud = prefs.getUInt("baud", UART_BAUD_DEFAULT);
  prefs.end();

  // Create UART TX queue
  uart_tx_queue = xRingbufferCreate(UART_BUF_SIZE * 8, RINGBUF_TYPE_BYTEBUF);
  if (!uart_tx_queue) {
    Serial.println("FATAL: Failed to create UART TX queue");
  }

  // Init UART
  initUart();

  // WiFi event handler
  WiFi.onEvent(WiFiEvent);

  // Connect or start AP
  if (wifi_ssid.length() > 0) {
    if (!connectWiFi(wifi_ssid, wifi_pass)) {
      startAP();
    }
  } else {
    startAP();
  }

  // Web server
  server.on("/", handleRoot);
  server.on("/status.json", handleStatus);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/baud", HTTP_POST, handleBaud);
  server.onNotFound(handleNotFound);
  server.begin();

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // TCP server
  tcpServer.begin();
  tcpServer.setNoDelay(true);
  Serial.printf("TCP bridge on port %d (max %d clients)\n", TCP_PORT, MAX_TCP_CLIENTS);

  wifiWasConnected = (WiFi.status() == WL_CONNECTED);
}

// ====== Loop ======
void loop() {
  server.handleClient();
  webSocket.loop();

  // Drain queued WebSocket data to UART (main loop context — no race)
  flushUartTxQueue();

  // TCP: prune dead, accept new, handle data
  pruneTcpClients();
  acceptTcpClients();
  handleTcpToUart();

  // UART -> WebSocket + TCP (read all available at once)
  size_t avail = Serial2.available();
  if (avail > 0) {
    // Use a stack buffer large enough
    uint8_t buf[UART_BUF_SIZE];
    size_t len = Serial2.readBytes(buf, min(avail, (size_t)UART_BUF_SIZE));
    if (len > 0) {
      broadcastUart(buf, len);
    }
  }

  // WiFi reconnect check
  checkReconnect();

  // Brief yield to watchdog/FreeRTOS
  delay(1);
}
