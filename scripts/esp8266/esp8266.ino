#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

// === WIFI ===
// On boot, try to join NORA's network as a station (so MILA and NORA can
// share a LAN and MILA can register itself with NORA's fleet registry).
// If NORA isn't reachable within the timeout, fall back to hosting our
// own access point exactly like before.
const char* nora_ssid     = "NORA";
const char* nora_password = "12345678";
const char* mila_ssid     = "MILA";
const char* mila_password = "12345678";
const unsigned long WIFI_JOIN_TIMEOUT_MS = 8000;

const IPAddress fleetHost(192, 168, 4, 1);   // NORA's fixed AP gateway address
const unsigned long FLEET_HEARTBEAT_MS = 15000;   // NORA drops entries after 20s of silence
bool fleetMode = false;   // true = joined NORA's network; false = running our own AP

// === WEB SERVER ===
ESP8266WebServer server(5010);

// === STATE ===
String currentMode = "wasd";
String lastCommand = "STOP";
float  lastDist    = 0;
String lastLeft    = "---";
String lastRight   = "---";
String lastTurn    = "";
String lastIR      = "---";
float  lastTemp    = 0;
float  lastHumidity = 0;
int    lastSpeed   = 100;
bool   lastGuard   = false;   // true while the manual-mode collision guard has force-stopped the robot

// === CONNECTION WATCHDOG ===
// Both clients (web dashboard and mila_controller.py) poll /status every
// 400ms for as long as they're running, whereas drive commands are only
// sent once per button press/release — so a gap in /status polling, not a
// gap in /cmd, is what actually tells us the client died or WiFi dropped.
// Without this, losing the connection mid-drive leaves the motors running
// forever since no STOP ever arrives.
const unsigned long CONN_TIMEOUT_MS = 1500;
unsigned long lastStatusPollMs = 0;

// =====================
void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(nora_ssid, nora_password);

  unsigned long attemptStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - attemptStart < WIFI_JOIN_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    fleetMode = true;
  } else {
    WiFi.disconnect();
    IPAddress local_ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(local_ip, gateway, subnet);
    WiFi.softAP(mila_ssid, mila_password);
    delay(500);
    fleetMode = false;
  }

  // Routes
  server.on("/",       handleRoot);
  server.on("/cmd",    handleCmd);
  server.on("/mode",   handleMode);
  server.on("/status", handleStatus);
  server.on("/fleet",  handleFleet);

  server.begin();

  if (fleetMode) fleetRegister();

  if (MDNS.begin("mila")) {
    MDNS.addService("http", "tcp", 5010);
  }

  ArduinoOTA.setHostname("mila");
  ArduinoOTA.begin();

  // Boot into WASD mode
  Serial.println("WASD");
}

// =====================
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  MDNS.update();

  if (fleetMode) {
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat >= FLEET_HEARTBEAT_MS) {
      lastHeartbeat = millis();
      fleetRegister();
    }
  }

  // Connection watchdog: if a client was polling /status and stops
  // (WiFi drop, closed app) while something is still driving, stop it.
  if (lastStatusPollMs != 0 && currentMode != "obstacle" && lastCommand != "STOP" &&
      millis() - lastStatusPollMs > CONN_TIMEOUT_MS) {
    lastCommand = "STOP";
    Serial.println("STOP");
  }

  // Read sensor data from Arduino
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("DIST:"))  lastDist  = line.substring(5).toFloat();
    if (line.startsWith("LEFT:"))  lastLeft  = line.substring(5);
    if (line.startsWith("RIGHT:")) lastRight = line.substring(6);
    if (line.startsWith("TURN:"))  lastTurn  = line.substring(5);
    if (line.startsWith("IR:"))    lastIR    = line.substring(3);
    if (line.startsWith("TEMP:"))  lastTemp     = line.substring(5).toFloat();
    if (line.startsWith("HUM:"))   lastHumidity = line.substring(4).toFloat();
    if (line.startsWith("SPEED:")) lastSpeed    = line.substring(6).toInt();
    if (line.startsWith("GUARD:")) lastGuard    = line.substring(6).toInt() != 0;
  }
}

