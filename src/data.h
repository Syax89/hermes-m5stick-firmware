#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "ble_bridge.h"
#include "xfer.h"

// UI helpers defined in main.cpp — for screen refresh during blocking calls
extern TFT_eSprite spr;
void drawVoiceProcessing();

// Cooperative cancel volatile states
extern volatile bool voiceCancelRequested;
extern WiFiClientSecure* volatile activeVoiceClient;
extern HTTPClient* volatile activeVoiceHttp;
extern SemaphoreHandle_t voiceMutex;

// Hermes session continuity. Empty = start a fresh conversation (server
// derives a session id and returns it via the X-Hermes-Session-Id response
// header, which we capture here). Non-empty = continue that session: we send
// it back as the X-Hermes-Session-Id request header so Hermes reloads the
// prior turns server-side instead of spawning a new session.
extern char hermesSessionId[64];

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t tokensLifetime;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
  char     model[16];        // model name from sessions API
  char     sessLines[8][64]; // session tiles for settings > sessions
  uint8_t  sessLineCount;
  char     activeTitle[40];  // title/preview of the running session (busy panel)
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Hermes connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;
static bool     _runIsActive = false;  // tracks if runs API reported an active run

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; uint32_t tokMonth; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0,0}, {"one idle",1,0,0,false,12000,12000},
  {"busy",4,3,0,false,89000,180000}, {"attention",2,1,1,false,45000,120000},
  {"completed",1,0,0,true,142000,250000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once NTP time has been synced to the RTC.
static bool _rtcValid = false;

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_sntp.h>

// NTP time sync via pool.ntp.org. SNTP sets system clock to UTC;
// we add 7200 manually for CEST, then write to M5 hardware RTC.
static const char* NTP_SERVER = "pool.ntp.org";
static bool        _ntpStarted = false;
extern uint32_t    _clkLastRead;  // main.cpp — zero to force RTC re-read

static void _applyJson(const char* payload, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;

  const char* runStatus = doc["status"];
  out->sessionsWaiting = 0;
  out->recentlyCompleted = false;
  _runIsActive = false;

  if (runStatus) {
    if (strcmp(runStatus, "queued") == 0 || strcmp(runStatus, "in_progress") == 0 || strcmp(runStatus, "running") == 0) {
      out->sessionsRunning = 1;
      _runIsActive = true;
      strncpy(out->msg, "Running...", sizeof(out->msg) - 1);
      out->promptId[0] = 0;
      out->promptTool[0] = 0;
      out->promptHint[0] = 0;
    } else if (strcmp(runStatus, "requires_action") == 0 || strcmp(runStatus, "waiting") == 0) {
      out->sessionsWaiting = 1;
      _runIsActive = true;
      strncpy(out->msg, "Needs approval", sizeof(out->msg) - 1);

      // Parse required_action tool calls
      JsonObject requiredAction = doc["required_action"];
      if (!requiredAction.isNull()) {
        JsonArray toolCalls = requiredAction["submit_tool_outputs"]["tool_calls"];
        if (toolCalls.size() > 0) {
          JsonObject toolCall = toolCalls[0];
          const char* pid = toolCall["id"];
          const char* pt = toolCall["function"]["name"];
          const char* pargs = toolCall["function"]["arguments"];

          strncpy(out->promptId, pid ? pid : "", sizeof(out->promptId) - 1);
          out->promptId[sizeof(out->promptId) - 1] = 0;
          strncpy(out->promptTool, pt ? pt : "Tool", sizeof(out->promptTool) - 1);
          out->promptTool[sizeof(out->promptTool) - 1] = 0;

          // Try to parse command from arguments JSON
          if (pargs) {
            JsonDocument argsDoc;
            if (deserializeJson(argsDoc, pargs) == DeserializationError::Ok) {
              const char* cmd = argsDoc["command"] | argsDoc["cmd"] | argsDoc["path"] | argsDoc["content"] | argsDoc["code"] | argsDoc["query"] | pargs;
              strncpy(out->promptHint, cmd, sizeof(out->promptHint) - 1);
            } else {
              strncpy(out->promptHint, pargs, sizeof(out->promptHint) - 1);
            }
          } else {
            out->promptHint[0] = 0;
          }
          out->promptHint[sizeof(out->promptHint) - 1] = 0;
        }
      } else if (doc["prompt"]) { // Support custom prompt block
        JsonObject pr = doc["prompt"];
        const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
        strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
        strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
        strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
      }
    } else {
      strncpy(out->msg, runStatus, sizeof(out->msg) - 1);
      out->promptId[0] = 0;
      out->promptTool[0] = 0;
      out->promptHint[0] = 0;
      _runIsActive = false;
      if (strcmp(runStatus, "completed") == 0 ||
          strcmp(runStatus, "succeeded") == 0 ||
          strcmp(runStatus, "cancelled") == 0 ||
          strcmp(runStatus, "failed") == 0 ||
          strcmp(runStatus, "expired") == 0) {
        out->recentlyCompleted = true;
      }
    }
    out->msg[sizeof(out->msg) - 1] = 0;
  }

  // Parse token usage — feeds the pet level system (cumulative, stats.h)
  uint32_t bridgeTokens = doc["usage"]["total_tokens"] | 0;
  if (bridgeTokens > 0) {
    statsOnBridgeTokens(bridgeTokens);
  }

  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

static void _applySessionsDoc(JsonDocument& doc, TamaState* out) {
  JsonArray data = doc["data"];
  if (!data) {
    Serial.println("[sess] no data array");
    return;
  }

  uint8_t total = 0;
  uint8_t running = 0;
  uint32_t todayTokens = 0;
  uint32_t monthTokens = 0;
  uint32_t lifetimeTokens = 0;
  uint8_t lineCount = 0;
  out->activeTitle[0] = 0;   // cleared unless a running session is found

  time_t now = time(nullptr);
  time_t todaySec = now > 0 ? now - (now % 86400) : 0;
  time_t monthSec = now > 0 ? now - 2592000 : 0;

  for (JsonVariant session : data) {
    const char* source = session["source"];
    if (source && strcmp(source, "cron") == 0) continue;
    total++;

    if (total == 1) {
      const char* m = session["model"];
      strncpy(out->model, m ? m : "?", sizeof(out->model) - 1);
      out->model[sizeof(out->model) - 1] = 0;
    }

    if (session["ended_at"].isNull()) {
      JsonVariant lastActive = session["last_active"];
      time_t la = lastActive.isNull() ? 0 : (time_t)lastActive.as<double>();
      if (la > 0 && now > 0 && (now - la) < 60) {
        running++;
        if (running == 1) {   // label of the (first) running session
          const char* t  = session["title"];
          const char* pv = session["preview"];
          const char* lbl = (t && t[0]) ? t : (pv ? pv : "");
          strncpy(out->activeTitle, lbl, sizeof(out->activeTitle) - 1);
          out->activeTitle[sizeof(out->activeTitle) - 1] = 0;
        }
      }
    }

    uint32_t tok = session["input_tokens"].as<uint32_t>()
                 + session["output_tokens"].as<uint32_t>();
    lifetimeTokens += tok;
    time_t startEpoch = 0;
    {
      JsonVariant sa = session["started_at"];
      if (!sa.isNull()) startEpoch = (time_t)sa.as<double>();
    }
    if (startEpoch > 0) {
      if (startEpoch >= todaySec) todayTokens += tok;
      if (startEpoch >= monthSec) monthTokens += tok;
    }

    if (lineCount < 8) {
      const char* title  = session["title"];
      const char* src    = source;
      const char* preview = session["preview"];
      char sym = '?';
      if (src) {
        if (strcmp(src, "telegram") == 0) sym = 't';
        else if (strcmp(src, "tui") == 0) sym = 'U';
        else if (strcmp(src, "api_server") == 0) sym = 'A';
      }
      if (title) {
        snprintf(out->sessLines[lineCount], sizeof(out->sessLines[0]),
                 "%c %.12s", sym, title);
      } else if (preview) {
        snprintf(out->sessLines[lineCount], sizeof(out->sessLines[0]),
                 "%c %.12s", sym, preview);
      } else {
        snprintf(out->sessLines[lineCount], sizeof(out->sessLines[0]),
                 "%c session", sym);
      }
      lineCount++;
    }
  }

  out->sessLineCount = lineCount;

  out->sessionsTotal  = total;
  out->sessionsRunning = running;
  out->tokensToday    = todayTokens;
  out->tokensLifetime = lifetimeTokens;

  if (lifetimeTokens > 0) statsEnsureTokens(lifetimeTokens);

  // When sessions API confirms no run is active, override the runs message
  if (running == 0 && !out->promptId[0]) {
    strncpy(out->msg, "Idle", sizeof(out->msg) - 1);
    out->msg[sizeof(out->msg) - 1] = 0;
  }

  out->lastUpdated = millis();
  _lastLiveMs = millis();
  Serial.printf("[sess] %u total, %u running, %lu today %lu lifetime, %u lines\n",
                total, running, (unsigned long)todayTokens, (unsigned long)lifetimeTokens, lineCount);
}

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->tokensLifetime=s.tokMonth; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected != wasConnected) {
    wasConnected = isConnected;
    if (isConnected) {
      Serial.print("WiFi Connected! IP: ");
      Serial.println(WiFi.localIP());
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", NTP_SERVER);  // get CET/CEST time from NTP
      _ntpStarted = true;
      Serial.println("NTP sync started (CET/CEST)...");
    } else {
      Serial.println("WiFi Disconnected!");
      _ntpStarted = false;
    }
  }

  if (isConnected && _ntpStarted && !_rtcValid) {
    // M5.begin() copies BM8563 RTC time → system clock. That time may be
    // stale/wrong (CEST from a previous boot). Wait until SNTP has actually
    // received a response and overwritten the system clock with UTC.
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      if (now > 1700000000) {
        struct tm* lt = localtime(&now);
        RTC_TimeTypeDef rt; RTC_DateTypeDef rd;
        rt.Hours   = lt->tm_hour; rt.Minutes = lt->tm_min; rt.Seconds = lt->tm_sec;
        rd.WeekDay = lt->tm_wday; rd.Month   = lt->tm_mon + 1; rd.Date = lt->tm_mday;
        rd.Year    = lt->tm_year + 1900;
        M5.Rtc.SetTime(&rt); M5.Rtc.SetDate(&rd);
        _clkLastRead = 0;
        _rtcValid = true;
        statsOnNtpSync((uint32_t)now);
        Serial.printf("NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      rd.Year, rd.Month, rd.Date, rt.Hours, rt.Minutes, rt.Seconds);
      }
    } else {
      static uint32_t _ntpWaitLog = 0;
      if (now - _ntpWaitLog > 5000) {
        _ntpWaitLog = now;
        Serial.printf("NTP waiting... status=%d\n", sntp_get_sync_status());
      }
    }
  }

  // Periodic NTP re-sync every 6 hours to keep drift in check
  if (_rtcValid && isConnected) {
    static uint32_t _lastNtpResync = 0;
    if (now - _lastNtpResync > 21600000) {  // 6h
      _lastNtpResync = now;
      time_t tnow = time(nullptr);
      if (tnow > 1700000000) {
        struct tm* lt = localtime(&tnow);
        RTC_TimeTypeDef rt; RTC_DateTypeDef rd;
        rt.Hours   = lt->tm_hour; rt.Minutes = lt->tm_min; rt.Seconds = lt->tm_sec;
        rd.WeekDay = lt->tm_wday; rd.Month   = lt->tm_mon + 1; rd.Date = lt->tm_mday;
        rd.Year    = lt->tm_year + 1900;
        M5.Rtc.SetTime(&rt); M5.Rtc.SetDate(&rd);
        _clkLastRead = 0;
        Serial.println("NTP resync OK");
      }
    }
  }

  if (!isConnected) {
    out->connected = false;
    strncpy(out->msg, "No Wi-Fi", sizeof(out->msg) - 1);
    out->msg[sizeof(out->msg) - 1] = 0;
    static uint32_t lastReconnect = 0;
    if (now - lastReconnect > 10000) {
      lastReconnect = now;
      Serial.println("WiFi reconnect...");
      WiFi.reconnect();
    }
    return;
  }

  static uint32_t lastPollMs = 0;
  if (now - lastPollMs < 3000 && lastPollMs != 0) {
    out->connected = dataConnected();
    return;
  }
  lastPollMs = now;

  bool prevRunActive = _runIsActive;

  HTTPClient http;
  {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/v1/runs/current",
             settings().hermesIp, settings().hermesPort);
    http.begin(url);
  }
  http.addHeader("Content-Type", "application/json");
  {
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", settings().hermesKey);
    http.addHeader("Authorization", auth);
  }
  http.setTimeout(2500); // 2.5 second timeout

  int httpCode = http.GET();
  if (httpCode == 200) {
    static char _pollBuf[2048];
    WiFiClient* stream = http.getStreamPtr();
    size_t len = 0;
    if (stream) {
      len = stream->readBytes(_pollBuf, sizeof(_pollBuf) - 1);
      _pollBuf[len] = 0;
    }
    Serial.printf("Poll OK (200), len=%d\n", len);
    _applyJson(_pollBuf, out);
    out->connected = true;
  } else if (httpCode == 404) {
    Serial.println("Poll: Idle (404)");
    out->connected = true;
    out->sessionsWaiting = 0;
    out->recentlyCompleted = prevRunActive;
    out->promptId[0] = 0;
    out->promptTool[0] = 0;
    out->promptHint[0] = 0;
    if (out->sessionsRunning > 0) {
      strncpy(out->msg, "Working...", sizeof(out->msg) - 1);
    } else {
      strncpy(out->msg, "idle", sizeof(out->msg) - 1);
    }
    out->msg[sizeof(out->msg) - 1] = 0;
    out->lastUpdated = millis();
    _lastLiveMs = millis();
  } else {
    out->connected = dataConnected();
    if (httpCode > 0) {
      Serial.printf("Poll Error HTTP: %d\n", httpCode);
      snprintf(out->msg, sizeof(out->msg), "HTTP: %d", httpCode);
    } else {
      Serial.println("Poll Error: Connection Fail");
      strncpy(out->msg, "HTTP Connection Fail", sizeof(out->msg) - 1);
    }
    out->msg[sizeof(out->msg) - 1] = 0;
  }
  http.end();

  // --- Sessions poll (every 15 s) ---
  static uint32_t lastSessPollMs = 0;
  static bool _sessFirstDone = false;
  uint32_t sessInterval = _sessFirstDone ? 15000 : 5000;
  if (now - lastSessPollMs >= sessInterval) {
    lastSessPollMs = now;
    _sessFirstDone = true;
    HTTPClient http2;
    {
      char url[128];
      snprintf(url, sizeof(url), "http://%s:%u/api/sessions",
               settings().hermesIp, settings().hermesPort);
      http2.begin(url);
    }
    http2.addHeader("Content-Type", "application/json");
    {
      char auth[96];
      snprintf(auth, sizeof(auth), "Bearer %s", settings().hermesKey);
      http2.addHeader("Authorization", auth);
    }
    http2.setTimeout(3000);

    int code = http2.GET();
    if (code == 200) {
      WiFiClient* stream = http2.getStreamPtr();
      if (stream) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, *stream);
        if (!err) {
          _applySessionsDoc(doc, out);
        } else {
          Serial.printf("[sess] parse error: %s\n", err.c_str());
        }
      }
    } else {
      Serial.printf("Sessions HTTP: %d\n", code);
    }
    http2.end();
  }
}

