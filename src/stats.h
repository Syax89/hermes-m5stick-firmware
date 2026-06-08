#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
extern SemaphoreHandle_t nvsMutex;

// Header-only with file-static state: include from exactly one translation
// unit (main.cpp). Including from a second .cpp produces duplicate symbols.

// Persistent stats backed by NVS. Load once at boot; save sparingly
// (NVS sectors have ~100K write cycles). We save on significant events
// only — approval, denial, nap end — never on a timer.

static const uint32_t TOKENS_PER_LEVEL = 50000;

struct Stats {
  uint32_t napSeconds;       // cumulative face-down time
  uint16_t approvals;
  uint16_t denials;
  uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  level;
  uint32_t tokens;          // cumulative output tokens, drives level
  uint8_t  decisions[8];     // rolling decisions buffer (1 = approval, 0 = denial)
  uint8_t  decIdx;
  uint8_t  decCount;
  uint32_t lastUseSec;
  uint32_t lastUseMs;
};

inline Stats _stats;
inline Preferences _prefs;
inline bool _dirty = false;
inline uint32_t _lastNapEndSec = 0;
inline uint8_t  _energyAtNap  = 3;
inline uint32_t _lastUseSec    = 0;
inline uint32_t _lastUseMs     = 0;

inline SemaphoreHandle_t statsMutex = NULL;

inline void statsLoad() {
  if (!statsMutex) {
    statsMutex = xSemaphoreCreateMutex();
  }
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
      _prefs.begin("buddy", true);
      _stats.napSeconds = _prefs.getUInt("nap", 0);
      _stats.approvals  = _prefs.getUShort("appr", 0);
      _stats.denials    = _prefs.getUShort("deny", 0);
      _stats.velIdx     = _prefs.getUChar("vidx", 0);
      _stats.velCount   = _prefs.getUChar("vcnt", 0);
      _stats.level      = _prefs.getUChar("lvl", 0);
      _stats.tokens     = _prefs.getUInt("tok", 0);
      size_t got = _prefs.getBytes("vel", _stats.velocity, sizeof(_stats.velocity));
      if (got != sizeof(_stats.velocity)) memset(_stats.velocity, 0, sizeof(_stats.velocity));
      
      size_t gotDec = _prefs.getBytes("dec", _stats.decisions, sizeof(_stats.decisions));
      if (gotDec != sizeof(_stats.decisions)) {
        memset(_stats.decisions, 1, sizeof(_stats.decisions)); // default to approvals (1)
      }
      _stats.decIdx     = _prefs.getUChar("didx", 0);
      _stats.decCount   = _prefs.getUChar("dcnt", 0);
      _lastNapEndSec    = _prefs.getUInt("nend", 0);
      _energyAtNap      = _prefs.getUChar("enap", 3);
      _lastUseSec       = _prefs.getUInt("luse", 0);
      _prefs.end();
      xSemaphoreGive(nvsMutex);
    }
    
    _lastUseMs = millis();
    
    // Initialize _lastNapEndSec if not found in NVS
    if (_lastNapEndSec == 0) {
      uint32_t nowSec = time(nullptr);
      _lastNapEndSec = (nowSec > 1700000000) ? nowSec : 1700000000;
    }
    
    // Initialize _lastUseSec if not found in NVS
    if (_lastUseSec == 0) {
      uint32_t nowSec = time(nullptr);
      _lastUseSec = (nowSec > 1700000000) ? nowSec : 1700000000;
    }
    
    // Level is derived from tokens; if NVS has level set but tokens at 0,
    // backfill so the derivation holds.
    if (_stats.tokens == 0 && _stats.level > 0) {
      _stats.tokens = (uint32_t)_stats.level * TOKENS_PER_LEVEL;
    }
    xSemaphoreGive(statsMutex);
  }
}

