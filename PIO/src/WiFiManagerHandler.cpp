#include "WiFiManagerHandler.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "ui.h"

#define SD_SPI_SCK  40
#define SD_SPI_MISO 39
#define SD_SPI_MOSI 14
#define SD_SPI_CS   12

AsyncWebServer server(80);
Preferences preferences;
const char* CONFIG_FILE = "/config.json";
const char* PREF_NAMESPACE = "gemini";

extern volatile bool wifiConnectedFlag;
extern volatile bool apiKeySavedFlag;
static bool sdInitialized = false;
static bool sdChecked = false;

static WebChatHandler gWebChatHandler = nullptr;
static DeviceStateProvider gDeviceStateProvider = nullptr;
static RemoteInputHandler gRemoteInputHandler = nullptr;
static TranscriptProvider gTranscriptProvider = nullptr;
static ShakeEasterEggTrigger gShakeEasterEggTrigger = nullptr;

void setWebChatHandler(WebChatHandler handler) {
    gWebChatHandler = handler;
}

void setDeviceStateProvider(DeviceStateProvider provider) {
    gDeviceStateProvider = provider;
}

void setRemoteInputHandler(RemoteInputHandler handler) {
    gRemoteInputHandler = handler;
}

void setTranscriptProvider(TranscriptProvider provider) {
    gTranscriptProvider = provider;
}

void setShakeEasterEggTrigger(ShakeEasterEggTrigger trigger) {
    gShakeEasterEggTrigger = trigger;
}

static void ensureNVSInitialized() {
    static bool nvsReady = false;
    if (!nvsReady) {
        preferences.begin(PREF_NAMESPACE, false);
        preferences.end();
        nvsReady = true;
    }
}

bool initSD() {
    ensureNVSInitialized();
    if (sdChecked && sdInitialized) return true;
    if (sdChecked && !sdInitialized) return false;

    sdChecked = true;
    SPI.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_SPI_CS);
    if (!SD.begin(SD_SPI_CS, SPI, 25000000)) {
        Serial.println("[Storage] MicroSD not found, using Flash NVS.");
        sdInitialized = false;
        return false;
    }

    sdInitialized = true;
    Serial.println("[Storage] MicroSD Card mounted.");
    return true;
}

bool isSDCardAvailable() {
    return initSD();
}

static bool loadSDConfigDoc(StaticJsonDocument<512> &doc) {
    if (!initSD()) return false;
    if (!SD.exists(CONFIG_FILE)) return true;

    File configFile = SD.open(CONFIG_FILE, FILE_READ);
    if (!configFile) return true;

    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error) {
        Serial.println("[Storage] Corrupt config.json on SD, resetting.");
        doc.clear();
    }
    return true;
}

static bool writeSDConfigDoc(StaticJsonDocument<512> &doc) {
    if (!initSD()) return false;

    File configFile = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!configFile) {
        Serial.println("[Storage] Failed to open config.json for writing on SD!");
        return false;
    }

    if (serializeJson(doc, configFile) == 0) {
        Serial.println("[Storage] Failed to write config JSON to SD!");
        configFile.close();
        return false;
    }

    configFile.close();
    return true;
}

bool saveApiKey(const String &apiKey) {
    ensureNVSInitialized();
    String cleanKey = apiKey;
    cleanKey.trim();
    cleanKey.replace("\r", "");
    cleanKey.replace("\n", "");
    cleanKey.replace(" ", "");

    preferences.begin(PREF_NAMESPACE, false);
    preferences.putString("apikey", cleanKey);
    preferences.end();

    if (initSD()) {
        StaticJsonDocument<512> doc;
        loadSDConfigDoc(doc);
        doc["apikey"] = cleanKey;
        writeSDConfigDoc(doc);
    }

    apiKeySavedFlag = true;
    Serial.println("[Storage] API Key saved.");
    return true;
}

String getStoredApiKey() {
    ensureNVSInitialized();
    if (initSD() && SD.exists(CONFIG_FILE)) {
        File configFile = SD.open(CONFIG_FILE, FILE_READ);
        if (configFile) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, configFile);
            configFile.close();
            if (!error && doc.containsKey("apikey")) {
                String k = doc["apikey"].as<String>();
                k.trim();
                if (!k.isEmpty()) return k;
            }
        }
    }

    preferences.begin(PREF_NAMESPACE, false);
    String apiKey = preferences.getString("apikey", "");
    preferences.end();
    apiKey.trim();
    return apiKey;
}

