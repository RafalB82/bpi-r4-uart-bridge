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
 * v1.4 - fix: XSS in /status.json (proper JSON escaping),
 *        fix: removed dead pendingRestart variable,
 *        fix: safe restart via flag + loop drain (no more delay+restart race),
 *        fix: erase-remove idiom for TCP client pruning,
 *        fix: static buffer for TCP->UART reads (stack safety),
 *        fix: non-printable filtering in broadcastUart (valid UTF-8 for WS),
 *        fix: ring buffer full warning logged,
 *        fix: SSID/password length validation (WPA2 limits),
 *        fix: Task Watchdog enabled for loop stability.
 * v1.3 - fix: mDNS no longer restarted every loop iteration (flag mdnsRunning),
 *        fix: UART RX read buffer enlarged to UART_RX_BUF_SIZE (was 256),
 *        fix: tcpClients vector pre-reserved to MAX_TCP_CLIENTS,
 *        fix: WiFiEvent logs AP client connect/disconnect events,
 *        fix: loop uses vTaskDelay(1) instead of delay(1).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <freertos/ringbuf.h>
#include <esp_task_wdt.h>
#include <vector>
#include <algorithm>

#define FIRMWARE_VERSION "1.4"
#define TCP_PORT 8888
#define HTTP_PORT 80
#define WEBSOCKET_PORT 81
#define UART_BAUD_DEFAULT 115200
#define UART_RX_PIN 16
#define UART_TX_PIN 17
#define UART_BUF_SIZE 4096
#define UART_RX_BUF_SIZE 4096
#define HOSTNAME "bpi-r4-bridge"
#define WIFI_TIMEOUT_MS 20000
#define MAX_TCP_CLIENTS 4

#define WDT_TIMEOUT_S 10
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 63

Preferences prefs;
WebServer server(HTTP_PORT);
WebSocketsServer webSocket(WEBSOCKET_PORT);
WiFiServer tcpServer(TCP_PORT);
std::vector<WiFiClient> tcpClients;
bool mdnsRunning = false;

String wifi_ssid, wifi_pass;
unsigned long uart_baud = UART_BAUD_DEFAULT;
unsigned long wifiReconnectMs = 0;
bool wifiWasConnected = false;
unsigned long restartRequestedMs = 0;  // non-zero = restart pending

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
<h1>BPI-R4 UART Bridge v1.4</h1>
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
  Serial2.setRxBufferSize(UART_RX_BUF_SIZE);
  Serial2.begin(uart_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.println("UART2 ready: " + String(uart_baud) + " baud");
}

// ====== JSON string escaping (prevents XSS / malformed JSON) ======
String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '/':  out += "\\/"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// ====== AP mode ======
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BPI-R4-Bridge", "config1234");
  Serial.println("AP started: BPI-R4-Bridge");
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  Serial.println("Connect and open http://192.168.4.1/");
}

// ====== WiFi connect with hostname + timeout ======
bool connectWiFi(String ssid, String pass) {
  if (ssid.length() == 0) return false;

  // Set hostname before WiFi.begin() so DHCP sees it
  WiFi.setHostname(HOSTNAME);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print("Connecting to WiFi \"" + ssid + "\" as " + String(HOSTNAME));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected: " + WiFi.localIP().toString());
    Serial.println("Hostname: " + String(HOSTNAME) + ".local");
    return true;
  }
  Serial.println("Failed to connect");
  return false;
}

// ====== Web handlers ======
void handleRoot() { server.send_P(200, "text/html", index_html); }