inline void statsSave() {
  if (!_dirty) return;
  Stats tempStats;
  uint32_t tempLastNapEndSec = 0;
  uint8_t tempEnergyAtNap = 3;
  uint32_t tempLastUseSec = 0;
  bool dirtyCopy = false;
  
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    tempStats = _stats;
    tempLastNapEndSec = _lastNapEndSec;
    tempEnergyAtNap = _energyAtNap;
    tempLastUseSec = _lastUseSec;
    dirtyCopy = _dirty;
    _dirty = false;
    xSemaphoreGive(statsMutex);
  }
  
  if (!dirtyCopy) return;

  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", false);
    _prefs.putUInt("nap", tempStats.napSeconds);
    _prefs.putUShort("appr", tempStats.approvals);
    _prefs.putUShort("deny", tempStats.denials);
    _prefs.putUChar("vidx", tempStats.velIdx);
    _prefs.putUChar("vcnt", tempStats.velCount);
    _prefs.putUChar("lvl", tempStats.level);
    _prefs.putUInt("tok", tempStats.tokens);
    _prefs.putBytes("vel", tempStats.velocity, sizeof(tempStats.velocity));
    _prefs.putBytes("dec", tempStats.decisions, sizeof(tempStats.decisions));
    _prefs.putUChar("didx", tempStats.decIdx);
    _prefs.putUChar("dcnt", tempStats.decCount);
    _prefs.putUInt("nend", tempLastNapEndSec);
    _prefs.putUChar("enap", tempEnergyAtNap);
    _prefs.putUInt("luse", tempLastUseSec);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

// Level is token-driven now; approvals only feed mood/velocity.
inline void statsOnApproval(uint32_t secondsToRespond) {
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    _stats.approvals++;
    _stats.velocity[_stats.velIdx] = (uint16_t)min(secondsToRespond, 65535u);
    _stats.velIdx = (_stats.velIdx + 1) % 8;
    if (_stats.velCount < 8) _stats.velCount++;
    
    // Add to rolling decisions buffer (1 = approval)
    _stats.decisions[_stats.decIdx] = 1;
    _stats.decIdx = (_stats.decIdx + 1) % 8;
    if (_stats.decCount < 8) _stats.decCount++;
    
    // Update last usage timestamp
    uint32_t nowSec = time(nullptr);
    _lastUseSec = (nowSec > 1700000000) ? nowSec : 1700000000;
    _lastUseMs = millis();
    
    _dirty = true;
    xSemaphoreGive(statsMutex);
  }
  statsSave();
}

// Tokens feed the pet. 50K per level, 5K per pip on the fed bar.
// Bridge sends cumulative since its start; we add the delta. A drop means
// the bridge restarted — resync without adding, don't lose NVS progress.
inline uint32_t _lastBridgeTokens = 0;
inline bool _tokensSynced = false;       // first-sight latch — see below
inline bool _levelUpPending = false;

inline void statsOnBridgeTokens(uint32_t bridgeTotal) {
  // The bridge sends its cumulative total since IT started. We track deltas.
  // Bridge restart → number drops → resync. But on DEVICE reboot,
  // _lastBridgeTokens is back to 0 while the bridge's total isn't — first
  // packet would re-credit the entire session. Latch on first sight instead.
  if (!_tokensSynced) {
    _lastBridgeTokens = bridgeTotal;
    _tokensSynced = true;
    return;
  }
  if (bridgeTotal < _lastBridgeTokens) {
    _lastBridgeTokens = bridgeTotal;     // bridge restarted
    return;
  }
  uint32_t delta = bridgeTotal - _lastBridgeTokens;
  _lastBridgeTokens = bridgeTotal;
  if (delta == 0) return;

  bool triggerSave = false;
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    uint8_t lvlBefore = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
    _stats.tokens += delta;
    uint8_t lvlAfter = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
    
    // Update last usage timestamp since tokens flowed (active usage)
    uint32_t nowSec = time(nullptr);
    _lastUseSec = (nowSec > 1700000000) ? nowSec : 1700000000;
    _lastUseMs = millis();
    
    _dirty = true; // Always set dirty so token increments will be saved during idle times

    // Heartbeats are timer-driven telemetry — don't wear NVS on every delta.
    // Tokens accumulate in RAM, persist only on the milestone. Worst case on
    // hard power-off: lose up to 50K tokens of progress.
    if (lvlAfter > lvlBefore) {
      _stats.level = lvlAfter;
      _levelUpPending = true;
      triggerSave = true;
    }
    xSemaphoreGive(statsMutex);
  }
  if (triggerSave) {
    statsSave();
  }
}