bool saveSelectedModel(const String &modelName) {
    ensureNVSInitialized();
    String cleanModel = modelName;
    cleanModel.trim();
    if (cleanModel.startsWith("models/")) {
        cleanModel = cleanModel.substring(7);
    }

    preferences.begin(PREF_NAMESPACE, false);
    preferences.putString("model", cleanModel);
    preferences.end();

    if (initSD()) {
        StaticJsonDocument<512> doc;
        loadSDConfigDoc(doc);
        doc["model"] = cleanModel;
        writeSDConfigDoc(doc);
    }

    Serial.println("[Storage] Model saved: " + cleanModel);
    return true;
}

String getStoredModel() {
    ensureNVSInitialized();
    if (initSD() && SD.exists(CONFIG_FILE)) {
        File configFile = SD.open(CONFIG_FILE, FILE_READ);
        if (configFile) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, configFile);
            configFile.close();
            if (!error && doc.containsKey("model")) {
                String m = doc["model"].as<String>();
                m.trim();
                if (!m.isEmpty() && m != "gemini-1.5-flash") {
                    if (m.startsWith("models/")) m = m.substring(7);
                    return m;
                }
            }
        }
    }

    preferences.begin(PREF_NAMESPACE, false);
    String model = preferences.getString("model", "gemini-3.1-flash-lite");
    preferences.end();
    model.trim();
    if (model.startsWith("models/")) model = model.substring(7);
    if (model.isEmpty() || model == "gemini-1.5-flash") {
        return "gemini-3.1-flash-lite";
    }
    return model;
}

bool saveWifiCredentials(const String &ssid, const String &password) {
    ensureNVSInitialized();
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", password);
    preferences.end();

    if (initSD()) {
        StaticJsonDocument<512> doc;
        loadSDConfigDoc(doc);
        doc["wifi_ssid"] = ssid;
        doc["wifi_pass"] = password;
        writeSDConfigDoc(doc);
    }

    Serial.println("[Storage] Wi-Fi credentials saved: " + ssid);
    return true;
}

String getStoredWifiSSID() {
    ensureNVSInitialized();
    if (initSD() && SD.exists(CONFIG_FILE)) {
        File configFile = SD.open(CONFIG_FILE, FILE_READ);
        if (configFile) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, configFile);
            configFile.close();
            if (!error && doc.containsKey("wifi_ssid")) {
                String s = doc["wifi_ssid"].as<String>();
                s.trim();
                if (!s.isEmpty()) return s;
            }
        }
    }

    preferences.begin(PREF_NAMESPACE, false);
    String ssid = preferences.getString("wifi_ssid", "");
    preferences.end();
    ssid.trim();
    return ssid;
}

String getStoredWifiPassword() {
    ensureNVSInitialized();
    if (initSD() && SD.exists(CONFIG_FILE)) {
        File configFile = SD.open(CONFIG_FILE, FILE_READ);
        if (configFile) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, configFile);
            configFile.close();
            if (!error && doc.containsKey("wifi_pass")) {
                String p = doc["wifi_pass"].as<String>();
                p.trim();
                return p;
            }
        }
    }

    preferences.begin(PREF_NAMESPACE, false);
    String pass = preferences.getString("wifi_pass", "");
    preferences.end();
    pass.trim();
    return pass;
}

void clearAllCredentials() {
    ensureNVSInitialized();
    preferences.begin(PREF_NAMESPACE, false);
    preferences.clear();
    preferences.end();

    if (initSD() && SD.exists(CONFIG_FILE)) {
        SD.remove(CONFIG_FILE);
        Serial.println("[Storage] config.json removed from SD.");
    }
    Serial.println("[Storage] All credentials and settings cleared.");
}

String getIPAddress() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    } else {
        return "Not Connected";
    }
}