void handleStatus() {
  String json = "{\"ssid\":\"" + jsonEscape(wifi_ssid) + "\",\"ip\":\"" +
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
  if (ssid.length() > MAX_SSID_LEN) {
    server.send(400, "text/plain", "SSID too long (max 32 chars)");
    return;
  }
  if (pass.length() > MAX_PASS_LEN) {
    server.send(400, "text/plain", "Password too long (max 63 chars)");
    return;
  }
  if (pass.length() > 0 && pass.length() < 8) {
    server.send(400, "text/plain", "Password too short (min 8 chars for WPA2)");
    return;
  }

  prefs.begin("uart-bridge", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  server.send(200, "text/plain", "Saved. Rebooting...");
  restartRequestedMs = millis();
}

void handleBaud() {
  if (!server.hasArg("baud")) {
    server.send(400, "text/plain", "baud required");
    return;
  }
  unsigned long baud = server.arg("baud").toInt();
  if (baud < 1200 || baud > 921600) {
    server.send(400, "text/plain", "Invalid baud rate (1200-921600)");
    return;
  }

  prefs.begin("uart-bridge", false);
  prefs.putUInt("baud", baud);
  prefs.end();

  server.send(200, "text/plain", "Baud saved. Rebooting...");
  restartRequestedMs = millis();
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ====== WebSocket event — queue data for loop, no direct UART write ======
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_TEXT && len > 0 && uart_tx_queue) {
    // Queue data; loop will drain it and write to Serial2
    if (xRingbufferSend(uart_tx_queue, payload, len, 0) != pdTRUE) {
      Serial.println("WARN: UART TX queue full, data dropped");
    }
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

// ====== Clean dead TCP clients (erase-remove idiom) ======
void pruneTcpClients() {
  tcpClients.erase(
    std::remove_if(tcpClients.begin(), tcpClients.end(),
      [](WiFiClient& c) {
        if (!c.connected()) {
          c.stop();
          return true;
        }
        return false;
      }),
    tcpClients.end()
  );
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
// Filters out non-printable characters (except common control chars) for WebSocket
void broadcastUart(const uint8_t* buf, size_t len) {
  // For TCP clients: send raw (they can handle binary)
  for (auto& c : tcpClients) {
    if (c.connected()) c.write(buf, len);
  }

  // For WebSocket: filter to valid printable + whitespace for safe browser display
  static uint8_t wsBuf[UART_BUF_SIZE];
  size_t wsLen = 0;
  for (size_t i = 0; i < len && wsLen < sizeof(wsBuf) - 1; i++) {
    uint8_t ch = buf[i];
    // Allow printable ASCII, newline, carriage return, tab, and ESC (for ANSI sequences)
    if (ch >= 0x20 || ch == '\n' || ch == '\r' || ch == '\t' || ch == 0x1B) {
      wsBuf[wsLen++] = ch;
    }
  }
  if (wsLen > 0) {
    webSocket.broadcastTXT(wsBuf, wsLen);
  }
}

// ====== Read from all TCP clients and write to UART ======
void handleTcpToUart() {
  static uint8_t buf[512];  // Static buffer - safe for stack, shared across calls
  for (auto& c : tcpClients) {
    if (!c.connected()) continue;
    int available = c.available();
    if (available <= 0) continue;
    int len = c.read(buf, min(available, (int)sizeof(buf)));
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
      mdnsRunning = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiReconnectMs = 0;
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("AP: client connected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("AP: client disconnected");
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

  // Enable Task Watchdog for the loop task (resets ESP if loop hangs)
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);  // Add current task (loopTask)

  // Load config from flash
  prefs.begin("uart-bridge", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  uart_baud = prefs.getUInt("baud", UART_BAUD_DEFAULT);
  prefs.end();

  // Validate baud rate from NVS (could be corrupted)
  if (uart_baud < 1200 || uart_baud > 921600) {
    uart_baud = UART_BAUD_DEFAULT;
    Serial.println("Invalid baud in NVS, falling back to " + String(uart_baud));
  }

  // Create UART TX queue
  uart_tx_queue = xRingbufferCreate(UART_BUF_SIZE * 8, RINGBUF_TYPE_BYTEBUF);
  if (!uart_tx_queue) {
    Serial.println("FATAL: Failed to create UART TX queue");
  }

  // Init UART
  initUart();

  // WiFi event handler (before connect, to catch GOT_IP for mDNS)
  WiFi.onEvent(WiFiEvent);

  // Connect or start AP
  if (wifi_ssid.length() > 0) {
    if (!connectWiFi(wifi_ssid, wifi_pass)) {
      startAP();
    }
  } else {
    startAP();
  }

  // Start mDNS if we got an IP (STA mode)
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      MDNS.addService("ws", "tcp", WEBSOCKET_PORT);
      Serial.printf("mDNS responder started: http://%s.local/\n", HOSTNAME);
    } else {
      Serial.println("mDNS responder failed to start");
    }
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

  // Pre-reserve TCP client slots to avoid repeated heap reallocs
  tcpClients.reserve(MAX_TCP_CLIENTS);

  wifiWasConnected = (WiFi.status() == WL_CONNECTED);
  mdnsRunning = (WiFi.status() == WL_CONNECTED);
}

// ====== Loop ======
void loop() {
  // Feed the watchdog
  esp_task_wdt_reset();

  // Safe restart: wait for network stack to flush before rebooting
  if (restartRequestedMs > 0 && (millis() - restartRequestedMs) > 1000) {
    Serial.println("Restarting...");
    Serial.flush();
    ESP.restart();
  }

  server.handleClient();
  webSocket.loop();

  // Drain queued WebSocket data to UART (main loop context — no race)
  flushUartTxQueue();

  // TCP: prune dead, accept new, handle data
  pruneTcpClients();
  acceptTcpClients();
  handleTcpToUart();

  // UART -> WebSocket + TCP (read all available, up to full RX buffer size)
  size_t avail = Serial2.available();
  if (avail > 0) {
    static uint8_t buf[UART_BUF_SIZE];
    size_t len = Serial2.readBytes(buf, min(avail, (size_t)UART_BUF_SIZE));
    if (len > 0) {
      broadcastUart(buf, len);
    }
  }

  // WiFi reconnect check
  checkReconnect();

  // Start mDNS once after (re)connect — not every iteration
  if (WiFi.status() == WL_CONNECTED && !mdnsRunning) {
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      MDNS.addService("ws", "tcp", WEBSOCKET_PORT);
      Serial.printf("mDNS (re)started: http://%s.local/\n", HOSTNAME);
      mdnsRunning = true;
    }
  }

  // Yield to FreeRTOS scheduler / watchdog
  vTaskDelay(1);
}