inline bool statsPollLevelUp() {
  bool r = false;
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    r = _levelUpPending;
    _levelUpPending = false;
    xSemaphoreGive(statsMutex);
  }
  return r;
}

inline void statsEnsureTokens(uint32_t minTokens) {
  bool triggerSave = false;
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    if (minTokens > _stats.tokens) {
      _stats.tokens = minTokens;
      _stats.level = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
      
      // Update last usage timestamp (since tokens increased)
      uint32_t nowSec = time(nullptr);
      _lastUseSec = (nowSec > 1700000000) ? nowSec : 1700000000;
      _lastUseMs = millis();
      
      _dirty = true;
      triggerSave = true;
    }
    xSemaphoreGive(statsMutex);
  }
  if (triggerSave) {
    statsSave();
  }
}

inline void statsOnDenial() {
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    _stats.denials++;
    
    // Add to rolling decisions buffer (0 = denial)
    _stats.decisions[_stats.decIdx] = 0;
    _stats.decIdx = (_stats.decIdx + 1) % 8;
    if (_stats.decCount < 8) _stats.decCount++;
    
    // Update last usage timestamp
    uint32_t nowSec = time(nullptr);
    _lastUseSec = (nowSec > 1700000000) ? nowSec : 1700000000;
    _lastUseMs = millis();
    
    _dirty = true;
    xSemaphoreGive(statsMutex);
  }
  statsSave();
}

inline void statsOnNapEnd(uint32_t seconds) {
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    _stats.napSeconds += seconds;
    _dirty = true;
    xSemaphoreGive(statsMutex);
  }
  statsSave();
}

// Median of the velocity ring buffer. 0 if empty.
inline uint16_t statsMedianVelocity() {
  uint16_t tmp[8];
  uint8_t n = 0;
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    n = _stats.velCount;
    if (n > 0) {
      memcpy(tmp, _stats.velocity, sizeof(tmp));
    }
    xSemaphoreGive(statsMutex);
  }
  if (n == 0) return 0;
  // insertion sort, n ≤ 8
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = tmp[i]; int8_t j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = k;
  }
  return tmp[n/2];
}

// 0..4 tier. Decays based on time elapsed since last Hermes usage.
inline uint8_t statsMoodTier() {
  uint32_t elapsedSec = 0;
  
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    uint32_t nowSec = time(nullptr);
    if (nowSec > 1700000000 && _lastUseSec > 1700000000) {
      if (nowSec >= _lastUseSec) {
        elapsedSec = nowSec - _lastUseSec;
      } else {
        elapsedSec = 0;
      }
    } else {
      elapsedSec = (millis() - _lastUseMs) / 1000;
    }
    xSemaphoreGive(statsMutex);
  }
  
  // Tiers based on elapsed inactivity:
  // <= 2 ore (7200s) -> 4 cuori
  // <= 8 ore (28800s) -> 3 cuori
  // <= 24 ore (86400s) -> 2 cuori
  // <= 48 ore (172800s) -> 1 cuore
  // > 48 ore -> 0 cuori
  if (elapsedSec <= 7200) return 4;
  if (elapsedSec <= 28800) return 3;
  if (elapsedSec <= 86400) return 2;
  if (elapsedSec <= 172800) return 1;
  return 0;
}

// Energy: starts at 3/5 on boot, tops up to full on nap end, drains 1 tier per 2h.

inline void statsOnWake() {
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    uint32_t nowSec = time(nullptr);
    _lastNapEndSec = (nowSec > 1700000000) ? nowSec : 1700000000;
    _energyAtNap = 5;
    _dirty = true;
    xSemaphoreGive(statsMutex);
  }
  statsSave();
}

inline void statsOnNtpSync(uint32_t nowSec) {
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    if (_lastUseSec == 1700000000) {
      _lastUseSec = nowSec;
      _dirty = true;
    }
    if (_lastNapEndSec == 1700000000) {
      _lastNapEndSec = nowSec;
      _dirty = true;
    }
    xSemaphoreGive(statsMutex);
  }
  statsSave();
}