inline bool sendApproval(const char* decision) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/v1/runs/current/approval",
             settings().hermesIp, settings().hermesPort);
    http.begin(url);
  }
  http.addHeader("Content-Type", "application/json");
  {
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", settings().hermesKey);
    http.addHeader("Authorization", auth);
  }
  http.setTimeout(4000); // 4 second timeout

  char payload[64];
  snprintf(payload, sizeof(payload), "{\"choice\":\"%s\"}", decision);

  int httpCode = http.POST(payload);
  http.end();

  return (httpCode >= 200 && httpCode < 300);
}

// ===== Audio → Groq STT → Hermes chat =====
inline bool sendAudioChat(int16_t* pcmData, size_t pcmSamples, uint32_t sampleRate,
                          char* responseOut, size_t responseLen,
                          char* transcriptOut, size_t transcriptLen,
                          char* errorOut, size_t errorLen) {
  uint32_t totalStart = millis();
  
  auto logMem = [](const char* label) {
    Serial.printf("[voice-mem] %s -> Free Heap: %u B, Max Block: %u B, Internal Free: %u B\n",
                  label,
                  (unsigned)xPortGetFreeHeapSize(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  };
  
  logMem("Start sendAudioChat");

  if (voiceCancelRequested) return false;

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(errorOut, errorLen, "WiFi offline");
    Serial.println("[voice-error] WiFi offline");
    return false;
  }

  // Audio Amplitude Diagnostics (RMS and Peak-to-Peak over entire buffer)
  int64_t sum = 0;
  int64_t sqSum = 0;
  int16_t vmin = 32767, vmax = -32768;
  for (size_t i = 0; i < pcmSamples; i++) {
    int16_t s = pcmData[i];
    sum += s;
    sqSum += (int32_t)s * s;
    if (s < vmin) vmin = s;
    if (s > vmax) vmax = s;
  }
  double mean = (double)sum / pcmSamples;
  double rms = sqrt((double)sqSum / pcmSamples);
  Serial.printf("[voice-diag] Samples: %d, Min: %d, Max: %d, Mean: %.2f, RMS: %.2f\n", 
                (int)pcmSamples, vmin, vmax, mean, rms);

  // DC Offset Removal
  int16_t dc = (int16_t)(sum / (int64_t)pcmSamples);
  for (size_t i = 0; i < pcmSamples; i++) pcmData[i] -= dc;
  Serial.printf("[voice-diag] DC offset of %d removed from audio\n", dc);

  const char* boundary = "----HermesVoiceBoundary";
  uint32_t dataSize = (uint32_t)pcmSamples * 2;

  uint8_t wav[44];
  memcpy(wav, "RIFF", 4); uint32_t rs = 36 + dataSize; memcpy(wav + 4, &rs, 4);
  memcpy(wav + 8, "WAVE", 4); memcpy(wav + 12, "fmt ", 4);
  uint32_t fsz = 16; memcpy(wav + 16, &fsz, 4);
  uint16_t af = 1, ch = 1; memcpy(wav + 20, &af, 2); memcpy(wav + 22, &ch, 2);
  memcpy(wav + 24, &sampleRate, 4); uint32_t br = sampleRate * 2; memcpy(wav + 28, &br, 4);
  uint16_t ba = 2, bp = 16; memcpy(wav + 32, &ba, 2); memcpy(wav + 34, &bp, 2);
  memcpy(wav + 36, "data", 4); memcpy(wav + 40, &dataSize, 4);

  char prefix[192]; int pfLen = snprintf(prefix, sizeof(prefix),
    "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stream.wav\"\r\nContent-Type: audio/wav\r\n\r\n", boundary);
  char suffix[192]; int sfLen = snprintf(suffix, sizeof(suffix),
    "\r\n--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-large-v3-turbo\r\n--%s--\r\n", boundary, boundary);

  size_t bodySize = (size_t)pfLen + 44 + (size_t)dataSize + (size_t)sfLen;

  // In-place structure
  uint8_t* body = (uint8_t*)pcmData - 44 - pfLen;
  memcpy((uint8_t*)pcmData - 44, wav, 44);
  memcpy(body, prefix, pfLen);
  memcpy((uint8_t*)pcmData + dataSize, suffix, sfLen);

  snprintf(errorOut, errorLen, "Connecting...");
  
  int httpCode = 0;
  String respBody;
  uint32_t tSttStart = millis();
  {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20); // 20s TCP/SSL timeout for connect/write/read
    HTTPClient http;
    
    // Register active pointers for cooperative cancellation
    if (voiceMutex && xSemaphoreTake(voiceMutex, portMAX_DELAY) == pdTRUE) {
      activeVoiceClient = &client;
      activeVoiceHttp = &http;
      xSemaphoreGive(voiceMutex);
    }
    
    if (voiceCancelRequested) {
      if (voiceMutex && xSemaphoreTake(voiceMutex, portMAX_DELAY) == pdTRUE) {
        activeVoiceClient = nullptr;
        activeVoiceHttp = nullptr;
        xSemaphoreGive(voiceMutex);
      }
      return false;
    }
    
    http.begin(client, "https://api.groq.com/openai/v1/audio/transcriptions");
    char ct[96]; snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);
    http.addHeader("Content-Type", ct);
    {
      char auth[160];
      snprintf(auth, sizeof(auth), "Bearer %s", settings().groqKey);
      http.addHeader("Authorization", auth);
    }
    http.setTimeout(20000); // 20s timeout for Groq STT
    
    snprintf(errorOut, errorLen, "Sending...");
    
    uint32_t tPostStart = millis();
    httpCode = http.POST(body, bodySize);
    uint32_t tPostDuration = millis() - tPostStart;
    Serial.printf("[voice-time] Groq POST took %u ms, httpCode = %d\n", tPostDuration, httpCode);
    
    if (httpCode >= 200 && httpCode < 300) {
      if (!voiceCancelRequested) {
        snprintf(errorOut, errorLen, "Receiving...");
        respBody = http.getString();
        Serial.printf("[voice-net] Groq response body length: %u bytes\n", (unsigned)respBody.length());
      }
    } else {
      Serial.printf("[voice-error] Groq STT failed, HTTP code: %d\n", httpCode);
    }
    http.end();
    
    if (voiceMutex && xSemaphoreTake(voiceMutex, portMAX_DELAY) == pdTRUE) {
      activeVoiceClient = nullptr;
      activeVoiceHttp = nullptr;
      xSemaphoreGive(voiceMutex);
    }
  }
  uint32_t tSttTotal = millis() - tSttStart;
  Serial.printf("[voice-time] Total Groq STT transaction: %u ms\n", tSttTotal);

  if (voiceCancelRequested) return false;

  // Free the recording buffer dynamically and early!
  extern int16_t* recBuffer;
  if (recBuffer) {
    free(recBuffer);
    recBuffer = nullptr;
    Serial.println("[voice] freed recBuffer after sending payload");
  }
  logMem("After freeing recBuffer");

  char transcript[512] = "";
  if (httpCode >= 200 && httpCode < 300 && respBody.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, respBody) == DeserializationError::Ok) {
      const char* txt = doc["text"];
      if (txt && txt[0]) { 
        strncpy(transcript, txt, sizeof(transcript) - 1); 
        transcript[sizeof(transcript) - 1] = 0; 
        Serial.printf("[voice-stt] Transcript: %s\n", transcript);
      } else {
        Serial.println("[voice-error] Groq JSON has empty or missing text field");
      }
    } else {
      Serial.println("[voice-error] Failed to deserialize Groq response JSON");
    }
  }

  if (!transcript[0]) {
    snprintf(errorOut, errorLen, "STT:%d", httpCode ? httpCode : -1);
    return false;
  }

  if (voiceCancelRequested) return false;

  strncpy(transcriptOut, transcript, transcriptLen - 1);
  transcriptOut[transcriptLen - 1] = 0;
  snprintf(errorOut, errorLen, "Thinking...");

  uint32_t tChatStart = millis();
  bool chatOk = false;
  {
    char url[128]; snprintf(url, sizeof(url), "http://%s:%u/v1/chat/completions",
      settings().hermesIp, settings().hermesPort);
    char json[640]; int jLen = snprintf(json, sizeof(json),
      "{\"model\":\"hermes-agent\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", transcript);
    
    if (jLen < 0 || jLen >= (int)sizeof(json)) {
      snprintf(errorOut, errorLen, "Her: body too long");
      Serial.println("[voice-error] Hermes completion payload too long");
      return false;
    }
    
    HTTPClient http; 
    if (voiceMutex && xSemaphoreTake(voiceMutex, portMAX_DELAY) == pdTRUE) {
      activeVoiceHttp = &http;
      xSemaphoreGive(voiceMutex);
    }
    
    if (voiceCancelRequested) {
      if (voiceMutex && xSemaphoreTake(voiceMutex, portMAX_DELAY) == pdTRUE) {
        activeVoiceHttp = nullptr;
        xSemaphoreGive(voiceMutex);
      }
      return false;
    }
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    {
      char auth[96];
      snprintf(auth, sizeof(auth), "Bearer %s", settings().hermesKey);
      http.addHeader("Authorization", auth);
    }
    // Session continuity: on a follow-up we already hold an id, so ask Hermes
    // to reload that session's history server-side. On the first turn we send
    // nothing and let the server mint the id (captured from the response).
    if (hermesSessionId[0]) {
      http.addHeader("X-Hermes-Session-Id", hermesSessionId);
      Serial.printf("[voice-net] continuing Hermes session %s\n", hermesSessionId);
    }
    static const char* kSessHdr = "X-Hermes-Session-Id";
    http.collectHeaders(&kSessHdr, 1);
    http.setTimeout(45000); // 45s timeout for local model completions

    Serial.printf("[voice-net] Sending completions request to Hermes: %s\n", url);
    uint32_t tPostChat = millis();
    int code = http.POST((uint8_t*)json, (size_t)jLen);
    uint32_t tPostChatDur = millis() - tPostChat;
    Serial.printf("[voice-time] Hermes completions POST took %u ms, httpCode = %d\n", tPostChatDur, code);
    
    if (code >= 200 && code < 300) {
      // Remember the session id Hermes assigned (or echoed) so the next
      // double-A follow-up continues this same conversation.
      String sid = http.header("X-Hermes-Session-Id");
      if (sid.length() > 0 && sid.length() < sizeof(hermesSessionId)) {
        strncpy(hermesSessionId, sid.c_str(), sizeof(hermesSessionId) - 1);
        hermesSessionId[sizeof(hermesSessionId) - 1] = 0;
        Serial.printf("[voice-net] Hermes session id = %s\n", hermesSessionId);
      }
      if (!voiceCancelRequested) {
        String resp = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok) {
          const char* content = doc["choices"][0]["message"]["content"];
          if (content) { 
            strncpy(responseOut, content, responseLen - 1); 
            responseOut[responseLen - 1] = 0; 
            chatOk = true; 
            Serial.printf("[voice-chat] Response: %s\n", responseOut);
          } else {
            snprintf(errorOut, errorLen, "Her: empty");
            Serial.println("[voice-error] Hermes completions returned empty choices/content");
          }
        } else {
          snprintf(errorOut, errorLen, "Her: bad JSON");
          Serial.println("[voice-error] Failed to deserialize Hermes completions JSON");
        }
      }
    } else {
      snprintf(errorOut, errorLen, "Her: HTTP %d", code);
      Serial.printf("[voice-error] Hermes completions request failed with HTTP code %d\n", code);
    }
    http.end();
    
    if (voiceMutex && xSemaphoreTake(voiceMutex, portMAX_DELAY) == pdTRUE) {
      activeVoiceHttp = nullptr;
      xSemaphoreGive(voiceMutex);
    }
  }
  
  uint32_t tChatTotal = millis() - tChatStart;
  uint32_t totalDuration = millis() - totalStart;
  Serial.printf("[voice-time] Hermes completion transaction: %u ms\n", tChatTotal);
  Serial.printf("[voice-time] Total sendAudioChat duration: %u ms\n", totalDuration);
  
  logMem("End sendAudioChat");
  return chatOk;
}


inline bool sendStop() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/v1/runs/current/stop",
             settings().hermesIp, settings().hermesPort);
    http.begin(url);
  }
  http.addHeader("Content-Type", "application/json");
  {
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", settings().hermesKey);
    http.addHeader("Authorization", auth);
  }
  http.setTimeout(4000);

  int httpCode = http.POST("");
  http.end();

  return (httpCode >= 200 && httpCode < 300);
}

// sessionsLineCount and sessionsLine are now accessed via TamaState.

