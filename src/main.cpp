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
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>

#define FIRMWARE_VERSION "1.0"
#define TCP_PORT 8888
#define HTTP_PORT 80
#define WEBSOCKET_PORT 81
#define UART_BAUD_DEFAULT 115200
#define UART_RX_PIN 16
#define UART_TX_PIN 17

Preferences prefs;
WebServer server(HTTP_PORT);
WebSocketsServer webSocket(WEBSOCKET_PORT);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;

String wifi_ssid, wifi_pass;
unsigned long uart_baud = UART_BAUD_DEFAULT;

bool shouldSaveConfig = false;

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
input{width:100%;padding:8px;border:1px solid #0f3460;border-radius:5px;
  background:#1a1a2e;color:#eee;font-size:14px;box-sizing:border-box}
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
<h1>BPI-R4 UART Bridge</h1>
<p>IP: <span id="ip">---</span> | TCP: <span id="tcp">port 8888</span></p>
<div class="tabs">
<button class="tab active" onclick="showPage('terminal')">Terminal</button>
<button class="tab" onclick="showPage('config')">WiFi Config</button>
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
<button type="submit">Connect</button>
</form>
<div id="wifi-msg" class="success"></div>
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
});
document.getElementById('wf').addEventListener('submit', function(e){
  e.preventDefault();
  var d = new URLSearchParams(new FormData(this));
  fetch('/connect',{method:'POST',body:d}).then(function(r){return r.text()}).then(function(m){
    document.getElementById('wifi-msg').textContent = m;
  });
});
</script>
</body></html>
)rawliteral";

// Initialize UART2 for BPI-R4
void initUart() {
  Serial2.begin(uart_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.println("UART2 ready: " + String(uart_baud) + " baud");
}

// Start AP mode (first boot or WiFi config mode)
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BPI-R4-Bridge", NULL);
  Serial.println("AP started: BPI-R4-Bridge");
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
}

// Try to connect to WiFi, return true on success
bool connectWiFi(String ssid, String pass) {
  if (ssid.length() == 0) return false;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  
  Serial.print("Connecting to WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
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
    "\",\"mode\":\"" + String(WiFi.getMode() == WIFI_STA ? "STA" : "AP") + "\"}";
  server.send(200, "application/json", json);
}

void handleConnect() {
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID required");
    return;
  }
  
  // Save to preferences
  prefs.begin("uart-bridge", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  
  server.send(200, "text/plain", "Saved. Attempting to connect...");
  delay(100);
  
  // Try to connect
  if (connectWiFi(ssid, pass)) {
    wifi_ssid = ssid;
    wifi_pass = pass;
    // Restart TCP server on new IP
    tcpServer.begin();
  } else {
    startAP();
  }
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ====== WebSocket ======
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_TEXT) {
    Serial2.write(payload, len);
  }
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBPI-R4 UART Bridge v" FIRMWARE_VERSION);
  
  // Load config from flash BEFORE initing UART
  prefs.begin("uart-bridge", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  uart_baud = prefs.getUInt("baud", UART_BAUD_DEFAULT);
  prefs.end();
  
  // Init UART with correct baud from config
  initUart();
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
  server.onNotFound(handleNotFound);
  server.begin();
  
  // WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  // TCP server
  tcpServer.begin();
  Serial.printf("TCP bridge on port %d\n", TCP_PORT);
}

// ====== Loop ======
void loop() {
  server.handleClient();
  webSocket.loop();
  
  // Handle TCP clients
  if (!tcpClient.connected()) {
    tcpClient.stop();
    tcpClient = tcpServer.available();
  }
  
  // UART -> WebSocket + TCP
  if (Serial2.available()) {
    uint8_t buf[128];
    size_t len = Serial2.readBytes(buf, sizeof(buf));
    if (len > 0) {
      webSocket.broadcastTXT((char*)buf, len);
      if (tcpClient.connected()) {
        tcpClient.write(buf, len);
      }
    }
  }
  
  // TCP -> UART
  if (tcpClient.connected() && tcpClient.available()) {
    uint8_t buf[128];
    int len = tcpClient.read(buf, sizeof(buf));
    if (len > 0) Serial2.write(buf, len);
  }
  
  delay(5);
}