inline uint8_t statsEnergyTier() {
  uint32_t hoursSince = 0;
  int8_t e = 3;
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    uint32_t nowSec = time(nullptr);
    if (nowSec > 1700000000 && _lastNapEndSec > 1700000000) {
      if (nowSec >= _lastNapEndSec) {
        hoursSince = (nowSec - _lastNapEndSec) / 3600;
      } else {
        hoursSince = 0;
      }
      e = (int8_t)_energyAtNap - (int8_t)(hoursSince / 2);
    } else {
      hoursSince = millis() / 3600000;
      e = (int8_t)_energyAtNap - (int8_t)(hoursSince / 2);
    }
    xSemaphoreGive(statsMutex);
  }
  if (e < 0) e = 0; if (e > 5) e = 5;
  return (uint8_t)e;
}

inline uint8_t statsFedProgress() {
  uint32_t tokens = 0;
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    tokens = _stats.tokens;
    xSemaphoreGive(statsMutex);
  }
  return (uint8_t)((tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

inline void statsGetSnapshot(Stats* out) {
  if (statsMutex && xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    *out = _stats;
    out->lastUseSec = _lastUseSec;
    out->lastUseMs = _lastUseMs;
    xSemaphoreGive(statsMutex);
  }
}

// --- Settings --------------------------------------------------------------

struct Settings {
  bool sound;
  bool led;
  uint8_t clockRot;  // 0=auto 1=portrait 2=landscape
  char hermesIp[64];
  uint16_t hermesPort;
  char wifiSsid[33];
  char wifiPass[64];
  char groqKey[128];
  char hermesKey[64];
  bool configured;
};

static Settings _settings = { true, true, 0, "192.168.1.100", 8642, "", "", "", "", false };

inline void settingsLoad() {
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", true);
    _settings.sound = _prefs.getBool("s_snd", true);
    _settings.led   = _prefs.getBool("s_led", true);
    _settings.clockRot = _prefs.getUChar("s_crot", 0);
    if (_settings.clockRot > 2) _settings.clockRot = 0;
    _prefs.getString("s_ip", _settings.hermesIp, sizeof(_settings.hermesIp));
    if (_settings.hermesIp[0] == '\0') strcpy(_settings.hermesIp, "192.168.1.100");
    _settings.hermesPort = _prefs.getUShort("s_port", 8642);
    _prefs.getString("s_ssid", _settings.wifiSsid, sizeof(_settings.wifiSsid));
    _prefs.getString("s_pass", _settings.wifiPass, sizeof(_settings.wifiPass));
    _prefs.getString("s_groq", _settings.groqKey, sizeof(_settings.groqKey));
    _prefs.getString("s_hkey", _settings.hermesKey, sizeof(_settings.hermesKey));
    _settings.configured = _prefs.getBool("s_cfg", false);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

inline void settingsSave() {
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", false);
    _prefs.putBool("s_snd", _settings.sound);
    _prefs.putBool("s_led", _settings.led);
    _prefs.putUChar("s_crot", _settings.clockRot);
    _prefs.putString("s_ip", _settings.hermesIp);
    _prefs.putUShort("s_port", _settings.hermesPort);
    _prefs.putString("s_ssid", _settings.wifiSsid);
    _prefs.putString("s_pass", _settings.wifiPass);
    _prefs.putString("s_groq", _settings.groqKey);
    _prefs.putString("s_hkey", _settings.hermesKey);
    _prefs.putBool("s_cfg", _settings.configured);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

inline char _petName[24] = "Buddy";
inline char _ownerName[32] = "";

inline void petNameLoad() {
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", true);
    _prefs.getString("petname", _petName, sizeof(_petName));
    _prefs.getString("owner", _ownerName, sizeof(_ownerName));
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

// Strip JSON-breaking chars — these names go into a printf'd JSON string
// unescaped (xfer.h status response). A quote persists to NVS and breaks
// the status endpoint until the name is re-set.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c != '"' && c != '\\' && c >= 0x20) dst[j++] = c;
  }
  dst[j] = 0;
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", false);
    _prefs.putString("petname", _petName);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

inline const char* petName() { return _petName; }

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", false);
    _prefs.putString("owner", _ownerName);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

inline const char* ownerName() { return _ownerName; }

inline uint8_t speciesIdxLoad() {
  uint8_t v = 0xFF;
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", true);
    v = _prefs.getUChar("species", 0xFF);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", false);
    _prefs.putUChar("species", idx);
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
}

inline Settings& settings() { return _settings; }

inline const Stats& stats() { return _stats; }