void startWebServer() {
    static bool serverStarted = false;
    if (serverStarted) return;

    // Main Hardware Debug & Remote Keyboard Console (serves on / and /chat)
    auto serveConsolePage = [](AsyncWebServerRequest *request) {
        String currentKey = getStoredApiKey();
        String currentModel = getStoredModel();
        String currentSSID = getStoredWifiSSID();
        String maskedKey = "";
        if (currentKey.length() > 8) {
            maskedKey = currentKey.substring(0, 4) + "..." + currentKey.substring(currentKey.length() - 4);
        } else if (!currentKey.isEmpty()) {
            maskedKey = "****";
        } else {
            maskedKey = "Not configured";
        }

        String html = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<title>CARDPUTER // HARDWARE CONSOLE</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
:root {
  --bg-main: #0B0E14;
  --bg-card: #121820;
  --bg-sub: #182230;
  --neon-green: #00FF9D;
  --neon-cyan: #00E5FF;
  --neon-pink: #FF0055;
  --neon-yellow: #FFB300;
  --text-main: #E2E8F0;
  --text-dim: #798E9C;
  --border-col: #202D3D;
  --font-mono: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: var(--bg-main);
  color: var(--text-main);
  font-family: var(--font-mono);
  padding: 12px;
  min-height: 100vh;
  display: flex;
  justify-content: center;
}
.console-wrapper {
  width: 100%;
  max-width: 760px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.top-header {
  background: var(--bg-card);
  border: 1px solid var(--border-col);
  border-radius: 8px;
  padding: 10px 14px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
}
.brand {
  font-size: 14px;
  font-weight: bold;
  color: var(--neon-cyan);
  letter-spacing: 1px;
  display: flex;
  align-items: center;
  gap: 8px;
}
.sys-badge {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 4px;
  background: #00ff9d15;
  color: var(--neon-green);
  border: 1px solid var(--neon-green);
}
.nav-tabs {
  display: flex;
  gap: 8px;
  background: var(--bg-card);
  padding: 6px;
  border-radius: 8px;
  border: 1px solid var(--border-col);
}
.nav-btn {
  flex: 1;
  background: transparent;
  border: 1px solid transparent;
  color: var(--text-dim);
  padding: 9px;
  border-radius: 6px;
  font-family: inherit;
  font-size: 13px;
  font-weight: bold;
  cursor: pointer;
  transition: all 0.2s;
}
.nav-btn.active {
  background: var(--bg-sub);
  color: var(--neon-cyan);
  border-color: var(--neon-cyan);
  box-shadow: 0 0 10px #00e5ff30;
}
/* HARDWARE TELEMETRY HUD */
.hud-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
  gap: 10px;
}
.hud-card {
  background: var(--bg-card);
  border: 1px solid var(--border-col);
  border-radius: 8px;
  padding: 10px;
  text-align: center;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  min-height: 95px;
}
.hud-label {
  font-size: 10px;
  text-transform: uppercase;
  color: var(--text-dim);
  margin-bottom: 6px;
  letter-spacing: 0.5px;
}
.hud-value {
  font-size: 13px;
  font-weight: bold;
  color: var(--text-main);
  margin-top: 4px;
}
.bulb-box {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  border: 2px solid rgba(255,255,255,0.2);
  transition: all 0.3s;
  display: inline-block;
}
@keyframes breatheFx { 0%{ transform:scale(0.92); opacity:0.4; } 50%{ transform:scale(1.1); opacity:1.0; } 100%{ transform:scale(0.92); opacity:0.4; } }
@keyframes blinkFx { 0%,49%{ opacity:1; } 50%,100%{ opacity:0.15; } }
@keyframes rainbowFx { 0%{ filter:hue-rotate(0deg); } 100%{ filter:hue-rotate(360deg); } }
.effect-breathe { animation: breatheFx 1.6s infinite ease-in-out; }
.effect-blink { animation: blinkFx 0.8s infinite steps(1); }
.effect-rainbow { animation: rainbowFx 3s infinite linear; background: radial-gradient(circle, #ff0055, #00e5ff) !important; }
.progress-rail {
  width: 90%;
  height: 6px;
  background: #090c10;
  border-radius: 3px;
  overflow: hidden;
  margin-top: 6px;
  border: 1px solid var(--border-col);
}
.progress-fill {
  height: 100%;
  background: var(--neon-cyan);
  transition: width 0.3s, background 0.3s;
}
/* CARDPUTER SCREEN MIRROR */
.screen-mirror-frame {
  background: #06080C;
  border: 2px solid #202D3D;
  border-radius: 10px;
  padding: 12px;
  position: relative;
  box-shadow: inset 0 0 30px rgba(0,0,0,0.8), 0 0 15px rgba(0,229,255,0.1);
}
.screen-mirror-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid #1A2433;
  padding-bottom: 6px;
  margin-bottom: 10px;
  font-size: 11px;
}
.screen-title { color: var(--neon-cyan); }
.status-pill {
  font-size: 10px;
  padding: 2px 6px;
  border-radius: 3px;
  background: #00ff9d20;
  color: var(--neon-green);
  border: 1px solid var(--neon-green);
}
.status-pill.thinking {
  background: #00e5ff20;
  color: var(--neon-cyan);
  border-color: var(--neon-cyan);
  animation: blinkFx 1s infinite;
}
.screen-content {
  height: 250px;
  overflow-y: auto;
  font-size: 13px;
  line-height: 1.5;
  padding-right: 6px;
  scrollbar-width: thin;
  scrollbar-color: #202D3D transparent;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.msg-bubble {
  max-width: 85%;
  padding: 8px 12px;
  border-radius: 8px;
  font-size: 13px;
  line-height: 1.4;
  word-break: break-word;
  white-space: pre-wrap;
}
.msg-bubble.user {
  align-self: flex-end;
  text-align: right;
  background: #182838;
  color: #FFFFFF;
  border: 1px solid #2B425B;
  border-bottom-right-radius: 2px;
  box-shadow: 0 2px 8px rgba(0,0,0,0.4);
}
.msg-bubble.gemini {
  align-self: flex-start;
  text-align: left;
  background: #0E1B28;
  color: #38B6FF;
  border: 1px solid #1C3B5B;
  border-bottom-left-radius: 2px;
  box-shadow: 0 0 10px rgba(56, 182, 255, 0.15);
}
.msg-bubble.system {
  align-self: center;
  text-align: center;
  color: var(--text-dim);
  font-size: 11px;
  font-style: italic;
}
/* REMOTE KEYBOARD INPUT */
.remote-input-area {
  background: var(--bg-card);
  border: 1px solid var(--border-col);
  border-radius: 8px;
  padding: 12px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.input-header {
  font-size: 11px;
  color: var(--neon-cyan);
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.input-row {
  display: flex;
  gap: 8px;
}
.input-field {
  flex: 1;
  background: #06080C;
  border: 1px solid var(--border-col);
  border-radius: 6px;
  padding: 12px 14px;
  color: #fff;
  font-family: inherit;
  font-size: 14px;
  outline: none;
  transition: border-color 0.2s, box-shadow 0.2s;
}
.input-field:focus {
  border-color: var(--neon-cyan);
  box-shadow: 0 0 8px #00e5ff40;
}
.send-btn {
  background: var(--neon-cyan);
  color: #06080C;
  border: none;
  border-radius: 6px;
  padding: 12px 20px;
  font-family: inherit;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
  transition: all 0.2s;
}
.send-btn:hover {
  background: #33ecff;
  box-shadow: 0 0 12px #00e5ff80;
}
.send-btn:disabled {
  background: #202D3D;
  color: var(--text-dim);
  cursor: not-allowed;
  box-shadow: none;
}
.chip-row {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}
.chip {
  background: var(--bg-sub);
  border: 1px solid var(--border-col);
  color: var(--text-main);
  border-radius: 4px;
  padding: 5px 10px;
  font-family: inherit;
  font-size: 11px;
  cursor: pointer;
  transition: all 0.2s;
}
.chip:hover {
  border-color: var(--neon-cyan);
  color: var(--neon-cyan);
}
.chip.puke-btn {
  border-color: #39FF14;
  color: #39FF14;
  background: #39ff1415;
}
.chip.puke-btn:hover {
  background: #39FF14;
  color: #000;
  box-shadow: 0 0 10px #39ff1480;
}
/* SETTINGS TAB */
.tab-pane { display: none; flex-direction: column; gap: 12px; }
.tab-pane.active { display: flex; }
.settings-card {
  background: var(--bg-card);
  border: 1px solid var(--border-col);
  border-radius: 8px;
  padding: 16px;
}
.settings-title {
  color: var(--neon-cyan);
  font-size: 14px;
  margin-bottom: 12px;
  border-bottom: 1px solid var(--border-col);
  padding-bottom: 6px;
}
.form-group { margin-bottom: 12px; }
.form-group label {
  display: block;
  font-size: 11px;
  color: var(--text-dim);
  margin-bottom: 6px;
  text-transform: uppercase;
}
.form-input {
  width: 100%;
  background: #06080C;
  border: 1px solid var(--border-col);
  border-radius: 6px;
  padding: 10px 12px;
  color: #fff;
  font-family: inherit;
  font-size: 13px;
  outline: none;
}
.form-input:focus { border-color: var(--neon-cyan); }
.submit-btn {
  width: 100%;
  background: var(--neon-green);
  color: #06080C;
  border: none;
  border-radius: 6px;
  padding: 12px;
  font-family: inherit;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
}
.restart-btn {
  width: 100%;
  background: var(--neon-pink);
  color: #fff;
  border: none;
  border-radius: 6px;
  padding: 12px;
  font-family: inherit;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
  margin-top: 8px;
}
</style>
</head>
<body>
<div class="console-wrapper">
  <div class="top-header">
    <div class="brand">⚡ M5CARDPUTER // HARDWARE CONSOLE</div>
    <div style="display:flex;gap:6px;align-items:center;">
      <span id="ipBadge" class="sys-badge">IP: )rawliteral" + getIPAddress() + R"rawliteral(</span>
      <span id="connBadge" class="sys-badge" style="border-color:var(--neon-green);">ONLINE</span>
    </div>
  </div>

  <div class="nav-tabs">
    <button class="nav-btn active" onclick="switchTab('terminal')">>_ REMOTE KEYBOARD & MIRROR</button>
    <button class="nav-btn" onclick="switchTab('settings')">⚙ HARDWARE & SETTINGS</button>
  </div>

  <!-- TAB 1: TERMINAL & HARDWARE HUD -->
  <div id="pane-terminal" class="tab-pane active">
    <div class="hud-grid">
      <div class="hud-card">
        <div class="hud-label">💡 RGB STATUS LED</div>
        <div id="ledBulb" class="bulb-box" style="background:#000;"></div>
        <div id="ledStatus" class="hud-value" style="font-size:11px;">OFF</div>
      </div>
      <div class="hud-card">
        <div class="hud-label">🖥️ DISPLAY / BACKLIGHT</div>
        <div id="screenVal" class="hud-value">--%</div>
        <div class="progress-rail"><div id="screenFill" class="progress-fill" style="width:0%;"></div></div>
      </div>
      <div class="hud-card">
        <div class="hud-label">🔋 BATTERY</div>
        <div id="batVal" class="hud-value">--%</div>
        <div class="progress-rail"><div id="batFill" class="progress-fill" style="width:0%;"></div></div>
      </div>
      <div class="hud-card">
        <div class="hud-label">📶 WI-FI / SIGNAL</div>
        <div id="wifiVal" class="hud-value">-- dBm</div>
        <div id="heapVal" style="font-size:10px;color:var(--text-dim);margin-top:4px;">RAM: -- KB</div>
      </div>
    </div>

    <div class="screen-mirror-frame">
      <div class="screen-mirror-header">
        <span class="screen-title">📟 CARDPUTER DISPLAY MIRROR (240x135)</span>
        <span id="aiStatusPill" class="status-pill">● IDLE</span>
      </div>
      <div id="screenContent" class="screen-content">Loading conversation...</div>
    </div>

    <div class="remote-input-area">
      <div class="input-header">
        <span>⌨ WIRELESS INPUT TO CARDPUTER</span>
        <span style="color:var(--text-dim);font-size:10px;">ENTER: send</span>
      </div>
      <form id="inputForm" class="input-row" onsubmit="sendRemoteInput(event)">
        <input type="text" id="cmdInput" class="input-field" placeholder="Type message or command for Cardputer..." autocomplete="off" autofocus>
        <button type="submit" id="sendBtn" class="send-btn">SEND</button>
      </form>
      <div class="chip-row">
        <button type="button" class="chip" onclick="quickSend('Turn the lamp red')">🔴 Red Lamp</button>
        <button type="button" class="chip" onclick="quickSend('Turn the lamp green')">🟢 Green Lamp</button>
        <button type="button" class="chip" onclick="quickSend('Turn the lamp blue')">🔵 Blue Lamp</button>
        <button type="button" class="chip" onclick="quickSend('Set rainbow effect on the lamp')">🌈 Rainbow</button>
        <button type="button" class="chip" onclick="quickSend('Turn off the lamp')">❌ Lamp Off</button>
        <button type="button" class="chip" onclick="quickSend('Set screen brightness to 10%')">☀️ Screen 10%</button>
        <button type="button" class="chip" onclick="quickSend('Set screen brightness to 80%')">☀️ Screen 80%</button>
        <button type="button" class="chip" onclick="quickSend('What is the battery level?')">🔋 Battery</button>
        <button type="button" class="chip puke-btn" onclick="triggerShakeEasterEgg()">🤢 SHAKE EASTER EGG</button>
      </div>
    </div>
  </div>

  <!-- TAB 2: SETTINGS & CONFIG -->
  <div id="pane-settings" class="tab-pane">
    <div class="settings-card">
      <div class="settings-title">⚙ GEMINI & WI-FI CONFIGURATION</div>
      <form action="/save" method="POST">
        <div class="form-group">
          <label>Gemini API Key</label>
          <input type="text" name="apiKey" class="form-input" placeholder="AIzaSy..." value=")rawliteral" + currentKey + R"rawliteral(">
        </div>
        <div class="form-group">
          <label>Gemini Model</label>
          <input type="text" id="modelInput" name="model" class="form-input" placeholder="gemini-3.1-flash-lite" value=")rawliteral" + currentModel + R"rawliteral(">
          <div class="chip-row" style="margin-top:8px;">
            <button type="button" class="chip" onclick="document.getElementById('modelInput').value='gemini-3.1-flash-lite'">3.1 Flash-Lite</button>
            <button type="button" class="chip" onclick="document.getElementById('modelInput').value='gemini-3.1-pro'">3.1 Pro</button>
            <button type="button" class="chip" onclick="document.getElementById('modelInput').value='gemini-3.0-flash'">3.0 Flash</button>
            <button type="button" class="chip" onclick="document.getElementById('modelInput').value='gemini-2.5-flash'">2.5 Flash</button>
          </div>
        </div>
        <div class="form-group">
          <label>Wi-Fi SSID</label>
          <input type="text" name="wifi_ssid" class="form-input" placeholder="Network Name" value=")rawliteral" + currentSSID + R"rawliteral(">
        </div>
        <div class="form-group">
          <label>Wi-Fi Password</label>
          <input type="password" name="wifi_pass" class="form-input" placeholder="••••••••">
        </div>
        <button type="submit" class="submit-btn">💾 SAVE SETTINGS</button>
      </form>
      <form action="/restart" method="POST" onsubmit="return confirm('Reboot Cardputer device?');">
        <button type="submit" class="restart-btn">🔄 REBOOT CARDPUTER</button>
      </form>
    </div>
  </div>
</div>

<script>
let cmdHistory = [];
let historyIndex = -1;

function switchTab(tab) {
  document.querySelectorAll('.nav-btn').forEach((b, i) => {
    b.classList.toggle('active', (tab === 'terminal' && i === 0) || (tab === 'settings' && i === 1));
  });
  document.getElementById('pane-terminal').classList.toggle('active', tab === 'terminal');
  document.getElementById('pane-settings').classList.toggle('active', tab === 'settings');
}

async function fetchTelemetry() {
  try {
    const res = await fetch('/api/state');
    if (!res.ok) return;
    const st = await res.json();
    
    // LED Bulb
    const bulb = document.getElementById('ledBulb');
    const ledDesc = document.getElementById('ledStatus');
    bulb.className = 'bulb-box';
    if (st.is_thinking) {
      bulb.style.background = '#0055ff';
      bulb.style.boxShadow = '0 0 20px #0055ff';
      bulb.classList.add('effect-breathe');
      ledDesc.innerText = 'THINKING...';
    } else if (st.led_effect === 'rainbow') {
      bulb.classList.add('effect-rainbow');
      bulb.style.boxShadow = '0 0 20px rgba(255,0,128,0.8)';
      ledDesc.innerText = 'RAINBOW';
    } else if (st.led_effect === 'off' || st.led_brightness === 0) {
      bulb.style.background = '#151b22';
      bulb.style.boxShadow = 'none';
      ledDesc.innerText = 'OFF';
    } else {
      const c = st.led_color || '#ffffff';
      bulb.style.background = c;
      bulb.style.boxShadow = '0 0 18px ' + c;
      if (st.led_effect === 'breathe') bulb.classList.add('effect-breathe');
      if (st.led_effect === 'blink') bulb.classList.add('effect-blink');
      const bPct = Math.round(st.led_brightness * 100 / 255);
      ledDesc.innerText = c + ' (' + bPct + '%)';
    }

    // Screen
    const sPct = Math.round(st.screen_brightness * 100 / 255);
    document.getElementById('screenVal').innerText = sPct + '%';
    document.getElementById('screenFill').style.width = sPct + '%';

    // Battery
    const bat = st.battery;
    document.getElementById('batVal').innerText = bat + '%';
    const bFill = document.getElementById('batFill');
    bFill.style.width = Math.max(5, bat) + '%';
    bFill.style.background = (bat > 40) ? 'var(--neon-green)' : ((bat > 20) ? 'var(--neon-yellow)' : 'var(--neon-pink)');

    // Wi-Fi / RSSI
    document.getElementById('wifiVal').innerText = (st.wifi_rssi || '--') + ' dBm';
    if (st.free_heap) {
      document.getElementById('heapVal').innerText = 'RAM: ' + Math.round(st.free_heap / 1024) + ' KB';
    }

    // AI Status Pill
    const pill = document.getElementById('aiStatusPill');
    if (st.is_thinking) {
      pill.innerText = '⚡ AI THINKING...';
      pill.className = 'status-pill thinking';
    } else {
      pill.innerText = '● IDLE';
      pill.className = 'status-pill';
    }
  } catch(e) {}
}

async function fetchTranscript() {
  try {
    const res = await fetch('/api/transcript');
    if (!res.ok) return;
    const data = await res.json();
    const scr = document.getElementById('screenContent');
    const prevScroll = scr.scrollHeight - scr.scrollTop <= scr.clientHeight + 30;
    
    const text = data.transcript || '';
    if (!text.trim()) {
      scr.innerHTML = '<div class="msg-bubble system">> [Ready. Start typing below or use Cardputer keyboard]</div>';
      return;
    }
    
    const lines = text.split('\n');
    let html = '';
    let currentRole = '';
    let currentText = '';
    
    const flushBubble = () => {
      if (!currentText.trim()) return;
      const clean = currentText.trim().replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
      if (currentRole === 'user') {
        html += '<div class="msg-bubble user">' + clean + '</div>';
      } else if (currentRole === 'gemini') {
        html += '<div class="msg-bubble gemini">' + clean + '</div>';
      } else {
        html += '<div class="msg-bubble system">' + clean + '</div>';
      }
      currentText = '';
    };

    for (const rawLine of lines) {
      const line = rawLine.trim();
      if (line.startsWith('> User:') || line.startsWith('> Web:') || line.startsWith('> You:')) {
        flushBubble();
        currentRole = 'user';
        currentText = line.replace(/^> (User|Web|You):\s*/, '') + '\n';
      } else if (line.startsWith('Gemini:')) {
        flushBubble();
        currentRole = 'gemini';
        currentText = line.replace(/^Gemini:\s*/, '') + '\n';
      } else {
        currentText += rawLine + '\n';
      }
    }
    flushBubble();
    
    scr.innerHTML = html;
    if (prevScroll) {
      scr.scrollTop = scr.scrollHeight;
    }
  } catch(e) {}
}

async function sendRemoteInput(e) {
  if (e) e.preventDefault();
  const inp = document.getElementById('cmdInput');
  const txt = inp.value.trim();
  if (!txt) return;

  cmdHistory.push(txt);
  historyIndex = cmdHistory.length;
  inp.value = '';
  document.getElementById('sendBtn').disabled = true;

  try {
    await fetch('/api/send_input', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'text=' + encodeURIComponent(txt)
    });
    fetchTelemetry();
    fetchTranscript();
  } catch(err) {}
  document.getElementById('sendBtn').disabled = false;
  inp.focus();
}

function quickSend(txt) {
  document.getElementById('cmdInput').value = txt;
  sendRemoteInput(null);
}

async function triggerShakeEasterEgg() {
  try {
    await fetch('/api/shake', { method: 'POST' });
  } catch(e) {}
}

// History navigation with Up/Down arrow in input
document.getElementById('cmdInput').addEventListener('keydown', (e) => {
  if (e.key === 'ArrowUp') {
    if (historyIndex > 0) {
      historyIndex--;
      e.target.value = cmdHistory[historyIndex];
    }
  } else if (e.key === 'ArrowDown') {
    if (historyIndex < cmdHistory.length - 1) {
      historyIndex++;
      e.target.value = cmdHistory[historyIndex];
    } else {
      historyIndex = cmdHistory.length;
      e.target.value = '';
    }
  }
});

setInterval(() => {
  fetchTelemetry();
  fetchTranscript();
}, 800);

fetchTelemetry();
fetchTranscript();
</script>
</body>
</html>)rawliteral";

        request->send(200, "text/html", html);
    };

    server.on("/", HTTP_GET, serveConsolePage);
    server.on("/chat", HTTP_GET, serveConsolePage);

    // Live Hardware Telemetry endpoint (/api/state)
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<512> doc;
        if (gDeviceStateProvider) {
            DeviceStateInfo st = gDeviceStateProvider();
            doc["screen_brightness"] = st.screenBrightness;
            doc["led_effect"] = st.ledEffect;
            doc["led_color"] = st.ledColorHex;
            doc["led_brightness"] = st.ledBrightness;
            doc["battery"] = st.batteryPercentage;
            doc["is_thinking"] = st.isThinking;
            doc["model"] = st.model;
            doc["wifi_rssi"] = st.wifiRssi;
            doc["free_heap"] = st.freeHeap;
            doc["uptime_sec"] = st.uptimeSec;
            doc["sd_mounted"] = st.sdMounted;
        } else {
            doc["screen_brightness"] = 180;
            doc["led_effect"] = "off";
            doc["led_color"] = "#000000";
            doc["led_brightness"] = 0;
            doc["battery"] = 100;
            doc["is_thinking"] = false;
            doc["model"] = getStoredModel();
            doc["wifi_rssi"] = WiFi.RSSI();
            doc["free_heap"] = ESP.getFreeHeap();
            doc["uptime_sec"] = millis() / 1000;
            doc["sd_mounted"] = isSDCardAvailable();
        }
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // Transcript mirror endpoint (/api/transcript)
    server.on("/api/transcript", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<4096> doc;
        if (gTranscriptProvider) {
            doc["transcript"] = gTranscriptProvider();
        } else {
            doc["transcript"] = "";
        }
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // Asynchronous non-blocking input submission (/api/send_input)
    // Feeds directly into Cardputer keyboard/input task queue without blocking async_tcp!
    server.on("/api/send_input", HTTP_POST, [](AsyncWebServerRequest *request) {
        String text = "";
        if (request->hasParam("text", true)) {
            text = request->getParam("text", true)->value();
        } else if (request->hasParam("prompt", true)) {
            text = request->getParam("prompt", true)->value();
        }
        text.trim();

        if (text.isEmpty()) {
            request->send(400, "application/json", "{\"error\":\"Empty text\"}");
            return;
        }

        bool ok = false;
        if (gRemoteInputHandler) {
            ok = gRemoteInputHandler(text);
        }

        StaticJsonDocument<256> doc;
        doc["status"] = ok ? "queued" : "busy";
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // Legacy /api/chat redirected to non-blocking remote input
    server.on("/api/chat", HTTP_POST, [](AsyncWebServerRequest *request) {
        String prompt = "";
        if (request->hasParam("prompt", true)) {
            prompt = request->getParam("prompt", true)->value();
        } else if (request->hasParam("text", true)) {
            prompt = request->getParam("text", true)->value();
        }
        prompt.trim();

        if (gRemoteInputHandler && !prompt.isEmpty()) {
            gRemoteInputHandler(prompt);
        }

        request->send(200, "application/json", "{\"status\":\"queued\"}");
    });

    // Easter egg trigger endpoint (/api/shake)
    server.on("/api/shake", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (gShakeEasterEggTrigger) {
            gShakeEasterEggTrigger();
        }
        request->send(200, "application/json", "{\"status\":\"puked\"}");
    });

    // Settings save handler (/save)
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        bool updated = false;

        if (request->hasParam("apiKey", true)) {
            String apiKey = request->getParam("apiKey", true)->value();
            if (!apiKey.isEmpty()) {
                saveApiKey(apiKey);
                updated = true;
            }
        }

        if (request->hasParam("model", true)) {
            String model = request->getParam("model", true)->value();
            if (!model.isEmpty()) {
                saveSelectedModel(model);
                updated = true;
            }
        }

        if (request->hasParam("wifi_ssid", true)) {
            String ssid = request->getParam("wifi_ssid", true)->value();
            String pass = "";
            if (request->hasParam("wifi_pass", true)) {
                pass = request->getParam("wifi_pass", true)->value();
            }
            if (!ssid.isEmpty()) {
                saveWifiCredentials(ssid, pass);
                updated = true;
            }
        }

        if (updated) {
            apiKeySavedFlag = true;
            request->redirect("/");
        } else {
            request->send(400, "text/plain", "No valid parameters provided.");
        }
    });

    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", "<!DOCTYPE html><html><body style='background:#0B0E14;color:#00FF9D;text-align:center;padding-top:50px;font-family:monospace;'><h2>Cardputer перезагружается...</h2></body></html>");
        delay(800);
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest *request){
        request->redirect("/");
    });

    server.begin();
    serverStarted = true;
    Serial.println("[WebServer] WebServer started successfully on port 80.");
}