// =====================
// Registers/heartbeats with NORA's fleet registry (see NORA's esp32.ino,
// port 5000: POST /register with name/type/capabilities). Best-effort —
// if NORA's registry doesn't respond we just move on and try again next
// heartbeat interval.
void fleetRegister() {
  WiFiClient client;
  if (!client.connect(fleetHost, 5000)) return;

  String body = "name=MILA&type=tank&capabilities=wasd,tank,obstacle_avoidance";
  client.print(String("POST /register HTTP/1.1\r\n") +
               "Host: " + fleetHost.toString() + "\r\n" +
               "Content-Type: application/x-www-form-urlencoded\r\n" +
               "Content-Length: " + body.length() + "\r\n" +
               "Connection: close\r\n\r\n" +
               body);
  client.stop();
}

// =====================
String currentIP() {
  return fleetMode ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

// =====================
// Proxies NORA's fleet roster (GET /robots on port 5000) so MILA's own
// dashboard can show her and any siblings without the browser needing to
// reach NORA directly.
void handleFleet() {
  if (!fleetMode) {
    server.send(200, "application/json", "{\"authority\":\"none\",\"robots\":[]}");
    return;
  }

  WiFiClient client;
  if (!client.connect(fleetHost, 5000)) {
    server.send(200, "application/json", "{\"authority\":\"unreachable\",\"robots\":[]}");
    return;
  }

  client.print(String("GET /robots HTTP/1.1\r\n") +
               "Host: " + fleetHost.toString() + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 2000) {
    delay(10);
  }

  String resp;
  bool inBody = false;
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (!inBody) {
      if (line == "\r") inBody = true;   // blank line marks end of HTTP headers
    } else {
      resp += line;
    }
  }
  client.stop();

  if (resp.length() == 0) resp = "{\"authority\":\"unreachable\",\"robots\":[]}";
  server.send(200, "application/json", resp);
}

// =====================
void handleCmd() {
  // Speed control works regardless of drive mode — both the preset cycle
  // and the dashboard's direct slider value ("SPEED:75").
  if (server.hasArg("v")) {
    String v = server.arg("v");
    if (v == "SPEEDCYCLE" || v.startsWith("SPEED:")) {
      Serial.println(v);
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  if (currentMode == "obstacle") {
    server.send(200, "text/plain", "IGNORED");
    return;
  }
  if (server.hasArg("v")) {
    lastCommand = server.arg("v");
    Serial.println(lastCommand);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "ERR");
  }
}

void handleMode() {
  if (server.hasArg("v")) {
    currentMode = server.arg("v");
    if      (currentMode == "obstacle") Serial.println("OBSTACLE");
    else if (currentMode == "tank")     Serial.println("TANK");
    else                                Serial.println("WASD");
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "ERR");
  }
}

