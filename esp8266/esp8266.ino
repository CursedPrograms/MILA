#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// === WIFI ===
const char* ssid     = "MILA";
const char* password = "12345678";

// === WEB SERVER ===
ESP8266WebServer server(80);

// === STATE ===
String currentMode = "obstacle";
String lastCommand = "STOP";
float  lastDist    = 0;
String lastLeft    = "---";
String lastRight   = "---";
String lastTurn    = "";

// =====================
void setup() {
  Serial.begin(115200);
  delay(100);

  // Force AP config before starting
  IPAddress local_ip(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  WiFi.softAP(ssid, password);
  delay(1000);

  // Routes
  server.on("/",       handleRoot);
  server.on("/cmd",    handleCmd);
  server.on("/mode",   handleMode);
  server.on("/status", handleStatus);

  server.begin();

  // Boot into obstacle mode
  Serial.println("OBSTACLE");
}

// =====================
void loop() {
  server.handleClient();

  // Read sensor data from Arduino
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("DIST:"))  lastDist  = line.substring(5).toFloat();
    if (line.startsWith("LEFT:"))  lastLeft  = line.substring(5);
    if (line.startsWith("RIGHT:")) lastRight = line.substring(6);
    if (line.startsWith("TURN:"))  lastTurn  = line.substring(5);
  }
}

// =====================
void handleCmd() {
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
    Serial.println(currentMode == "obstacle" ? "OBSTACLE" : "MANUAL");
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "ERR");
  }
}

void handleStatus() {
  String json = "{";
  json += "\"mode\":\""  + currentMode       + "\",";
  json += "\"cmd\":\""   + lastCommand        + "\",";
  json += "\"dist\":"    + String(lastDist)   + ",";
  json += "\"left\":\""  + lastLeft           + "\",";
  json += "\"right\":\"" + lastRight          + "\",";
  json += "\"turn\":\""  + lastTurn           + "\"";
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

<div class="statusbar">
  <div class="stat">
    <div class="stat-label">MODE</div>
    <div class="stat-value" id="s-mode">OBSTACLE</div>
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
</div>

<main>
  <div class="mode-row">
    <button class="mode-btn active" id="btn-obstacle" onclick="setMode('obstacle')">⚡ OBSTACLE</button>
    <button class="mode-btn"        id="btn-wasd"     onclick="setMode('wasd')">🎮 WASD</button>
    <button class="mode-btn"        id="btn-dual"     onclick="setMode('dual')">🕹 DUAL MOTOR</button>
  </div>

  <!-- OBSTACLE -->
  <div class="panel active" id="panel-obstacle">
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
  <div class="panel" id="panel-wasd">
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

  <!-- DUAL -->
  <div class="panel" id="panel-dual">
    <div class="panel-title">◈ DUAL — INDEPENDENT MOTOR CONTROL</div>
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
</main>

<footer>MILA v1.0 &nbsp;|&nbsp; 192.168.4.1 &nbsp;|&nbsp; <span id="uptime">--:--</span></footer>

<script>
  let mode = 'obstacle';
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
    ['obstacle','wasd','dual'].forEach(x => {
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

  // Keyboard
  const keyMap = { w:'FORWARD', s:'BACKWARD', a:'LEFT', d:'RIGHT' };
  const held = {};
  document.addEventListener('keydown', e => {
    if (held[e.key] || mode === 'obstacle') return;
    held[e.key] = true;
    const cmd = keyMap[e.key.toLowerCase()];
    if (cmd) sendCmd(cmd);
  });
  document.addEventListener('keyup', e => {
    held[e.key] = false;
    const cmd = keyMap[e.key.toLowerCase()];
    if (cmd) sendCmd('STOP');
  });

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
