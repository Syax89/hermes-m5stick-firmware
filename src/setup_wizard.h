#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <M5StickCPlus.h>
#include "stats.h"
#include "buddy.h"
#include "character.h"

extern TFT_eSprite spr;
extern const int W;
extern const int H;
extern bool buddyMode;

enum SetupPersonaState { 
  SETUP_P_SLEEP, 
  SETUP_P_IDLE, 
  SETUP_P_BUSY, 
  SETUP_P_ATTENTION, 
  SETUP_P_CELEBRATE, 
  SETUP_P_DIZZY, 
  SETUP_P_HEART 
};

inline void runSetupWizard() {
  // 1. Show scanning screen on M5StickC Plus2 screen
  spr.fillSprite(0x0842); // dark grey-blue
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(0xFFFF, 0x0842);
  spr.drawString("Scansione Wi-Fi...", W / 2, H / 2);
  spr.pushSprite(0, 0);

  // Scan networks
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int numNetworks = WiFi.scanNetworks();

  // Generate options
  String wifiOptions = "";
  for (int i = 0; i < numNetworks; ++i) {
    if (WiFi.SSID(i).isEmpty()) continue;
    bool dup = false;
    for (int j = 0; j < i; ++j) {
      if (WiFi.SSID(i) == WiFi.SSID(j)) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      wifiOptions += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>\n";
    }
  }

  // Get MAC Address for unique AP Name
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apName[32];
  snprintf(apName, sizeof(apName), "Hermes-Buddy-%02X%02X", mac[4], mac[5]);

  // Start Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName);
  delay(100);

  // Start DNS Server (captive portal)
  DNSServer dnsServer;
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  // Start Web Server
  WebServer server(80);
  
  volatile bool setupDone = false;
  volatile bool shouldReboot = false;
  uint32_t rebootTime = 0;

  server.on("/", HTTP_GET, [&]() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Hermes Buddy Setup</title>
  <style>
    :root {
      --bg-color: #0b0f19;
      --panel-bg: rgba(17, 24, 39, 0.7);
      --border-color: rgba(255, 255, 255, 0.08);
      --accent: linear-gradient(135deg, #6366f1 0%, #a855f7 100%);
      --accent-hover: linear-gradient(135deg, #4f46e5 0%, #9333ea 100%);
      --text-color: #f3f4f6;
      --text-dim: #9ca3af;
      --glow: rgba(168, 85, 247, 0.4);
    }
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }
    body {
      background-color: var(--bg-color);
      color: var(--text-color);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      padding: 20px;
      overflow-x: hidden;
      position: relative;
    }
    body::before {
      content: '';
      position: absolute;
      top: -10%;
      left: -10%;
      width: 50%;
      height: 50%;
      background: radial-gradient(circle, rgba(99, 102, 241, 0.15) 0%, transparent 70%);
      pointer-events: none;
      z-index: 0;
    }
    body::after {
      content: '';
      position: absolute;
      bottom: -10%;
      right: -10%;
      width: 50%;
      height: 50%;
      background: radial-gradient(circle, rgba(168, 85, 247, 0.15) 0%, transparent 70%);
      pointer-events: none;
      z-index: 0;
    }
    .container {
      width: 100%;
      max-width: 480px;
      background: var(--panel-bg);
      border: 1px solid var(--border-color);
      border-radius: 20px;
      padding: 30px;
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5), 0 0 40px rgba(99, 102, 241, 0.05);
      z-index: 1;
    }
    h1 {
      font-size: 24px;
      font-weight: 700;
      text-align: center;
      margin-bottom: 8px;
      background: var(--accent);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      letter-spacing: -0.5px;
    }
    .subtitle {
      font-size: 14px;
      color: var(--text-dim);
      text-align: center;
      margin-bottom: 28px;
    }
    .form-group {
      margin-bottom: 20px;
      position: relative;
    }
    label {
      display: block;
      font-size: 12px;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      margin-bottom: 6px;
      color: var(--text-dim);
    }
    .input-wrapper {
      position: relative;
      display: flex;
      align-items: center;
    }
    input, select {
      width: 100%;
      background: rgba(31, 41, 55, 0.5);
      border: 1px solid var(--border-color);
      border-radius: 10px;
      padding: 12px 16px;
      color: var(--text-color);
      font-size: 15px;
      outline: none;
      transition: all 0.3s ease;
    }
    input:focus, select:focus {
      border-color: #a855f7;
      box-shadow: 0 0 10px var(--glow);
      background: rgba(31, 41, 55, 0.8);
    }
    .password-toggle {
      position: absolute;
      right: 14px;
      background: none;
      border: none;
      color: var(--text-dim);
      cursor: pointer;
      font-size: 13px;
      padding: 4px;
      display: flex;
      align-items: center;
      justify-content: center;
      transition: color 0.2s;
    }
    .password-toggle:hover {
      color: var(--text-color);
    }
    input[type="password"] {
      padding-right: 45px;
    }
    .field-help {
      font-size: 11px;
      color: #9ca3af;
      margin-top: 6px;
      line-height: 1.4;
    }
    .field-help a {
      color: #a855f7;
      text-decoration: none;
      font-weight: 500;
      transition: color 0.2s;
    }
    .field-help a:hover {
      color: #6366f1;
      text-decoration: underline;
    }
    .btn-submit {
      width: 100%;
      background: var(--accent);
      border: none;
      border-radius: 10px;
      padding: 14px;
      color: white;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      margin-top: 10px;
      transition: all 0.3s ease;
      box-shadow: 0 4px 15px rgba(168, 85, 247, 0.3);
    }
    .btn-submit:hover {
      background: var(--accent-hover);
      box-shadow: 0 6px 20px rgba(168, 85, 247, 0.5);
      transform: translateY(-1px);
    }
    .btn-submit:active {
      transform: translateY(1px);
    }
    .footer {
      text-align: center;
      margin-top: 24px;
      font-size: 11px;
      color: rgba(255, 255, 255, 0.3);
    }
    .flex-row {
      display: flex;
      gap: 12px;
    }
    .flex-row .form-group {
      flex: 1;
    }
    .port-group {
      max-width: 100px;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Hermes Buddy</h1>
    <div class="subtitle">Configurazione Iniziale</div>
    
    <form action="/save" method="POST" id="setupForm">
      <!-- Wi-Fi SSID Dropdown & Input -->
      <div class="form-group">
        <label for="wifiSelect">Reti Wi-Fi rilevate</label>
        <select id="wifiSelect" onchange="selectWifiNetwork()">
          <option value="">-- Seleziona una rete --</option>
          WIFI_OPTIONS_PLACEHOLDER
          <option value="custom">Inserisci manualmente...</option>
        </select>
        <div class="field-help">Seleziona la tua rete Wi-Fi domestica. Nota: Hermes Buddy supporta solo reti a 2.4 GHz.</div>
      </div>

      <div class="form-group" id="manualSsidGroup" style="display: none;">
        <label for="ssid">SSID Wi-Fi</label>
        <input type="text" id="ssid" name="ssid" placeholder="Nome della rete" maxlength="32">
        <div class="field-help">Inserisci manualmente il nome (SSID) della tua rete Wi-Fi.</div>
      </div>

      <div class="form-group">
        <label for="pass">Password Wi-Fi</label>
        <div class="input-wrapper">
          <input type="password" id="pass" name="pass" placeholder="Password della rete" maxlength="63">
          <button type="button" class="password-toggle" onclick="togglePassword('pass')">mostra</button>
        </div>
        <div class="field-help">La password della rete Wi-Fi selezionata. Lascia vuoto se la rete è aperta.</div>
      </div>

      <!-- Groq API Key -->
      <div class="form-group">
        <label for="groq">API Key Groq</label>
        <div class="input-wrapper">
          <input type="password" id="groq" name="groq" placeholder="gsk_..." required maxlength="127">
          <button type="button" class="password-toggle" onclick="togglePassword('groq')">mostra</button>
        </div>
        <div class="field-help">Usata per la trascrizione vocale ultra-rapida. Genera una chiave gratuita su <a href="https://console.groq.com/keys" target="_blank">console.groq.com</a> (inizia con <code>gsk_</code>).</div>
      </div>

      <!-- Hermes Server Address & Port -->
      <div class="flex-row">
        <div class="form-group">
          <label for="ip">Indirizzo Hermes</label>
          <input type="text" id="ip" name="ip" placeholder="192.168.1.100 o host" required maxlength="63" value="192.168.1.100">
          <div class="field-help">L'IP locale (es. <code>192.168.1.X</code>) o l'host del PC su cui gira il tuo server Hermes.</div>
        </div>
        <div class="form-group port-group">
          <label for="port">Porta</label>
          <input type="number" id="port" name="port" placeholder="8642" value="8642" required min="1" max="65535">
          <div class="field-help">Default: 8642.</div>
        </div>
      </div>

      <!-- Hermes API Key -->
      <div class="form-group">
        <label for="hkey">API Key Hermes</label>
        <div class="input-wrapper">
          <input type="password" id="hkey" name="hkey" placeholder="API Key Hermes" required maxlength="63">
          <button type="button" class="password-toggle" onclick="togglePassword('hkey')">mostra</button>
        </div>
        <div class="field-help">La chiave API di autenticazione configurata nel tuo server Hermes.</div>
      </div>

      <button type="submit" class="btn-submit">Salva e Riavvia</button>
    </form>
    
    <div class="footer">Hermes Buddy Setup Portal</div>
  </div>

  <script>
    function togglePassword(id) {
      const input = document.getElementById(id);
      const btn = input.nextElementSibling;
      if (input.type === "password") {
        input.type = "text";
        btn.textContent = "nascondi";
      } else {
        input.type = "password";
        btn.textContent = "mostra";
      }
    }

    function selectWifiNetwork() {
      const select = document.getElementById('wifiSelect');
      const manualGroup = document.getElementById('manualSsidGroup');
      const ssidInput = document.getElementById('ssid');
      
      if (select.value === 'custom') {
        manualGroup.style.display = 'block';
        ssidInput.required = true;
        ssidInput.value = '';
        ssidInput.focus();
      } else if (select.value !== '') {
        manualGroup.style.display = 'none';
        ssidInput.required = false;
        ssidInput.value = select.value;
      } else {
        manualGroup.style.display = 'none';
        ssidInput.required = false;
        ssidInput.value = '';
      }
    }
    
    selectWifiNetwork();

    document.getElementById('setupForm').addEventListener('submit', function(e) {
      const select = document.getElementById('wifiSelect');
      const ssidInput = document.getElementById('ssid');
      if (select.value === '' && ssidInput.value === '') {
        e.preventDefault();
        alert('Per favore, seleziona o inserisci una rete Wi-Fi.');
      }
    });
  </script>
</body>
</html>
)rawliteral";
    html.replace("WIFI_OPTIONS_PLACEHOLDER", wifiOptions);
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [&]() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String groq = server.arg("groq");
    String ip = server.arg("ip");
    String portStr = server.arg("port");
    String hkey = server.arg("hkey");

    if (ssid.length() > 0 && groq.length() > 0 && ip.length() > 0 && hkey.length() > 0) {
      strncpy(settings().wifiSsid, ssid.c_str(), sizeof(settings().wifiSsid) - 1);
      settings().wifiSsid[sizeof(settings().wifiSsid) - 1] = 0;
      
      strncpy(settings().wifiPass, pass.c_str(), sizeof(settings().wifiPass) - 1);
      settings().wifiPass[sizeof(settings().wifiPass) - 1] = 0;
      
      strncpy(settings().groqKey, groq.c_str(), sizeof(settings().groqKey) - 1);
      settings().groqKey[sizeof(settings().groqKey) - 1] = 0;
      
      strncpy(settings().hermesIp, ip.c_str(), sizeof(settings().hermesIp) - 1);
      settings().hermesIp[sizeof(settings().hermesIp) - 1] = 0;
      
      settings().hermesPort = portStr.toInt();
      if (settings().hermesPort == 0) settings().hermesPort = 8642;
      
      strncpy(settings().hermesKey, hkey.c_str(), sizeof(settings().hermesKey) - 1);
      settings().hermesKey[sizeof(settings().hermesKey) - 1] = 0;
      
      settings().configured = true;
      
      settingsSave();

      String successHtml = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Configurazione Completata</title>
  <style>
    body {
      background-color: #0b0f19;
      color: #f3f4f6;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      margin: 0;
      text-align: center;
    }
    .card {
      background: rgba(17, 24, 39, 0.7);
      border: 1px solid rgba(255, 255, 255, 0.08);
      border-radius: 20px;
      padding: 40px;
      max-width: 400px;
      backdrop-filter: blur(16px);
      box-shadow: 0 10px 30px rgba(0,0,0,0.5);
    }
    h1 {
      color: #a855f7;
      margin-bottom: 16px;
    }
    p {
      color: #9ca3af;
      margin-bottom: 24px;
      line-height: 1.5;
    }
    .spinner {
      border: 3px solid rgba(255, 255, 255, 0.1);
      width: 36px;
      height: 36px;
      border-radius: 50%;
      border-left-color: #a855f7;
      animation: spin 1s linear infinite;
      margin: 0 auto;
    }
    @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
  </style>
</head>
<body>
  <div class="card">
    <h1>Salvataggio...</h1>
    <p>Configurazione salvata con successo! Il dispositivo si sta riavviando.</p>
    <div class="spinner"></div>
  </div>
</body>
</html>
)rawliteral";
      server.send(200, "text/html", successHtml);
      
      shouldReboot = true;
      rebootTime = millis() + 2000;
    } else {
      server.send(400, "text/plain", "Parametri non validi!");
    }
  });

  server.onNotFound([&]() {
    if (server.hostHeader() != "192.168.4.1") {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Non Trovato");
    }
  });

  server.begin();

  // Chirp twice to indicate setup mode active
  if (settings().sound) {
    M5.Beep.tone(1000, 100); delay(100);
    M5.Beep.tone(1000, 100);
  }

  uint32_t frame = 0;
  while (!setupDone) {
    dnsServer.processNextRequest();
    server.handleClient();
    
    int clients = WiFi.softAPgetStationNum();
    
    // Animate mascot
    if (buddyMode) {
      buddyTick(SETUP_P_CELEBRATE);
    } else if (characterLoaded()) {
      characterTick();
    }
    
    // Draw screen
    {
      uint16_t bgCol = 0x0842; // dark grey-blue
      uint16_t txtCol = 0xFFFF; // white
      uint16_t accentCol = 0xA81F; // violet
      uint16_t accentCol2 = 0x07FF; // cyan
      
      spr.fillSprite(bgCol);
      int cx = W / 2;
      
      spr.setTextDatum(MC_DATUM);
      spr.setTextSize(2);
      spr.setTextColor(accentCol, bgCol);
      spr.drawString("SETUP MODE", cx, 25);
      
      spr.setTextSize(1);
      spr.setTextColor(txtCol, bgCol);
      spr.drawString("Connetti al Wi-Fi:", cx, 55);
      
      int ssidW = spr.textWidth(apName) + 16;
      spr.fillRoundRect(cx - ssidW/2, 68, ssidW, 20, 4, 0x18E3);
      spr.setTextColor(0xFFFF, 0x18E3);
      spr.drawString(apName, cx, 78);
      
      int my = 135;
      if (buddyMode) {
        // Redraw buddy at position
        // buddyTick drew to spr at its default center coords already, 
        // so we don't need to do anything else.
      } else if (characterLoaded()) {
        characterRenderTo(&spr, cx, my);
      } else {
        spr.setTextSize(2);
        spr.setTextColor(0x07E0, bgCol);
        spr.drawString("[:-)]", cx, my);
      }
      
      spr.setTextSize(1);
      spr.setTextColor(txtCol, bgCol);
      spr.drawString("Apri il browser su:", cx, 185);
      
      spr.fillRoundRect(cx - 50, 195, 100, 18, 4, 0x18E3);
      spr.setTextColor(0xFFFF, 0x18E3);
      spr.drawString("192.168.4.1", cx, 204);
      
      spr.setTextSize(1);
      spr.setTextColor(0x94B2, bgCol);
      char clientsBuf[32];
      snprintf(clientsBuf, sizeof(clientsBuf), "Dispositivi: %d", clients);
      spr.drawString(clientsBuf, cx, 227);
      
      if (clients > 0) {
        uint16_t dotCol = (frame / 5) % 2 ? 0x07E0 : bgCol;
        spr.fillCircle(10, 227, 4, dotCol);
      } else {
        uint16_t dotCol = (frame / 10) % 2 ? 0xF800 : bgCol;
        spr.fillCircle(10, 227, 4, dotCol);
      }
      
      spr.pushSprite(0, 0);
    }
    
    if (shouldReboot && millis() > rebootTime) {
      setupDone = true;
    }
    
    frame++;
    delay(30);
  }

  // Chirp and restart
  if (settings().sound) {
    M5.Beep.tone(1500, 300); delay(300);
  }
  ESP.restart();
}