void handleStatus() {
  lastStatusPollMs = millis();

  String json = "{";
  json += "\"mode\":\""  + currentMode       + "\",";
  json += "\"cmd\":\""   + lastCommand        + "\",";
  json += "\"dist\":"    + String(lastDist)   + ",";
  json += "\"left\":\""  + lastLeft           + "\",";
  json += "\"right\":\"" + lastRight          + "\",";
  json += "\"turn\":\""  + lastTurn           + "\",";
  json += "\"ir\":\""    + lastIR             + "\",";
  json += "\"temp\":"    + String(lastTemp, 1)     + ",";
  json += "\"hum\":"     + String(lastHumidity, 1) + ",";
  json += "\"ip\":\""    + currentIP()             + "\",";
  json += "\"fleet\":"   + String(fleetMode ? 1 : 0) + ",";
  json += "\"speed\":"   + String(lastSpeed) + ",";
  json += "\"guard\":"   + String(lastGuard ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

// =====================
void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MILA</title>
<style>
  :root {
    --bg:      #0a0a0f;
    --panel:   #12121a;
    --border:  #1e1e2e;
    --accent:  #00d4ff;
    --accent2: #7c3aed;
    --red:     #ff4444;
    --green:   #00ff88;
    --text:    #e0e0f0;
    --sub:     #666688;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Courier New', monospace;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
  }
  header {
    width: 100%;
    padding: 16px 24px;
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    gap: 14px;
  }
  .logo-ring {
    width: 40px; height: 40px;
    border-radius: 50%;
    border: 2px solid var(--accent);
    display: flex; align-items: center; justify-content: center;
    box-shadow: 0 0 14px var(--accent);
    font-size: 18px; font-weight: bold; color: var(--accent);
  }
  .logo-text h1 { font-size: 1.2rem; letter-spacing: 5px; color: var(--accent); }
  .logo-text p  { font-size: 0.6rem; color: var(--sub); letter-spacing: 2px; margin-top: 2px; }
  .conn-dot {
    margin-left: auto;
    width: 10px; height: 10px;
    border-radius: 50%;
    background: var(--green);
    box-shadow: 0 0 8px var(--green);
  }

  .statusbar {
    width: 100%; max-width: 860px;
    display: flex; gap: 10px; flex-wrap: wrap;
    padding: 14px 16px;
  }
  .stat {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px 14px;
    flex: 1; min-width: 100px;
  }
  .stat-label { font-size: 0.55rem; color: var(--sub); letter-spacing: 2px; margin-bottom: 4px; }
  .stat-value { font-size: 1rem; color: var(--accent); }

  main {
    width: 100%; max-width: 860px;
    padding: 0 16px 40px;
    display: flex; flex-direction: column; gap: 12px;
  }
  .mode-row { display: flex; gap: 8px; flex-wrap: wrap; }
  .mode-btn {
    flex: 1;
    padding: 12px 8px;
    border-radius: 8px;
    border: 1px solid var(--border);
    background: var(--panel);
    color: var(--sub);
    font-family: 'Courier New', monospace;
    font-size: 0.7rem; letter-spacing: 2px;
    cursor: pointer; transition: all 0.2s;
  }
  .mode-btn.active {
    border-color: var(--accent); color: var(--accent);
    box-shadow: 0 0 10px rgba(0,212,255,0.15);
  }

  .panel {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 20px;
    display: none;
  }
  .panel.active { display: block; }
  .panel-title {
    font-size: 0.6rem; letter-spacing: 3px;
    color: var(--sub); margin-bottom: 18px;
  }

  /* DPAD */
  .dpad {
    display: grid;
    grid-template-columns: repeat(3, 72px);
    grid-template-rows: repeat(3, 72px);
    gap: 6px;
    margin: 0 auto; width: fit-content;
  }
  .dpad-btn {
    width: 72px; height: 72px;
    background: #1a1a28;
    border: 1px solid var(--border);
    border-radius: 10px;
    color: var(--text); font-size: 1.5rem;
    cursor: pointer; transition: all 0.1s;
    display: flex; align-items: center; justify-content: center;
    user-select: none; -webkit-tap-highlight-color: transparent;
    touch-action: none;
  }
  .dpad-btn:active, .dpad-btn.pressed {
    background: var(--accent2);
    border-color: var(--accent);
    box-shadow: 0 0 14px rgba(0,212,255,0.3);
    transform: scale(0.92);
  }
  .dpad-btn.empty { visibility: hidden; pointer-events: none; }
  .dpad-stop {
    background: #2a1a1a; border-color: #4a2020;
    font-size: 0.65rem; letter-spacing: 1px; color: var(--red);
  }

  /* DUAL */
  .dual { display: flex; gap: 24px; justify-content: center; flex-wrap: wrap; }
  .stick-group { display: flex; flex-direction: column; align-items: center; gap: 8px; }
  .stick-label { font-size: 0.6rem; letter-spacing: 2px; color: var(--sub); }
  .stick-col { display: flex; flex-direction: column; gap: 6px; align-items: center; }
  .stick-btn {
    width: 72px; height: 72px;
    background: #1a1a28;
    border: 1px solid var(--border);
    border-radius: 10px;
    color: var(--text); font-size: 1.5rem;
    cursor: pointer; transition: all 0.1s;
    display: flex; align-items: center; justify-content: center;
    user-select: none; -webkit-tap-highlight-color: transparent;
    touch-action: none;
  }
  .stick-btn:active, .stick-btn.pressed {
    background: var(--accent2); border-color: var(--accent);
    box-shadow: 0 0 14px rgba(0,212,255,0.3);
    transform: scale(0.92);
  }

  /* OBSTACLE */
  .obs-wrap { display: flex; flex-direction: column; align-items: center; gap: 16px; }
  .scan-arc { width: 220px; height: 120px; }
  .scan-arc svg { width: 100%; height: 100%; }
  .obs-readings { display: flex; gap: 12px; justify-content: center; flex-wrap: wrap; }
  .obs-stat {
    background: #0e0e1a; border: 1px solid var(--border);
    border-radius: 8px; padding: 10px 16px; text-align: center; min-width: 90px;
  }
  .obs-stat-label { font-size: 0.55rem; color: var(--sub); letter-spacing: 2px; }
  .obs-stat-val   { font-size: 1.2rem; color: var(--accent); margin-top: 3px; }
  .turn-ind {
    font-size: 0.7rem; letter-spacing: 2px;
    padding: 8px 20px; border-radius: 6px;
    border: 1px solid var(--border); color: var(--sub);
  }
  .turn-ind.left  { border-color: var(--accent);  color: var(--accent);  }
  .turn-ind.right { border-color: var(--accent2); color: var(--accent2); }

  footer {
    font-size: 0.55rem; color: var(--sub);
    letter-spacing: 2px; padding: 16px; text-align: center;
  }

  /* SPEED */
  .speed-row { display: flex; align-items: center; gap: 12px; }
  .speed-row span { font-size: 0.6rem; color: var(--sub); letter-spacing: 2px; }
  .speed-row input[type=range] { flex: 1; accent-color: var(--accent); }
  #speedVal { color: var(--accent); font-size: 0.75rem; min-width: 34px; text-align: right; }

  /* GUARD BANNER */
  .guard-banner {
    display: none;
    width: 100%; max-width: 860px;
    margin: 0 16px 4px;
    background: #2a1010; border: 1px solid var(--red); color: var(--red);
    text-align: center; padding: 8px; font-size: 0.65rem; letter-spacing: 2px;
    border-radius: 8px;
  }

  /* TELEMETRY HISTORY */
  #histChart { width: 100%; height: 110px; display: block; background: #0e0e1a; border-radius: 6px; }
  .hist-legend { display: flex; gap: 14px; justify-content: center; font-size: 0.55rem; margin-top: 8px; }
</style>
</head>
<body>

<header>
  <div class="logo-ring">M</div>
  <div class="logo-text">
    <h1>M I L A</h1>
    <p>MINIATURE INTEGRATED LOGIC AUTOMATON</p>
  </div>
  <div class="conn-dot" id="connDot"></div>
</header>

<div class="guard-banner" id="guardBanner">⚠ COLLISION GUARD — OBSTACLE TOO CLOSE, STOPPED</div>

<div class="statusbar">
  <div class="stat">
    <div class="stat-label">MODE</div>
    <div class="stat-value" id="s-mode">WASD</div>
  </div>
  <div class="stat">
    <div class="stat-label">FRONT CM</div>
    <div class="stat-value" id="s-dist">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">LEFT CM</div>
    <div class="stat-value" id="s-left">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">RIGHT CM</div>
    <div class="stat-value" id="s-right">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">LAST CMD</div>
    <div class="stat-value" id="s-cmd">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">LAST IR</div>
    <div class="stat-value" id="s-ir">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">TEMP</div>
    <div class="stat-value" id="s-temp">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">HUMIDITY</div>
    <div class="stat-value" id="s-hum">---</div>
  </div>
  <div class="stat">
    <div class="stat-label">SPEED</div>
    <div class="stat-value" id="s-speed">100%</div>
  </div>
</div>

<main>
  <div class="panel active">
    <div class="panel-title">◈ TELEMETRY — LAST ~24s</div>
    <canvas id="histChart" width="820" height="110"></canvas>
    <div class="hist-legend">
      <span style="color:#00d4ff;">■ FRONT CM</span>
      <span style="color:#ff8800;">■ TEMP C</span>
      <span style="color:#7c3aed;">■ HUMIDITY %</span>
    </div>
  </div>

  <div class="mode-row">
    <button class="mode-btn active" id="btn-wasd"     onclick="setMode('wasd')">🎮 WASD</button>
    <button class="mode-btn"        id="btn-tank"     onclick="setMode('tank')">🕹 TANK</button>
    <button class="mode-btn"        id="btn-obstacle" onclick="setMode('obstacle')">⚡ OBSTACLE</button>
  </div>

  <div class="speed-row">
    <span>SPEED</span>
    <input type="range" id="speedSlider" min="25" max="100" step="25" value="100" oninput="setSpeed(this.value)">
    <span id="speedVal">100%</span>
  </div>

  <!-- OBSTACLE -->
  <div class="panel" id="panel-obstacle">
    <div class="panel-title">◈ AUTONOMOUS OBSTACLE AVOIDANCE</div>
    <div class="obs-wrap">
      <div class="scan-arc">
        <svg viewBox="0 0 220 120" xmlns="http://www.w3.org/2000/svg">
          <defs>
            <radialGradient id="ag" cx="50%" cy="100%" r="55%">
              <stop offset="0%"   stop-color="#00d4ff" stop-opacity="0.12"/>
              <stop offset="100%" stop-color="#00d4ff" stop-opacity="0"/>
            </radialGradient>
          </defs>
          <path d="M10,115 A100,100 0 0,1 210,115" fill="url(#ag)" stroke="#1e1e2e" stroke-width="1"/>
          <path d="M35,115 A75,75 0 0,1 185,115"   fill="none" stroke="#1e1e2e" stroke-width="1" stroke-dasharray="4,4"/>
          <path d="M60,115 A50,50 0 0,1 160,115"   fill="none" stroke="#1e1e2e" stroke-width="1" stroke-dasharray="2,4"/>
          <line x1="110" y1="115" x2="10"  y2="115" stroke="#1e1e2e" stroke-width="1"/>
          <line x1="110" y1="115" x2="210" y2="115" stroke="#1e1e2e" stroke-width="1"/>
          <line id="scanLine" x1="110" y1="115" x2="110" y2="15" stroke="#00d4ff" stroke-width="2" opacity="0.85"/>
          <circle cx="110" cy="115" r="4" fill="#00d4ff" opacity="0.8"/>
          <circle id="obsDot" cx="110" cy="50" r="5" fill="#ff4444" opacity="0"/>
        </svg>
      </div>
      <div class="obs-readings">
        <div class="obs-stat">
          <div class="obs-stat-label">◄ LEFT</div>
          <div class="obs-stat-val" id="obs-left">---</div>
        </div>
        <div class="obs-stat">
          <div class="obs-stat-label">▲ FRONT</div>
          <div class="obs-stat-val" id="obs-front">---</div>
        </div>
        <div class="obs-stat">
          <div class="obs-stat-label">RIGHT ►</div>
          <div class="obs-stat-val" id="obs-right">---</div>
        </div>
      </div>
      <div class="turn-ind" id="obs-turn">SCANNING...</div>
    </div>
  </div>

  <!-- WASD -->
  <div class="panel active" id="panel-wasd">
    <div class="panel-title">◈ WASD — KEYBOARD / TOUCH</div>
    <div class="dpad">
      <div class="dpad-btn empty"></div>
      <div class="dpad-btn" id="btn-w" ontouchstart="ev(event,'FORWARD')"  ontouchend="ev(event,'STOP')" onmousedown="ev(event,'FORWARD')"  onmouseup="ev(event,'STOP')">▲</div>
      <div class="dpad-btn empty"></div>
      <div class="dpad-btn" id="btn-a" ontouchstart="ev(event,'LEFT')"     ontouchend="ev(event,'STOP')" onmousedown="ev(event,'LEFT')"     onmouseup="ev(event,'STOP')">◄</div>
      <div class="dpad-btn dpad-stop"  ontouchstart="ev(event,'STOP')"     ontouchend="ev(event,'STOP')" onmousedown="ev(event,'STOP')"     onmouseup="ev(event,'STOP')">STOP</div>
      <div class="dpad-btn" id="btn-d" ontouchstart="ev(event,'RIGHT')"    ontouchend="ev(event,'STOP')" onmousedown="ev(event,'RIGHT')"    onmouseup="ev(event,'STOP')">►</div>
      <div class="dpad-btn empty"></div>
      <div class="dpad-btn" id="btn-s" ontouchstart="ev(event,'BACKWARD')" ontouchend="ev(event,'STOP')" onmousedown="ev(event,'BACKWARD')" onmouseup="ev(event,'STOP')">▼</div>
      <div class="dpad-btn empty"></div>
    </div>
  </div>

  <!-- TANK -->
  <div class="panel" id="panel-tank">
    <div class="panel-title">◈ TANK — INDEPENDENT TRACK CONTROL (Q/A left, E/D right)</div>
    <div class="dual">
      <div class="stick-group">
        <div class="stick-label">◈ LEFT MOTOR</div>
        <div class="stick-col">
          <div class="stick-btn" id="btn-lf" ontouchstart="ev(event,'L_FWD')" ontouchend="ev(event,'STOP')" onmousedown="ev(event,'L_FWD')" onmouseup="ev(event,'STOP')">▲</div>
          <div class="stick-btn" id="btn-lb" ontouchstart="ev(event,'L_BWD')" ontouchend="ev(event,'STOP')" onmousedown="ev(event,'L_BWD')" onmouseup="ev(event,'STOP')">▼</div>
        </div>
      </div>
      <div class="stick-group">
        <div class="stick-label">◈ RIGHT MOTOR</div>
        <div class="stick-col">
          <div class="stick-btn" id="btn-rf" ontouchstart="ev(event,'R_FWD')" ontouchend="ev(event,'STOP')" onmousedown="ev(event,'R_FWD')" onmouseup="ev(event,'STOP')">▲</div>
          <div class="stick-btn" id="btn-rb" ontouchstart="ev(event,'R_BWD')" ontouchend="ev(event,'STOP')" onmousedown="ev(event,'R_BWD')" onmouseup="ev(event,'STOP')">▼</div>
        </div>
      </div>
    </div>
  </div>

  <!-- FLEET -->
  <div class="panel" id="panel-fleet-wrap" style="display:block; padding:0;">
    <div id="fleetPanel" style="display:none;" class="panel active">
      <div class="panel-title">◈ FLEET</div>
      <div id="fleetAuthority" style="font-size:0.8rem; color:var(--accent); margin-bottom:8px;">---</div>
      <div id="fleetList" style="font-size:0.75rem; color:var(--sub);"></div>
    </div>
  </div>
</main>

<footer>MILA v1.0 &nbsp;|&nbsp; <span id="ipAddr">192.168.4.1</span>:5010 &nbsp;|&nbsp; <span id="uptime">--:--</span></footer>

<script>
  let mode = 'wasd';
  let scanAngle = 90, scanDir = 1;
  let startTime = Date.now();
  let connected = true;

  // Scan line animation
  setInterval(() => {
    scanAngle += scanDir * 2.5;
    if (scanAngle >= 150 || scanAngle <= 30) scanDir *= -1;
    const rad = (180 - scanAngle) * Math.PI / 180;
    const x2 = 110 + 100 * Math.cos(rad);
    const y2 = 115 - 100 * Math.sin(rad);
    const el = document.getElementById('scanLine');
    if (el) { el.setAttribute('x2', x2); el.setAttribute('y2', y2); }
  }, 35);

  // Uptime counter
  setInterval(() => {
    const s = Math.floor((Date.now() - startTime) / 1000);
    const m = Math.floor(s / 60);
    document.getElementById('uptime').textContent =
      String(m).padStart(2,'0') + ':' + String(s % 60).padStart(2,'0');
  }, 1000);

  function setMode(m) {
    mode = m;
    fetch('/mode?v=' + m);
    ['wasd','tank','obstacle'].forEach(x => {
      document.getElementById('btn-'   + x).classList.toggle('active', x === m);
      document.getElementById('panel-' + x).classList.toggle('active', x === m);
    });
    document.getElementById('s-mode').textContent = m.toUpperCase();
  }

  function ev(e, cmd) {
    e.preventDefault();
    if (mode === 'obstacle') return;
    sendCmd(cmd);
    // Visual feedback
    const el = e.currentTarget;
    if (cmd === 'STOP') {
      document.querySelectorAll('.dpad-btn, .stick-btn').forEach(b => b.classList.remove('pressed'));
    } else {
      el.classList.add('pressed');
    }
  }

  function sendCmd(cmd) {
    fetch('/cmd?v=' + cmd).catch(() => {});
    document.getElementById('s-cmd').textContent = cmd;
  }

  // Keyboard — WASD mode drives like a d-pad, TANK mode drives each track
  // independently (Q/A = left track fwd/back, E/D = right track fwd/back).
  const wasdKeyMap = { w:'FORWARD', s:'BACKWARD', a:'LEFT', d:'RIGHT' };
  const tankKeyMap = { q:'L_FWD', a:'L_BWD', e:'R_FWD', d:'R_BWD' };
  const held = {};
  document.addEventListener('keydown', e => {
    if (held[e.key] || mode === 'obstacle') return;
    held[e.key] = true;
    const k = e.key.toLowerCase();
    const cmd = mode === 'tank' ? tankKeyMap[k] : wasdKeyMap[k];
    if (cmd) sendCmd(cmd);
  });
  document.addEventListener('keyup', e => {
    held[e.key] = false;
    const k = e.key.toLowerCase();
    const cmd = mode === 'tank' ? tankKeyMap[k] : wasdKeyMap[k];
    if (cmd) sendCmd('STOP');
  });

  // X cycles speed presets (100/75/50/25), same button the IR remote's
  // OK key now triggers — mirrors it here for keyboard/touch parity.
  document.addEventListener('keydown', e => {
    if (e.key.toLowerCase() === 'x') fetch('/cmd?v=SPEEDCYCLE').catch(() => {});
  });

  // Speed slider — sends an explicit value rather than stepping presets.
  function setSpeed(v) {
    document.getElementById('speedVal').textContent = v + '%';
    fetch('/cmd?v=SPEED:' + v).catch(() => {});
  }

  // Telemetry history — a rolling buffer of the last ~24s (60 samples at
  // the 400ms poll interval), drawn as a plain canvas sparkline. Kept
  // entirely client-side since /status already carries everything needed.
  const HIST_LEN = 60;
  const distHist = [], tempHist = [], humHist = [];
  function pushHist(arr, val, max) {
    arr.push(Math.max(0, Math.min(max, isNaN(val) ? 0 : val)));
    if (arr.length > HIST_LEN) arr.shift();
  }
  function drawHistory() {
    const canvas = document.getElementById('histChart');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    function line(arr, max, color) {
      if (arr.length < 2) return;
      ctx.beginPath();
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.5;
      arr.forEach((v, i) => {
        const x = (i / (HIST_LEN - 1)) * w;
        const y = h - (v / max) * h;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();
    }
    line(distHist, 100, '#00d4ff');
    line(tempHist, 50,  '#ff8800');
    line(humHist,  100, '#7c3aed');
  }

  // Fleet roster — only relevant once MILA has joined NORA's network.
  function pollFleet() {
    fetch('/fleet').then(r => r.json()).then(f => {
      const panel = document.getElementById('fleetPanel');
      if (f.authority === 'none') { panel.style.display = 'none'; return; }
      panel.style.display = 'block';
      document.getElementById('fleetAuthority').textContent = 'authority: ' + f.authority;
      const others = (f.robots || []).filter(r => r.name !== 'MILA');
      document.getElementById('fleetList').innerHTML = others.length
        ? others.map(r => `&bull; ${r.name} (${r.type}) &mdash; ${r.ip}`).join('<br>')
        : '(no other robots registered)';
    }).catch(() => {});
  }
  setInterval(pollFleet, 3000);
  pollFleet();

  // Status polling
  function poll() {
    fetch('/status')
      .then(r => r.json())
      .then(d => {
        connected = true;
        document.getElementById('connDot').style.background = '#00ff88';
        document.getElementById('connDot').style.boxShadow  = '0 0 8px #00ff88';

        document.getElementById('s-dist').textContent  = d.dist + ' cm';
        document.getElementById('s-left').textContent  = d.left + ' cm';
        document.getElementById('s-right').textContent = d.right + ' cm';
        document.getElementById('s-cmd').textContent   = d.cmd;
        document.getElementById('s-ir').textContent    = d.ir;
        document.getElementById('s-temp').textContent  = d.temp + ' C';
        document.getElementById('s-hum').textContent   = d.hum + ' %';
        document.getElementById('ipAddr').textContent  = d.ip;
        document.getElementById('s-speed').textContent = d.speed + '%';

        document.getElementById('guardBanner').style.display = (d.guard == 1) ? 'block' : 'none';

        const slider = document.getElementById('speedSlider');
        if (document.activeElement !== slider) {
          slider.value = d.speed;
          document.getElementById('speedVal').textContent = d.speed + '%';
        }

        pushHist(distHist, parseFloat(d.dist), 100);
        pushHist(tempHist, parseFloat(d.temp), 50);
        pushHist(humHist,  parseFloat(d.hum),  100);
        drawHistory();

        document.getElementById('obs-front').textContent = d.dist + ' cm';
        document.getElementById('obs-left').textContent  = d.left + ' cm';
        document.getElementById('obs-right').textContent = d.right + ' cm';

        const ti = document.getElementById('obs-turn');
        if (d.turn === 'LEFT')  { ti.textContent = '◄ TURNING LEFT';  ti.className = 'turn-ind left';  }
        else if (d.turn === 'RIGHT') { ti.textContent = 'TURNING RIGHT ►'; ti.className = 'turn-ind right'; }
        else                    { ti.textContent = '▲ MOVING FORWARD'; ti.className = 'turn-ind'; }

        // Obstacle dot position on arc
        const dot = document.getElementById('obsDot');
        const dist = parseFloat(d.dist);
        if (dist < 100 && dist > 0) {
          const mapped = Math.max(25, 115 - dist * 0.9);
          dot.setAttribute('cy', mapped);
          dot.setAttribute('opacity', '1');
        } else {
          dot.setAttribute('opacity', '0');
        }
      })
      .catch(() => {
        connected = false;
        document.getElementById('connDot').style.background = '#ff4444';
        document.getElementById('connDot').style.boxShadow  = '0 0 8px #ff4444';
      });
  }

  setInterval(poll, 400);
  poll();
</script>
</body>
</html>
)rawhtml";

  server.send(200, "text/html", html);
}
