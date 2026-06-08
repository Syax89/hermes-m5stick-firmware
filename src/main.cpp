#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <cmath>
#include <stdarg.h>
#include <esp_system.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "setup_wizard.h"

const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "SLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
// Landscape double-buffer for the busy/clock panels. Direct-to-LCD drawing in
// landscape flickers (no buffer); this 240×135 sprite lets us composite a
// frame off-screen and push it in one shot, just like the portrait `spr`.
// Created lazily on entering landscape, freed on leaving / before recording.
TFT_eSprite landSpr = TFT_eSprite(&M5.Lcd);

// Advertise as "Hermes-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Hermes";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Hermes-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
const int LED_PIN = 19;          // red LED, active-high

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

SemaphoreHandle_t nvsMutex = NULL;
SemaphoreHandle_t tamaMutex = NULL;
SemaphoreHandle_t voiceMutex = NULL;

enum VoiceState { VOICE_NONE, VOICE_RECORDING, VOICE_PROCESSING, VOICE_RESPONSE, VOICE_ERROR };
volatile VoiceState voiceState = VOICE_NONE;

TamaState    tama;
TamaState    networkTama;

static void networkTask(void* pvParameters) {
  for (;;) {
    if (voiceState == VOICE_NONE) {
      TamaState tempTama;
      if (tamaMutex && xSemaphoreTake(tamaMutex, portMAX_DELAY) == pdTRUE) {
        tempTama = networkTama;
        xSemaphoreGive(tamaMutex);
      }
      dataPoll(&tempTama);
      if (tamaMutex && xSemaphoreTake(tamaMutex, portMAX_DELAY) == pdTRUE) {
        networkTama = tempTama;
        xSemaphoreGive(tamaMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;
bool    btnBLong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Voice recording
#define REC_SAMPLE_RATE   8000
#define REC_BUF_SAMPLES   (REC_SAMPLE_RATE * 4)   // 4 seconds (64KB DRAM)
#define REC_MAX_MS         4000  // auto-stop after 4s
int16_t* volatile recBuffer = nullptr;
bool voiceRecordFirstDraw = true;
volatile bool voiceProcFirstDraw = true;   // re-init the direct loading screen
TaskHandle_t volatile voiceTaskHandle = nullptr;
size_t voiceActualSamples = 0;

volatile bool voiceCancelRequested = false;
volatile bool voiceCancelling = false;   // task still unwinding after a cancel
WiFiClientSecure* volatile activeVoiceClient = nullptr;
HTTPClient* volatile activeVoiceHttp = nullptr;

char     voiceResponse[512] = "";
char     voiceTranscript[256] = "";
char     voiceError[80] = "";
char     hermesSessionId[64] = "";   // see data.h — session continuity
uint32_t voiceRecStart = 0;
volatile uint32_t voiceResponseStartMs = 0;
uint8_t  voiceResponseScroll = 0;

// Double-press tracking for BtnA (conserves long-press for menu)
uint32_t lastBtnAReleaseTime = 0;
bool     btnADoublePending = false;
const uint32_t DOUBLE_PRESS_MS = 280;


// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax = 0, ay = 0, az = 0;
  if (!M5.Imu.getAccelData(&ax, &ay, &az)) return false;
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

static uint8_t menuCount() {
  bool active = (tama.sessionsRunning > 0 || tama.sessionsWaiting > 0 || tama.promptId[0]);
  return MENU_N + (active ? 1 : 0);
}

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "wifi info", "hermes info", "led", "clock rot", "ascii pet", "sessions", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "factory reset", "back" };
const uint8_t RESET_N = 2;

bool     factoryResetWarningOpen = false;
bool     factoryResetDoublePending = false;
uint32_t lastResetReleaseTime = 0;

bool    sessionsOpen = false;
uint8_t sessScroll  = 0;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      settingsOpen = false;
      displayMode = DISP_INFO;
      infoPage = 4; // wifi page
      applyDisplayMode();
      characterInvalidate();
      return;
    case 3:
      settingsOpen = false;
      displayMode = DISP_INFO;
      infoPage = 2; // hermes page
      applyDisplayMode();
      characterInvalidate();
      return;
    case 4: s.led = !s.led; break;
    case 5: s.clockRot = (s.clockRot + 1) % 3; break;
    case 6: nextPet(); return;
    case 7: settingsOpen = false; sessionsOpen = true; sessScroll = 0; return;
    case 8: resetOpen = true; resetSel = 0; spr.fillSprite(0x0000); return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void executeFactoryReset() {
  beep(800, 200);
  if (nvsMutex && xSemaphoreTake(nvsMutex, portMAX_DELAY) == pdTRUE) {
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    xSemaphoreGive(nvsMutex);
  }
  LittleFS.format();
  bleClearBonds();
  delay(300);
  ESP.restart();
}

static void applyReset(uint8_t idx) {
  if (idx == 1) { // "back"
    resetOpen = false;
    spr.fillSprite(0x0000);
    return;
  }

  if (idx == 0) { // "factory reset"
    resetOpen = false;
    factoryResetWarningOpen = true;
    factoryResetDoublePending = false;
    beep(1000, 100);
    spr.fillSprite(0x0000);
  }
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i == 1) {
      spr.setTextColor(s.sound ? GREEN : p.textDim, PANEL);
      spr.print(s.sound ? " on" : "off");
    } else if (i == 4) {
      spr.setTextColor(s.led ? GREEN : p.textDim, PANEL);
      spr.print(s.led ? " on" : "off");
    } else if (i == 5) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 6) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

static void drawFactoryResetWarning() {
  const Palette& p = characterPalette();
  int mw = 125, mh = 160;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, HOT);
  
  spr.setTextSize(1);
  spr.setTextDatum(MC_DATUM);
  
  spr.setTextColor(HOT, PANEL);
  spr.drawString("ATTENZIONE!", W / 2, my + 15);
  
  spr.setTextColor(p.text, PANEL);
  spr.drawString("Questo cancellera", W / 2, my + 45);
  spr.drawString("tutti i dati e", W / 2, my + 60);
  spr.drawString("le impostazioni.", W / 2, my + 75);
  
  spr.setTextColor(0x5AEB, PANEL); // bright yellow/orange
  spr.drawString("Premi 2 volte A", W / 2, my + 105);
  spr.drawString("per confermare.", W / 2, my + 120);
  
  spr.setTextColor(p.textDim, PANEL);
  spr.drawString("B per annullare", W / 2, my + 145);
  
  spr.setTextDatum(TL_DATUM);
}

static void approvalTask(void* pvParameters) {
  char* dec = (char*)pvParameters;
  sendApproval(dec);
  free(dec);
  vTaskDelete(NULL);
}

static void sendApprovalAsync(const char* decision) {
  char* dec = strdup(decision);
  if (dec) {
    xTaskCreatePinnedToCore(approvalTask, "approvalTask", 4096, dec, 1, NULL, 0);
  }
}

static void stopTask(void* pvParameters) {
  sendStop();
  vTaskDelete(NULL);
}

static void sendStopAsync() {
  xTaskCreatePinnedToCore(stopTask, "stopTask", 4096, NULL, 1, NULL, 0);
}

static void drawSessions() {
  const Palette& p = characterPalette();
  uint8_t n = tama.sessLineCount;
  const uint8_t ROWS = 6;
  int mw = 118, mh = 16 + ROWS * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;

  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);

  spr.setTextColor(p.text, PANEL);
  spr.setCursor(mx + 6, my + 8);
  spr.print("Sessions");

  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + mw - 36, my + 8);
  spr.printf("%u/%u", n > 0 ? sessScroll + 1 : 0, n);

  if (n == 0) {
    spr.setTextColor(p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 24);
    spr.print("no sessions data");
  } else {
    if (sessScroll > n - ROWS) sessScroll = n > ROWS ? n - ROWS : 0;
    for (uint8_t i = 0; i < ROWS && sessScroll + i < n; i++) {
      uint8_t idx = sessScroll + i;
      bool sel = (idx == sessScroll);
      spr.setTextColor(sel ? p.text : p.textDim, PANEL);
      spr.setCursor(mx + 6, my + 24 + i * 14);
      spr.print(sel ? "> " : "  ");
      spr.print(tama.sessLines[idx]);
    }
  }

  drawMenuHints(p, mx, mw, my + mh - 12, "A", "back");
}

void drawVoiceRecording();
void drawVoiceProcessing();
void drawVoiceResponse();

// Mic helpers called from data.h streaming
void M5MicEnd()   { M5.Mic.end(); }
void M5MicBegin() {
  auto cfg = M5.Mic.config();
  cfg.magnification = 32;
  M5.Mic.config(cfg);
  M5.Mic.begin();
}
bool M5MicRecord(int16_t* b, size_t n, uint32_t r) { return M5.Mic.record(b, n, r); }

// Recording: circular buffer - continuously record, keep last N samples
static uint32_t recWraps = 0;   // number of full buffer wraps
static bool recStarted = false;

static void voiceStartRecording() {
  if (voiceState != VOICE_NONE || voiceTaskHandle != nullptr) return;

  spr.deleteSprite();
  if (landSpr.width() != 0) landSpr.deleteSprite();   // free its DRAM for audio

  recBuffer = (int16_t*)calloc(1, 256 + REC_BUF_SAMPLES * 2 + 256);
  if (!recBuffer) {
    Serial.println("[voice] failed to allocate recBuffer");
    snprintf(voiceError, sizeof(voiceError), "OOM (96K)");
    voiceState = VOICE_ERROR;
    spr.createSprite(W, H);
    beep(400, 120);
    return;
  }
  
  M5MicEnd();
  M5MicBegin();
  recWraps = 0;
  recStarted = true;
  
  voiceState = VOICE_RECORDING;
  voiceRecStart = millis();
  voiceResponse[0] = 0;
  voiceTranscript[0] = 0;
  voiceError[0] = 0;
  voiceCancelRequested = false; // Reset cancel flag
  voiceRecordFirstDraw = true;
  
  M5MicRecord((int16_t*)((uint8_t*)recBuffer + 256), REC_BUF_SAMPLES, REC_SAMPLE_RATE);
  beep(1200, 40); beep(1600, 40);
  wake();
}

static void voiceSendTask(void* pvParameters) {
  Serial.println("[voice-task] background send task started");
  
  bool ok = sendAudioChat((int16_t*)((uint8_t*)recBuffer + 256), voiceActualSamples, REC_SAMPLE_RATE,
                          voiceResponse, sizeof(voiceResponse),
                          voiceTranscript, sizeof(voiceTranscript),
                          voiceError, sizeof(voiceError));
  
  Serial.printf("[voice-task] sendAudioChat finished, status = %s\n", ok ? "OK" : "FAIL");
  
  // Safe cleanup of recBuffer
  if (recBuffer) {
    free(recBuffer);
    recBuffer = nullptr;
    Serial.println("[voice-task] freed recBuffer in task exit cleanup");
  }

  if (voiceCancelRequested) {
    Serial.println("[voice-task] cancel detected at exit");
    // Buffer is already freed above. Flip back to idle (no beep) so the main
    // loop leaves the PROCESSING screen and recreates the sprite safely.
    voiceState = VOICE_NONE;
  } else if (ok && voiceResponse[0]) {
    asm volatile("" ::: "memory");
    voiceState = VOICE_RESPONSE;
    beep(2400, 40);
  } else {
    if (!voiceError[0]) snprintf(voiceError, sizeof(voiceError), "No response");
    asm volatile("" ::: "memory");
    voiceState = VOICE_ERROR;
    beep(400, 120);
  }
  
  wake();
  
  voiceTaskHandle = nullptr;
  Serial.println("[voice-task] background send task exiting");
  vTaskDelete(NULL);
}

static void voiceStopAndSend() {
  if (voiceState != VOICE_RECORDING) return;
  voiceState = VOICE_PROCESSING;
  voiceProcFirstDraw = true;   // first loading frame paints its static chrome
  beep(1600, 40); beep(1200, 40);

  M5MicEnd();
  recStarted = false;
  uint32_t durMs = millis() - voiceRecStart;
  
  // Calculate actual recorded samples (handle circular buffer wraps)
  size_t totalSamples = (size_t)((uint64_t)REC_SAMPLE_RATE * durMs / 1000);
  if (totalSamples > REC_BUF_SAMPLES * 2) totalSamples = REC_BUF_SAMPLES * 2; // sanity cap
  size_t actualSamples = totalSamples;
  if (actualSamples > REC_BUF_SAMPLES) actualSamples = REC_BUF_SAMPLES;
  
  if (actualSamples < 1600) {
    snprintf(voiceError, sizeof(voiceError), "Too short (%lums)", durMs);
    voiceState = VOICE_ERROR;
    if (recBuffer) { free(recBuffer); recBuffer = nullptr; }
    if (spr.width() == 0) spr.createSprite(W, H);
    beep(400, 120); return;
  }

  voiceActualSamples = actualSamples;

  // Shrink the buffer to the actual size of the audio payload to reclaim heap early
  size_t neededSize = 256 + actualSamples * 2 + 256;
  int16_t* shrunkBuffer = (int16_t*)realloc(recBuffer, neededSize);
  if (shrunkBuffer) {
    recBuffer = shrunkBuffer;
    Serial.printf("[voice] shrunk recBuffer from %u to %u bytes\n", 
                  (unsigned)(256 + REC_BUF_SAMPLES * 2 + 256), (unsigned)neededSize);
  }

  Serial.printf("[voice] starting background send task for %u samples (%lums)...\n", actualSamples, durMs);
  
  // Create background task on Core 0 with priority 1 (same as networkTask)
  xTaskCreatePinnedToCore(voiceSendTask, "voiceSendTask", 8192, NULL, 1, (TaskHandle_t*)&voiceTaskHandle, 0);
  
  wake();
}

static void voiceCancel() {
  Serial.println("[voice] cancel requested");
  recStarted = false;
  M5MicEnd();

  if (voiceTaskHandle != nullptr) {
    // A network task is still in flight (PROCESSING). Cancel is purely
    // cooperative: we ONLY raise the flag. We must NOT tear down the client
    // from this core — the task is inside a blocking mbedTLS POST on Core 0,
    // and calling stop()/end() here destroys the TLS context under its feet
    // (mbedTLS is not thread-safe) → crash. Nor do we touch the sprite or the
    // audio buffer (the task owns 64KB + TLS state; a 64KB sprite alloc now
    // would exhaust the heap). The task checks voiceCancelRequested at its
    // next checkpoint (bounded by the 20s/45s request timeouts), frees the
    // buffer, and flips voiceState back to NONE on its way out; the main loop
    // then recreates the sprite once the buffer is gone.
    voiceCancelRequested = true;
    voiceCancelling = true;
    Serial.println("[voice] cancel: task in flight, unwinding cooperatively");
    return;
  }

  // No task running (RECORDING aborted before send, or RESPONSE/ERROR already
  // on screen). Safe to tear down and recreate the sprite right here.
  voiceCancelRequested = false;
  voiceCancelling = false;
  voiceState = VOICE_NONE;
  voiceResponse[0] = 0;
  voiceTranscript[0] = 0;
  voiceError[0] = 0;

  if (recBuffer) {
    free(recBuffer);
    recBuffer = nullptr;
    Serial.println("[voice] freed recBuffer on cancel (no active task)");
  }
  if (spr.width() == 0) {
    spr.createSprite(W, H);
    Serial.println("[voice] spr recreated on cancel");
  }
}

// Double-press A on the response/error screen: drop the finished exchange
// and immediately reopen the mic so the user can fire off a follow-up
// without bouncing back to the home screen. The background send task has
// already exited by the time a response/error is on screen (it nulls its
// own handle before flipping voiceState), so it's safe to start fresh.
static void voiceContinue() {
  Serial.println("[voice] continue: new take from response");
  voiceState = VOICE_NONE;
  voiceResponse[0] = 0;
  voiceTranscript[0] = 0;
  voiceError[0] = 0;
  voiceStartRecording();
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Axp.PowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
    case 6:
      sendStopAsync();
      Serial.println("stop: async sent");
      menuOpen = false;
      characterInvalidate();
      break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  uint8_t n = menuCount();
  int mw = 118, mh = 16 + n * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < n; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    if (i < MENU_N) {
      spr.print(menuItems[i]);
      if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
    } else {
      spr.print("stop hermes");
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
  static bool _firstRead = true;
  if (_firstRead && _clkDt.Year >= 2020) {
    _firstRead = false;
    Serial.printf("RTC read: %04d-%02d-%02d %02d:%02d:%02d\n",
                  _clkDt.Year, _clkDt.Month, _clkDt.Date,
                  _clkTm.Hours, _clkTm.Minutes, _clkTm.Seconds);
  }
}

static void clockUpdateOrient() {
  float ax = 0, ay = 0, az = 0;
  if (!M5.Imu.getAccelData(&ax, &ay, &az)) return;
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }

// Compose the landscape clock (buddy left, time/date right) onto a surface.
// Shared by the off-screen buffer and the direct-LCD fallback.
static void clockLandscapeCompose(TFT_eSPI* t, const Palette& p) {
  if (buddyMode) buddyRenderTo(t, activeState);
  else { characterSetState(activeState); characterRenderTo(t, 57, 45); }

  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char hm[6];  snprintf(hm,  sizeof(hm),  "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
  char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
  t->setTextDatum(MC_DATUM);
  t->setTextSize(3); t->setTextColor(p.text, p.bg);    t->drawString(hm, 170, 42);
  t->setTextSize(2); t->setTextColor(p.textDim, p.bg); t->drawString(ssl, 170, 72);
                                                       t->drawString(wdl, 170, 102);
  if (!_onUsb) {
    t->setTextSize(1); t->setTextColor(0xF800, p.bg);  t->drawString("No USB", 170, 120);
  }
  t->setTextDatum(TL_DATUM);
  t->setTextSize(1);
}

static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    if (!_onUsb) {
      spr.setTextColor(0xF800, p.bg);
      spr.drawString("No USB", CX, 218);
    }
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: composited off-screen into landSpr and pushed in one shot, so
  // the buddy animation never tears or flashes the way per-tick direct-LCD
  // drawing does. ~20fps keeps the full-frame push light. Falls back to direct
  // LCD only if the buffer can't be allocated.
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw < 48) return;
  lastDraw = millis();

  if (landSpr.width() == 0) {
    landSpr.setColorDepth(16);
    landSpr.createSprite(240, 135);
  }
  M5.Lcd.setRotation(clockOrient);
  if (landSpr.width() != 0) {
    landSpr.fillSprite(p.bg);
    clockLandscapeCompose(&landSpr, p);
    landSpr.pushSprite(0, 0);
  } else {
    static uint8_t painted = 0xFE;
    if (painted != clockOrient) { M5.Lcd.fillScreen(p.bg); painted = clockOrient; }
    clockLandscapeCompose(&M5.Lcd, p);
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_SLEEP;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 1)  return P_BUSY;
  
  // Se connesso e inattivo, adatta l'animazione all'umore (mood) del pet
  uint8_t mood = statsMoodTier();
  if (mood >= 4) {
    // Molto felice: alterna cuoricini (30%) e festa (10%), altrimenti normale
    uint32_t t = millis() / 5000;
    if (t % 10 < 3) return P_HEART;
    if (t % 10 == 3) return P_CELEBRATE;
    return P_IDLE;
  } else if (mood <= 1) {
    // Triste/trascurato: alterna sonnolenza (66%) e stordimento/tristezza (33%)
    uint32_t t = millis() / 6000;
    if (t % 3 == 0) return P_DIZZY;
    return P_SLEEP;
  }
  
  return P_IDLE;   // connected, 0 sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax = 0, ay = 0, az = 0;
  if (!M5.Imu.getAccelData(&ax, &ay, &az)) return false;
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

static void tinyHeart(int x, int y, bool filled, uint16_t col);

// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 32, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(6, y); spr.print(section);
  y += 10;
  // Draw an elegant line separator below header
  spr.drawFastHLine(4, y, W - 8, p.textDim);
  y += 6;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    // Draw a nice card container
    spr.drawRoundRect(6, y, W - 12, H - y - 10, 4, p.textDim);
    int cardY = y + 6;
    auto cardLn = [&](uint16_t color, const char* text) {
      spr.setTextColor(color, p.bg);
      spr.setCursor(12, cardY);
      spr.print(text);
      cardY += 9;
    };
    cardLn(p.text, "Hermes Companion");
    cardLn(p.textDim, "I monitor runs");
    cardLn(p.textDim, "and track tokens.");
    cardY += 4;
    cardLn(p.body, "Controls:");
    cardLn(p.textDim, "Press A on prompt");
    cardLn(p.textDim, "to approve runs.");
    cardLn(p.textDim, "Press B to deny.");
    cardY += 4;
    cardLn(p.body, "Settings:");
    cardLn(p.textDim, "Hold A for menu.");
    cardLn(p.textDim, "Select ASCII pet");
    cardLn(p.textDim, "to cycle companion.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    
    // Draw Button Cap rows
    int rowY = y + 4;
    auto drawButtonRow = [&](const char* key, const char* label1, const char* label2) {
      int kw = strlen(key) * 6 + 10;
      spr.fillRoundRect(8, rowY, kw, 12, 2, p.body);
      spr.setTextColor(p.bg, p.body);
      spr.setCursor(8 + (kw - strlen(key)*6)/2, rowY + 2);
      spr.print(key);
      
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(8 + kw + 8, rowY + 2);
      spr.print(label1);
      if (label2[0]) {
        spr.setTextColor(p.textDim, p.bg);
        spr.setCursor(8 + kw + 8, rowY + 11);
        spr.print(label2);
        rowY += 22;
      } else {
        rowY += 16;
      }
    };
    
    drawButtonRow("A", "Approve / Next", "Front button");
    drawButtonRow("B", "Deny / Page", "Side button");
    drawButtonRow("Menu", "Open Settings", "Hold A button");
    drawButtonRow("PWR", "Screen Off / On", "Hold 6s to Off");
    drawButtonRow("2x A", "Voice Record", "Talk to Hermes");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "HERMES", infoPage);
    
    // Status indicator
    bool conn = tama.connected;
    spr.fillCircle(12, y + 6, 4, conn ? GREEN : RED);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(22, y + 2);
    spr.print(conn ? "Connected" : "Disconnected");
    
    // Status Panel Box
    int boxY = y + 14;
    spr.drawRoundRect(6, boxY, W - 12, H - boxY - 10, 4, p.textDim);
    
    int rowY = boxY + 6;
    auto drawHermesRow = [&](const char* label, const char* val, uint16_t valColor) {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(12, rowY);
      spr.print(label);
      spr.setTextColor(valColor, p.bg);
      spr.setCursor(W - 16 - strlen(val)*6, rowY);
      spr.print(val);
      rowY += 10;
    };
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", tama.sessionsTotal);
    drawHermesRow("Total Runs:", buf, p.text);
    
    snprintf(buf, sizeof(buf), "%u", tama.sessionsRunning);
    drawHermesRow("Running:", buf, p.text);
    
    snprintf(buf, sizeof(buf), "%u", tama.sessionsWaiting);
    drawHermesRow("Waiting:", buf, tama.sessionsWaiting > 0 ? HOT : p.text);
    
    drawHermesRow("Protocol:", "HTTP (REST)", p.text);
    
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    if (age > 9999) snprintf(buf, sizeof(buf), "never");
    else snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)age);
    drawHermesRow("Updated:", buf, p.text);
    
    drawHermesRow("State:", stateNames[activeState], p.text);
    
    if (tama.model[0]) {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(12, rowY);
      spr.print("Model:");
      rowY += 10;
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(12, rowY);
      int maxLen = (W - 24) / 6;
      char truncatedModel[32];
      if ((int)strlen(tama.model) > maxLen) {
        strncpy(truncatedModel, tama.model, maxLen - 2);
        truncatedModel[maxLen - 2] = '.';
        truncatedModel[maxLen - 1] = '.';
        truncatedModel[maxLen] = '\0';
      } else {
        strcpy(truncatedModel, tama.model);
      }
      spr.print(truncatedModel);
      rowY += 10;
    } else {
      drawHermesRow("Model:", "none", p.text);
    }

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    static uint32_t lastBatUpdate = 0;
    static int vBat_mV = 0;
    static int iBat_mA = 0;
    static int vBus_mV = 0;
    static int pct = 0;
    static bool usb = false;
    static bool charging = false;
    static bool full = false;

    static uint8_t lastPageSeen = 0xFF;
    uint32_t nowMs = millis();
    if (lastPageSeen != infoPage || nowMs - lastBatUpdate >= 2000 || lastBatUpdate == 0) {
      lastPageSeen = infoPage;
      lastBatUpdate = nowMs;
      vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
      iBat_mA = (int)M5.Axp.GetBatCurrent();
      vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
      pct = M5.Axp.GetBatLevel();
      usb = vBus_mV > 4000;
      charging = usb && iBat_mA > 1;
      full = usb && vBat_mV > 4100 && iBat_mA < 10;
    }
    
    // Draw Battery Icon
    int bx = 8, by = y + 2;
    int bw = 24, bh = 12;
    spr.drawRect(bx, by, bw, bh, p.text);
    spr.fillRect(bx + bw, by + 3, 2, 6, p.text); // battery tip
    int fillW = (pct * (bw - 4)) / 100;
    uint16_t batCol = full ? GREEN : (charging ? 0xFFE0 : (pct < 20 ? HOT : p.body));
    if (fillW > 0) {
      spr.fillRect(bx + 2, by + 2, fillW, bh - 4, batCol);
    }
    
    // Draw Charging Lightning Bolt symbol inside the battery if charging
    if (charging && !full) {
      spr.fillTriangle(bx + 11, by + 2, bx + 15, by + 2, bx + 10, by + 7, batCol == p.body ? p.bg : p.body);
      spr.fillTriangle(bx + 12, by + 5, bx + 8, by + 10, bx + 13, by + 10, batCol == p.body ? p.bg : p.body);
    }
    
    // Print battery percentage
    spr.setTextSize(1);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(bx + bw + 8, by + 2);
    spr.printf("%d%%", pct);
    
    spr.setTextColor(full ? GREEN : (charging ? 0xFFE0 : p.textDim), p.bg);
    spr.setCursor(bx + bw + 42, by + 2);
    spr.print(full ? "Full" : (charging ? "Charging" : (usb ? "USB" : "Battery")));
    
    // System Stats Panel
    int boxY = y + 18;
    spr.drawRoundRect(6, boxY, W - 12, H - boxY - 10, 4, p.textDim);
    
    int rowY = boxY + 6;
    auto drawDeviceRow = [&](const char* label, const char* val) {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(12, rowY);
      spr.print(label);
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(W - 16 - strlen(val)*6, rowY);
      spr.print(val);
      rowY += 10;
    };
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    drawDeviceRow("Voltage:", buf);
    
    if (ownerName()[0]) drawDeviceRow("Owner:", ownerName());
    
    uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf), "%luh %02lum", up / 3600, (up / 60) % 60);
    drawDeviceRow("Uptime:", buf);
    
    snprintf(buf, sizeof(buf), "%u KB", ESP.getFreeHeap() / 1024);
    drawDeviceRow("Free Heap:", buf);
    
    snprintf(buf, sizeof(buf), "%u/4", brightLevel);
    drawDeviceRow("Screen:", buf);
    
    drawDeviceRow("Reset:", resetReasonStr());
    
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(12, rowY);
    spr.print("Hermes:");
    spr.setTextColor(dataConnected() ? GREEN : HOT, p.bg);
    const char* gwStatus = dataConnected() ? "online" : "offline";
    spr.setCursor(W - 16 - strlen(gwStatus)*6, rowY);
    spr.print(gwStatus);
    rowY += 10;

  } else if (infoPage == 4) {
    _infoHeader(p, y, "WI-FI", infoPage);
    bool connected = (WiFi.status() == WL_CONNECTED);

    // Draw Wi-Fi Icon
    int wx = 18, wy = y + 10;
    uint16_t wifiCol = connected ? GREEN : HOT;
    spr.fillCircle(wx, wy, 2, wifiCol);
    spr.drawArc(wx, wy, 4, 5, 225, 315, wifiCol);
    spr.drawArc(wx, wy, 8, 9, 225, 315, wifiCol);
    spr.drawArc(wx, wy, 12, 13, 225, 315, wifiCol);
    
    // Status text
    spr.setTextSize(1);
    spr.setTextColor(connected ? GREEN : HOT, p.bg);
    spr.setCursor(wx + 22, wy - 4);
    spr.print(connected ? "Connected" : "Offline");
    
    // Wi-Fi Details Panel
    int boxY = y + 18;
    spr.drawRoundRect(6, boxY, W - 12, H - boxY - 10, 4, p.textDim);
    
    int rowY = boxY + 6;
    auto drawWifiRow = [&](const char* label, const char* val) {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(12, rowY);
      spr.print(label);
      spr.setTextColor(p.text, p.bg);
      // Limit value print width to not overflow card
      int maxLen = (W - 28 - (strlen(label) * 6)) / 6;
      char truncatedVal[24];
      if ((int)strlen(val) > maxLen) {
        strncpy(truncatedVal, val, maxLen - 2);
        truncatedVal[maxLen - 2] = '.';
        truncatedVal[maxLen - 1] = '.';
        truncatedVal[maxLen] = '\0';
      } else {
        strcpy(truncatedVal, val);
      }
      spr.setCursor(W - 16 - strlen(truncatedVal)*6, rowY);
      spr.print(truncatedVal);
      rowY += 10;
    };
    
    if (connected) {
      drawWifiRow("SSID:", WiFi.SSID().c_str());
      drawWifiRow("IP:", WiFi.localIP().toString().c_str());
      char buf[16];
      snprintf(buf, sizeof(buf), "%d dBm", WiFi.RSSI());
      drawWifiRow("RSSI:", buf);
      
      // Divider
      spr.drawFastHLine(10, rowY + 3, W - 20, p.textDim);
      rowY += 8;
      
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(12, rowY);
      spr.print("Serv IP:");
      rowY += 10;

      spr.setTextColor(p.text, p.bg);
      spr.setCursor(12, rowY);
      int maxLen = (W - 24) / 6;
      char truncatedIp[32];
      if ((int)strlen(settings().hermesIp) > maxLen) {
        strncpy(truncatedIp, settings().hermesIp, maxLen - 2);
        truncatedIp[maxLen - 2] = '.';
        truncatedIp[maxLen - 1] = '.';
        truncatedIp[maxLen] = '\0';
      } else {
        strcpy(truncatedIp, settings().hermesIp);
      }
      spr.print(truncatedIp);
      rowY += 10;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(12, rowY);
      spr.print("Serv Port:");
      rowY += 10;

      spr.setTextColor(p.text, p.bg);
      spr.setCursor(12, rowY);
      spr.print(settings().hermesPort);
      rowY += 10;
    } else {
      drawWifiRow("SSID:", WiFi.SSID().length() ? WiFi.SSID().c_str() : "unknown");
      drawWifiRow("State:", "connecting...");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    
    int boxY = y + 12;
    spr.drawRoundRect(6, boxY, W - 12, H - boxY - 14, 4, p.textDim);
    spr.drawRoundRect(8, boxY + 2, W - 16, H - boxY - 18, 4, p.textDim);
    
    // Centered text lines
    int cardY = boxY + 12;
    auto centeredLn = [&](uint16_t color, const char* text, int spacing = 12) {
      spr.setTextColor(color, p.bg);
      int tw = strlen(text) * 6;
      spr.setCursor((W - tw) / 2, cardY);
      spr.print(text);
      cardY += spacing;
    };
    
    centeredLn(p.textDim, "creata da", 12);
    centeredLn(p.text, "Syax89", 14);
    
    // Draw a small red heart!
    tinyHeart(W / 2, cardY + 2, true, RED);
    cardY += 12;
    
    centeredLn(p.textDim, "Firmware Version", 12);
    centeredLn(p.body, "v2.2.0", 14);
    
    centeredLn(p.textDim, "hardware device", 10);
    centeredLn(p.text, "M5StickC Plus 2");
  }
}

// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  if (width > 23) width = 23;
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    // Skip any run of whitespace — space, but also \n \r \t. Without this
    // a model reply's embedded newlines get baked into a "word" and print()
    // then jumps the cursor mid-line, throwing text off-screen.
    while (*p && (uint8_t)*p <= ' ') p++;
    // measure next word (run of printable chars)
    const char* w = p;
    while (*p && (uint8_t)*p > ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// ── Shared visual language for the recording → processing → response flow ──
// The three screens used to look like three unrelated apps (different
// borders, different "you said" labels, processing showing none of the data
// it already had). These helpers give them one continuous identity: a
// pulsing border whose color travels from HOT (the user's mic is hot) to
// the character's body color (Hermes has taken over), plus a shared
// "You: / Hermes:" transcript layout that fills in progressively instead of
// hiding behind a generic spinner.
static uint16_t blend565(uint16_t a, uint16_t b, float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint8_t r = ar + (uint8_t)((br - ar) * t);
  uint8_t g = ag + (uint8_t)((bg - ag) * t);
  uint8_t bl = ab + (uint8_t)((bb - ab) * t);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// A calm, static frame inset from the screen edge — 2px thick, rounded
// corners, padded ~6px in so it reads as a deliberate border rather than
// the panel running off the edge. The color still carries the HOT→body
// travel across the three phases.
static void drawInsetBorder(TFT_eSPI* tgt, uint16_t color) {
  const int m = 6;
  tgt->drawRoundRect(m, m, W - 2 * m, H - 2 * m, 6, color);
  tgt->drawRoundRect(m + 1, m + 1, W - 2 * m - 2, H - 2 * m - 2, 5, color);
}

// ── PHASE 1 — RECORDING ────────────────────────────────────────────────
// Its own world: a circular VU meter. 36 ticks ring the screen and light up
// with the live mic level, the elapsed time sits big in the middle, and a
// thin arc of "fuel" drains toward the 4s limit. Drawn straight to the LCD
// (the sprite is freed for the audio buffer) so every element repaints only
// its own footprint each frame — no full clears, no flicker.
void drawVoiceRecording() {
  const Palette& p = characterPalette();
  uint32_t elapsedMs = millis() - voiceRecStart;

  const int cx = CX, cy = 116, rin = 38, rout = 50;
  const int NT = 36;
  static float lvl;
  static uint32_t lastTenths;
  static bool lastBlink;

  if (voiceRecordFirstDraw) {
    voiceRecordFirstDraw = false;
    M5.Lcd.fillScreen(p.bg);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(p.textDim, p.bg);
    M5.Lcd.drawString("RECORDING", 24, 12);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.drawString("A send   B cancel", CX, H - 18);
    M5.Lcd.setTextDatum(TL_DATUM);
    lvl = 0;
    lastTenths = 0xFFFFFFFF;
    lastBlink = false;
  }

  // Blinking REC dot, top-left
  bool blink = (millis() / 450) % 2;
  if (blink != lastBlink) {
    lastBlink = blink;
    M5.Lcd.fillCircle(14, 16, 4, blink ? HOT : p.bg);
    if (!blink) M5.Lcd.drawCircle(14, 16, 4, p.textDim);
  }

  // Live level: peak of the last ~30ms, smoothed (fast attack / slow decay)
  size_t curr = (size_t)((uint64_t)REC_SAMPLE_RATE * elapsedMs / 1000);
  if (curr > REC_BUF_SAMPLES) curr = REC_BUF_SAMPLES;
  size_t win = 240, s0 = (curr > win) ? curr - win : 0;
  int32_t peak = 0;
  int16_t* pcm = recBuffer ? (int16_t*)((uint8_t*)recBuffer + 256) : nullptr;
  if (pcm) for (size_t s = s0; s < curr; s++) { int32_t a = abs(pcm[s]); if (a > peak) peak = a; }
  float target = peak / 8000.0f; if (target > 1.0f) target = 1.0f;
  lvl += (target - lvl) * (target > lvl ? 0.5f : 0.15f);

  // VU ring — every tick repaints over itself, so no clearing is needed.
  int litCount = (int)(lvl * NT + 0.5f);
  for (int k = 0; k < NT; k++) {
    float a = -1.5708f + k * (6.2832f / NT);   // start at top, clockwise
    float ca = cosf(a), sa = sinf(a);
    uint16_t col = (k < litCount) ? HOT : p.textDim;
    M5.Lcd.drawLine(cx + (int)(ca * rin), cy + (int)(sa * rin),
                    cx + (int)(ca * rout), cy + (int)(sa * rout), col);
    if (k < litCount) {  // brighten lit ticks with an outer cap
      M5.Lcd.fillCircle(cx + (int)(ca * rout), cy + (int)(sa * rout), 1,
                        blend565(HOT, 0xFFFF, 0.5f));
    }
  }

  // Elapsed time, large, centered inside the ring (cleared box on change)
  uint32_t tenths = elapsedMs / 100;
  if (tenths != lastTenths) {
    lastTenths = tenths;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u.%u",
             (unsigned)(elapsedMs / 1000), (unsigned)((elapsedMs % 1000) / 100));
    M5.Lcd.fillRect(cx - 26, cy - 16, 52, 30, p.bg);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(p.text, p.bg);
    M5.Lcd.drawString(buf, cx, cy - 4);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(p.textDim, p.bg);
    M5.Lcd.drawString("/ 4.0s", cx, cy + 12);
    M5.Lcd.setTextDatum(TL_DATUM);
  }

  drawInsetBorder(&M5.Lcd, p.textDim);
}

// ── PHASE 2 — PROCESSING ───────────────────────────────────────────────
// Its own world: a centered "thinking" card. The real status verb leads,
// three dots bounce in sequence beneath it, and the user's words sit at the
// bottom as a quiet caption so nothing is hidden. No labels or dividers
// shared with the response screen — this reads as its own moment.
// Dot animation geometry, shared by the buffered and incremental paths.
static const int PROC_BASE_Y = 120, PROC_DOT_R = 5, PROC_SPACING = 22;

static void procDrawDots(TFT_eSPI* t, const Palette& p) {
  int dx0 = CX - PROC_SPACING;
  for (int i = 0; i < 3; i++) {
    float ph = sinf(millis() * 0.006f - i * 0.9f);
    if (ph < 0) ph = 0;
    int hop = (int)(ph * 16);
    int x = dx0 + i * PROC_SPACING;
    t->fillEllipse(x, PROC_BASE_Y + 11, PROC_DOT_R, 2, p.textDim);   // shadow
    t->fillCircle(x, PROC_BASE_Y - hop, PROC_DOT_R, p.body);
  }
}

static void procDrawStatus(TFT_eSPI* t, const Palette& p, const char* status) {
  t->setTextDatum(MC_DATUM);
  t->setTextSize(strlen(status) <= 9 ? 2 : 1);
  t->setTextColor(p.text, p.bg);
  t->drawString(status, CX, 64);
  t->setTextSize(1);
  t->setTextDatum(TL_DATUM);
}

static void procDrawCaption(TFT_eSPI* t, const Palette& p) {
  t->setTextDatum(MC_DATUM);
  t->setTextColor(p.textDim, p.bg);
  if (voiceTranscript[0]) {
    char w[2][24];
    uint8_t n = wrapInto(voiceTranscript, w, 2, 18);
    for (uint8_t i = 0; i < n && i < 2; i++) t->drawString(w[i], CX, 170 + i * 12);
  } else {
    t->drawString("transcribing audio", CX, 170);
  }
  t->setTextDatum(TL_DATUM);
}

void drawVoiceProcessing() {
  const Palette& p = characterPalette();
  const char* status = voiceCancelling ? "Cancelling"
                     : (voiceError[0] ? voiceError : "Thinking");

  // Buffered path: the sprite exists (after STT frees the audio buffer). Full
  // redraw + single push — inherently flicker-free, like every portrait page.
  if (spr.width() > 0) {
    spr.fillSprite(p.bg);
    procDrawStatus(&spr, p, status);
    procDrawDots(&spr, p);
    procDrawCaption(&spr, p);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(voiceCancelling ? "please wait" : "B to cancel", CX, H - 18);
    spr.setTextDatum(TL_DATUM);
    drawInsetBorder(&spr, p.textDim);
    spr.pushSprite(0, 0);
    voiceProcFirstDraw = true;   // re-init the direct path if the sprite is freed again
    return;
  }

  // Incremental direct-to-LCD path: during STT the sprite is freed for the
  // 64KB audio buffer, so a full-screen clear every frame would flicker badly.
  // Instead paint the static chrome once, then repaint only the status verb
  // (on change) and the bouncing dots (each frame) over their own footprint.
  static char lastStatus[24] = "";
  static bool lastCancel = false;
  if (voiceProcFirstDraw) {
    voiceProcFirstDraw = false;
    M5.Lcd.fillScreen(p.bg);
    procDrawCaption(&M5.Lcd, p);
    drawInsetBorder(&M5.Lcd, p.textDim);
    lastStatus[0] = 0;
    lastCancel = !voiceCancelling;   // force status + footer on this frame
  }

  if (strcmp(lastStatus, status) != 0) {
    strncpy(lastStatus, status, sizeof(lastStatus) - 1);
    lastStatus[sizeof(lastStatus) - 1] = 0;
    M5.Lcd.fillRect(8, 50, W - 16, 28, p.bg);
    procDrawStatus(&M5.Lcd, p, status);
  }

  if (lastCancel != voiceCancelling) {
    lastCancel = voiceCancelling;
    M5.Lcd.fillRect(8, H - 26, W - 16, 14, p.bg);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(p.textDim, p.bg);
    M5.Lcd.drawString(voiceCancelling ? "please wait" : "B to cancel", CX, H - 18);
    M5.Lcd.setTextDatum(TL_DATUM);
  }

  // Dots: clear just their band and redraw each frame.
  int dx0 = CX - PROC_SPACING;
  M5.Lcd.fillRect(dx0 - PROC_DOT_R - 2, PROC_BASE_Y - 18,
                  PROC_SPACING * 2 + 2 * PROC_DOT_R + 4, 36, p.bg);
  procDrawDots(&M5.Lcd, p);
}

// ── PHASE 3 — RESPONSE ─────────────────────────────────────────────────
// A clean chat card. Your question sits quietly at the top in a tinted
// strip; below it a "Hermes" header (dot + label + accent rule) introduces
// the answer, which fades in line by line with a blinking caret while more
// remain. wrapInto now treats newlines as separators, so the reply stays
// inside its column instead of jumping off-screen.
void drawVoiceResponse() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);

  // — Your question: a soft tinted strip, up to 2 lines —
  char q[2][24];
  uint8_t qn = voiceTranscript[0] ? wrapInto(voiceTranscript, q, 2, 20) : 0;
  int qStripH = 14 + (qn > 0 ? qn : 1) * 10;
  spr.fillRoundRect(6, 6, W - 12, qStripH, 4, PANEL);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(12, 11);
  spr.print("You");
  spr.setTextColor(p.text, PANEL);
  if (qn) {
    for (uint8_t i = 0; i < qn; i++) {
      spr.setCursor(12, 23 + i * 10);
      spr.print(q[i]);
    }
  } else {
    spr.setCursor(12, 23);
    spr.print("(voice message)");
  }

  // — Hermes header: dot + label + accent rule —
  int hy = 6 + qStripH + 8;
  spr.fillCircle(11, hy + 3, 3, p.body);
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(20, hy);
  spr.print("Hermes");
  spr.drawFastHLine(8, hy + 12, W - 16, p.body);

  // — Answer: line-by-line reveal, ~110ms per line, blinking caret —
  char wr[48][24];
  uint8_t n = wrapInto(voiceResponse, wr, 48, 19);
  uint32_t shown = (millis() - voiceResponseStartMs) / 110;
  uint8_t visible = (shown < n) ? (uint8_t)shown : n;

  const int ty = hy + 20, lh = 11;
  uint8_t maxRows = (uint8_t)((H - 24 - ty) / lh);   // stop above the footer

  // Clamp scroll offset to prevent overflow in all cases
  if (voiceResponseScroll + maxRows > n) {
    if (n > maxRows) {
      voiceResponseScroll = n - maxRows;
    } else {
      voiceResponseScroll = 0;
    }
  }

  spr.setTextColor(p.text, p.bg);
  for (uint8_t i = voiceResponseScroll; i < visible && i < voiceResponseScroll + maxRows; i++) {
    spr.setCursor(8, ty + (i - voiceResponseScroll) * lh);
    spr.print(wr[i]);
  }
  if (visible < n && visible >= voiceResponseScroll && visible < voiceResponseScroll + maxRows && (millis() / 350) % 2) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(8, ty + (visible - voiceResponseScroll) * lh);
    spr.print("|");
  }

  spr.setTextColor(p.textDim, p.bg);
  spr.setTextDatum(MC_DATUM);
  if (shown < n) {
    spr.drawString("B skip reveal", CX, H - 18);
  } else if (n > maxRows) {
    if (voiceResponseScroll + maxRows < n) {
      spr.drawString("B scroll   AA again", CX, H - 18);
    } else {
      spr.drawString("B close    AA again", CX, H - 18);
    }
  } else {
    spr.drawString("AA again   B close", CX, H - 18);
  }
  spr.setTextDatum(TL_DATUM);

  drawInsetBorder(&spr, p.textDim);
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p, int y) {
  Stats sSnapshot;
  statsGetSnapshot(&sSnapshot);
  
  // Single border using p.body (mascot color) matching Info screens but in the theme color
  spr.drawRoundRect(6, y, W - 12, H - y - 10, 4, p.body);
  
  int rowY = y + 8;
  
  // Level display
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(2);
  char lvlBuf[16];
  snprintf(lvlBuf, sizeof(lvlBuf), "Lv %u", sSnapshot.level);
  int tw = strlen(lvlBuf) * 12;
  spr.setCursor((W - tw) / 2, rowY);
  spr.print(lvlBuf);
  spr.setTextSize(1);
  rowY += 18;

  // Mood row
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(14, rowY);
  spr.print("Mood:");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) {
    tinyHeart((W - 52) + i * 12, rowY + 4, i < mood, moodCol);
  }
  rowY += 12;

  // Fed row
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(14, rowY);
  spr.print("Fed:");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = (W - 86) + i * 8;
    if (i < fed) spr.fillCircle(px, rowY + 4, 2, p.body);
    else spr.drawCircle(px, rowY + 4, 2, p.textDim);
  }
  rowY += 12;

  // Energy row
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(14, rowY);
  spr.print("Energy:");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? GREEN : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = (W - 60) + i * 10;
    if (i < en) spr.fillRect(px, rowY + 1, 8, 6, enCol);
    else spr.drawRect(px, rowY + 1, 8, 6, p.textDim);
  }
  rowY += 14;

  // Divider
  spr.drawFastHLine(14, rowY, W - 28, p.body);
  rowY += 6;

  // Stats table
  auto drawStatRow = [&](const char* label, const char* val) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(14, rowY);
    spr.print(label);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(W - 20 - strlen(val)*6, rowY);
    spr.print(val);
    rowY += 10;
  };
  
  // Format Last Use time
  uint32_t elapsedSec = 0;
  uint32_t nowSec = time(nullptr);
  if (nowSec > 1700000000 && sSnapshot.lastUseSec > 1700000000) {
    if (nowSec >= sSnapshot.lastUseSec) {
      elapsedSec = nowSec - sSnapshot.lastUseSec;
    }
  } else {
    elapsedSec = (millis() - sSnapshot.lastUseMs) / 1000;
  }
  
  char lastUseBuf[32];
  if (elapsedSec < 60) {
    snprintf(lastUseBuf, sizeof(lastUseBuf), "Just now");
  } else if (elapsedSec < 3600) {
    snprintf(lastUseBuf, sizeof(lastUseBuf), "%lum ago", elapsedSec / 60);
  } else if (elapsedSec < 86400) {
    snprintf(lastUseBuf, sizeof(lastUseBuf), "%luh ago", elapsedSec / 3600);
  } else {
    snprintf(lastUseBuf, sizeof(lastUseBuf), "%lud ago", elapsedSec / 86400);
  }

  drawStatRow("Last Use:", lastUseBuf);
  
  char sessBuf[16];
  snprintf(sessBuf, sizeof(sessBuf), "%u", tama.sessionsTotal);
  drawStatRow("Sessions:", sessBuf);
  
  char buf[32];
  uint32_t nap = sSnapshot.napSeconds;
  snprintf(buf, sizeof(buf), "%luh %02lum", nap/3600, (nap/60)%60);
  drawStatRow("Napped:", buf);
  
  auto tokFmt = [&](const char* label, uint32_t v) {
    char tBuf[16];
    if (v >= 1000000)   snprintf(tBuf, sizeof(tBuf), "%lu.%luM", v/1000000, (v/100000)%10);
    else if (v >= 1000) snprintf(tBuf, sizeof(tBuf), "%lu.%luK", v/1000, (v/100)%10);
    else                snprintf(tBuf, sizeof(tBuf), "%lu", v);
    drawStatRow(label, tBuf);
  };
  
  tokFmt("Lifetime:", sSnapshot.tokens);
  tokFmt("Daily:", tama.tokensToday);
}

static void drawPetHowTo(const Palette& p, int y) {
  // Single border using p.body (mascot color)
  spr.drawRoundRect(6, y, W - 12, H - y - 10, 4, p.body);
  
  int cardY = y + 10;
  auto centeredLn = [&](uint16_t color, const char* text, int spacing = 10) {
    spr.setTextColor(color, p.bg);
    int tw = strlen(text) * 6;
    spr.setCursor((W - tw) / 2, cardY);
    spr.print(text);
    cardY += spacing;
  };
  
  centeredLn(p.body, "GUIDE", 12);
  
  centeredLn(p.text, "MOOD", 8);
  centeredLn(p.textDim, "Approve fast = Up", 8);
  centeredLn(p.textDim, "Deny often = Down", 12);
  
  centeredLn(p.text, "FED", 8);
  centeredLn(p.textDim, "50K tokens = Lv Up", 12);
  
  centeredLn(p.text, "ENERGY", 8);
  centeredLn(p.textDim, "Face-down to Nap", 8);
  centeredLn(p.textDim, "Nap refills energy", 14);
  
  centeredLn(p.textDim, "A: screens  B: page", 8);
  centeredLn(p.textDim, "Hold A: menu", 8);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;
  
  // Uniform header style matching Info screens
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 32, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
  y += 14;
  
  spr.drawFastHLine(4, y, W - 8, p.body);
  y += 6;

  if (petPage == 0) drawPetStats(p, y);
  else drawPetHowTo(p, y);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }

  // Only display critical connection warnings/errors, hide normal states like "idle" or "Working..."
  if (strstr(tama.msg, "No ") || strstr(tama.msg, "Fail") || strstr(tama.msg, "Error") || strstr(tama.msg, "HTTP")) {
    const Palette& p = characterPalette();
    const int LH = 8;
    const int AREA = LH + 4;
    spr.fillRect(0, H - AREA, W, AREA, p.bg);
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
  }
}

// Compact token formatter shared by the busy panels (mirrors the pet stats).
static void busyTokFmt(uint32_t v, char* b, size_t n) {
  if (v >= 1000000)   snprintf(b, n, "%lu.%luM", v / 1000000, (v / 100000) % 10);
  else if (v >= 1000) snprintf(b, n, "%lu.%luK", v / 1000, (v / 100) % 10);
  else                snprintf(b, n, "%lu", v);
}

// Buddy-at-work screen (portrait). Mirrors the resting clock layout — the
// mascot peeks at the top, a panel fills the lower half — but here the buddy
// is working and the panel reports the live Hermes session (time, the running
// session's title, and stats) instead of just the date. The buddy owns
// y0..~85 (peek mode); we own y90..H, same split as drawClock.
static void drawBusyPanel(const Palette& p) {
  spr.fillRect(0, 90, W, H - 90, p.bg);
  spr.setTextDatum(MC_DATUM);

  if (_clkDt.Year >= 2020) {
    char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
    spr.setTextSize(3); spr.setTextColor(p.text, p.bg);
    spr.drawString(hm, CX, 112);
  }
  spr.setTextSize(1);
  spr.drawFastHLine(18, 133, W - 36, p.textDim);

  // "working" line with a breathing dot.
  bool on = (millis() / 500) % 2;
  spr.setTextColor(p.body, p.bg);
  spr.drawString("Hermes working", CX + 6, 146);
  spr.fillCircle(CX - 46, 146, 3, on ? p.body : p.textDim);

  // The running session's title/summary, wrapped to 2 lines (or the model
  // name as a fallback when no title is exposed yet).
  int y = 162;
  if (tama.activeTitle[0]) {
    char tl[2][24];
    uint8_t n = wrapInto(tama.activeTitle, tl, 2, 21);
    spr.setTextColor(p.text, p.bg);
    for (uint8_t i = 0; i < n; i++) { spr.drawString(tl[i], CX, y); y += 12; }
  } else if (tama.model[0]) {
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(tama.model, CX, y);
  }

  // Stats footer.
  char buf[28], tb[12];
  spr.setTextColor(p.textDim, p.bg);
  snprintf(buf, sizeof(buf), "%u run  %u total",
           tama.sessionsRunning, tama.sessionsTotal);
  spr.drawString(buf, CX, 200);
  busyTokFmt(tama.tokensToday, tb, sizeof(tb));
  snprintf(buf, sizeof(buf), "today %s tok", tb);
  spr.drawString(buf, CX, 214);

  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

// Render the busy-landscape info column + buddy into a target surface. Used
// for both the off-screen sprite (preferred) and the direct-LCD fallback.
static void busyLandscapeCompose(TFT_eSPI* t, const Palette& p) {
  // Buddy on the left — portrait coords (center x67, y~30) land upper-left.
  if (buddyMode) buddyRenderTo(t, activeState);
  else { characterSetState(activeState); characterRenderTo(t, 57, 45); }

  t->setTextDatum(MC_DATUM);
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  t->setTextSize(3); t->setTextColor(p.text, p.bg);
  t->drawString(hm, 182, 30);
  t->setTextSize(1); t->setTextColor(p.body, p.bg);
  t->drawString("working", 182, 54);

  int y = 72;
  if (tama.activeTitle[0]) {
    char tl[2][24];
    uint8_t n = wrapInto(tama.activeTitle, tl, 2, 18);
    t->setTextColor(p.text, p.bg);
    for (uint8_t i = 0; i < n; i++) { t->drawString(tl[i], 182, y); y += 12; }
  }

  char buf[24], tb[12];
  t->setTextColor(p.textDim, p.bg);
  snprintf(buf, sizeof(buf), "%u run  %u tot", tama.sessionsRunning, tama.sessionsTotal);
  t->drawString(buf, 182, 104);
  busyTokFmt(tama.tokensToday, tb, sizeof(tb));
  snprintf(buf, sizeof(buf), "today %s", tb);
  t->drawString(buf, 182, 118);
  t->setTextDatum(TL_DATUM);
  t->setTextSize(1);
}

// Buddy-at-work screen (landscape). Composited off-screen into landSpr and
// pushed in one shot so the working buddy's animation doesn't flicker the way
// direct-to-LCD drawing does. Falls back to direct LCD only if the sprite
// can't be allocated.
static void drawBusyPanelLandscape(const Palette& p) {
  // ~20fps is plenty for the buddy and keeps the full-frame SPI push light.
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw < 48) return;
  lastDraw = millis();

  if (landSpr.width() == 0) {
    landSpr.setColorDepth(16);
    landSpr.createSprite(240, 135);   // ~64KB; may fail under pressure
  }

  M5.Lcd.setRotation(clockOrient);
  if (landSpr.width() != 0) {
    landSpr.fillSprite(p.bg);
    busyLandscapeCompose(&landSpr, p);
    landSpr.pushSprite(0, 0);
  } else {
    // Fallback: no buffer available — draw straight to the LCD (may flicker).
    static uint8_t painted = 0xFE;
    if (painted != clockOrient) { M5.Lcd.fillScreen(p.bg); painted = clockOrient; }
    busyLandscapeCompose(&M5.Lcd, p);
  }
  M5.Lcd.setRotation(0);
}

static void drawTerminalBoot(uint32_t elapsed) {
  spr.fillSprite(0x0000);
  spr.setTextColor(0x07E0, 0x0000); // green
  spr.setTextSize(1);
  spr.setTextDatum(TL_DATUM);
  
  int y = 10;
  spr.drawString("> BOOTING HERMES-OS...", 6, y); y += 15;
  if (elapsed > 150) { spr.drawString("  CPU: ESP32 @ 160MHz", 6, y); y += 15; }
  if (elapsed > 300) {
    char buf[32]; snprintf(buf, sizeof(buf), "  DRAM: %d KB FREE", ESP.getFreeHeap() / 1024);
    spr.drawString(buf, 6, y); y += 15;
  }
  if (elapsed > 450) {
    char buf[32]; snprintf(buf, sizeof(buf), "  PSRAM: %d KB FREE", ESP.getFreePsram() / 1024);
    spr.drawString(buf, 6, y); y += 15;
  }
  if (elapsed > 600) {
    char buf[32]; snprintf(buf, sizeof(buf), "  FS FREE: %d KB", (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
    spr.drawString(buf, 6, y); y += 15;
  }
  if (elapsed > 750) {
    char buf[32]; snprintf(buf, sizeof(buf), "  PET: %s (%s)", petName(), buddyMode ? "ASCII" : "GIF");
    spr.drawString(buf, 6, y); y += 15;
  }
  if (elapsed > 900) {
    char buf[32]; snprintf(buf, sizeof(buf), "  WIFI: %s", settings().wifiSsid);
    spr.drawString(buf, 6, y); y += 15;
  }
  if (elapsed > 1050) { spr.drawString("  STATUS: READY", 6, y); y += 15; }
  
  if (elapsed < 1200 && (elapsed / 250) % 2 == 0) {
    int curX = 6;
    if (elapsed > 1050) curX += 90;
    else if (elapsed > 900) curX += 80;
    spr.fillRect(curX, y - 13, 6, 10, 0x07E0);
  }
}

static void drawGraphicWelcome(uint32_t elapsed, bool wifiConnected, int frame, const Palette& p) {
  spr.fillSprite(p.bg);
  
  int cx = W / 2;
  int cy = H / 2 - 15;
  
  // Outer arcs rotating clockwise
  float angle1 = (elapsed * 0.08f);
  spr.drawArc(cx, cy, 46, 48, angle1, angle1 + 100, p.textDim);
  spr.drawArc(cx, cy, 46, 48, angle1 + 180, angle1 + 280, p.textDim);
  
  // Inner arcs rotating counter-clockwise
  float angle2 = -(elapsed * 0.12f);
  spr.drawArc(cx, cy, 34, 36, angle2, angle2 + 120, p.ink ? p.ink : p.textDim);
  spr.drawArc(cx, cy, 34, 36, angle2 + 180, angle2 + 300, p.ink ? p.ink : p.textDim);
  
  if (buddyMode) {
    buddyRenderTo(&spr, P_CELEBRATE);
  } else if (characterLoaded()) {
    characterRenderTo(&spr, cx, cy);
  }
  
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.drawString("HERMES", cx, 28);
  
  spr.setTextSize(1);
  spr.setTextColor(p.body, p.bg);
  spr.drawString("B U D D Y", cx, 46);
  
  if (ownerName()[0]) {
    char line[64];
    snprintf(line, sizeof(line), "%s's companion", ownerName());
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(line, cx, H - 55);
    
    spr.setTextSize(2);
    spr.setTextColor(p.text, p.bg);
    spr.drawString(petName(), cx, H - 40);
  } else {
    spr.setTextColor(p.text, p.bg);
    spr.drawString("A buddy appears...", cx, H - 45);
  }
  
  int wx = W - 15, wy = 12;
  if (wifiConnected) {
    spr.fillCircle(wx, wy, 2, 0x07E0);
    spr.drawCircle(wx, wy, 5, 0x07E0);
    spr.drawCircle(wx, wy, 8, 0x07E0);
  } else {
    uint16_t wifiCol = (frame / 10) % 2 ? p.textDim : 0xFFE0;
    spr.fillCircle(wx, wy, 2, wifiCol);
    if ((frame / 5) % 3 >= 1) spr.drawCircle(wx, wy, 5, wifiCol);
    if ((frame / 5) % 3 >= 2) spr.drawCircle(wx, wy, 8, wifiCol);
  }
  
  float progress = (elapsed - 1200.0f) / 2300.0f;
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;
  
  int barW = W - 30;
  int barX = 15;
  int barY = H - 20;
  spr.drawRoundRect(barX, barY, barW, 6, 3, p.textDim);
  int fillW = (int)(progress * (barW - 4));
  if (fillW > 0) {
    spr.fillRoundRect(barX + 2, barY + 2, fillW, 2, 1, p.body);
  }
}

void setup() {
  nvsMutex = xSemaphoreCreateMutex();
  tamaMutex = xSemaphoreCreateMutex();
  voiceMutex = xSemaphoreCreateMutex();
  M5.begin();
  Serial.printf("Reset Reason: %s\n", resetReasonStr());
  Serial.printf("PSRAM: size=%u free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("DRAM: size=%u free=%u max=%u\n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  M5.Lcd.setRotation(0);
  M5.Imu.Init();
  M5.Beep.begin();
  // Set mic gain BEFORE begin (magnification: 1-255, default 16)
  {
    auto cfg = M5.Mic.config();
    cfg.magnification = 32;
    M5.Mic.config(cfg);
  }
  M5.Mic.begin();
  Serial.printf("Mic gain=%u\n", M5.Mic.config().magnification);
  startBt();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);   // off
  applyBrightness();
  lastInteractMs = millis();

  recBuffer = nullptr;

  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  // Sprite uses DMA-capable DRAM. Retry on failure, report heap state.
  {
    int retry = 0;
    for (; retry < 5; retry++) {
      spr.createSprite(W, H);
      if (spr.width() > 0) break;
      Serial.printf("sprite fail retry %d heap=%u max=%u psram=%u\n",
                    retry, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
      delay(200);
    }
    if (spr.width() == 0) {
      Serial.println("WARN: sprite not available, rendering direct to LCD");
    }
  }
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  if (!settings().configured) {
    runSetupWizard();
  }

  {
    const Palette& p = characterPalette();
    
    // Play a cool high-tech startup chirp!
    if (settings().sound) {
      M5.Beep.tone(800, 50); delay(50);
      M5.Beep.tone(1200, 50); delay(50);
      M5.Beep.tone(1600, 100);
    }

    // Start WiFi connection
    WiFi.begin(settings().wifiSsid, settings().wifiPass);
    WiFi.setAutoReconnect(true);

    uint32_t startMs = millis();
    uint32_t durationMs = 3500; // extended for 3-phase show
    uint32_t frame = 0;

    // Open mascot for character if in GIF mode
    if (!buddyMode && characterLoaded()) {
      characterSetState(P_CELEBRATE);
    }

    while (millis() - startMs < durationMs) {
      bool wifiConnected = (WiFi.status() == WL_CONNECTED);
      uint32_t elapsed = millis() - startMs;
      
      if (spr.width() > 0) {
        if (elapsed < 1200) {
          // Phase 1: Terminal booting
          drawTerminalBoot(elapsed);
        } else if (elapsed < 1700) {
          // Phase 2: Split-screen sweep wipe
          float tSweep = (elapsed - 1200.0f) / 500.0f;
          int ySweep = (int)(tSweep * H);
          
          spr.setClipRect(0, 0, W, ySweep);
          drawGraphicWelcome(elapsed, wifiConnected, frame, p);
          
          spr.setClipRect(0, ySweep, W, H - ySweep);
          drawTerminalBoot(elapsed);
          
          spr.clearClipRect();
          spr.drawFastHLine(0, ySweep, W, 0x07FF); // Cyan laser
          spr.drawFastHLine(0, ySweep - 1, W, 0xFFFF); // White core
          spr.drawFastHLine(0, ySweep + 1, W, 0x07FF);
        } else {
          // Phase 3: Graphic welcome
          drawGraphicWelcome(elapsed, wifiConnected, frame, p);
        }
        spr.pushSprite(0, 0);
      } else {
        M5.Lcd.fillScreen(p.bg);
        M5.Lcd.setTextDatum(MC_DATUM);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(p.text, p.bg);
        M5.Lcd.drawString("HERMES BUDDY", W/2, H/2 - 20);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(p.textDim, p.bg);
        M5.Lcd.drawString(wifiConnected ? "connected" : "connecting...", W/2, H/2 + 10);
      }
      
      frame++;
      delay(30);
    }
    spr.setTextDatum(TL_DATUM);
    spr.setTextSize(1);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
  xTaskCreatePinnedToCore(networkTask, "networkTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  M5.update();
  M5.Beep.update();
  
  static VoiceState lastVoiceState = VOICE_NONE;
  if (voiceState != lastVoiceState) {
    if (voiceState == VOICE_RESPONSE) {
      voiceResponseStartMs = millis();
      voiceResponseScroll = 0;
    }
    lastVoiceState = voiceState;
  }
  
  // Stop and send audio once recording is finished (4s limit reached)
  if (voiceState == VOICE_RECORDING && recStarted && !M5.Mic.isRecording()) {
    voiceStopAndSend();
  }
  // Auto-stop recording after max duration
  if (voiceState == VOICE_RECORDING && millis() - voiceRecStart >= REC_MAX_MS) {
    voiceStopAndSend();
  }
  
  // Recreate sprite on main thread when background processing frees recBuffer
  if (voiceState == VOICE_PROCESSING && !recBuffer && spr.width() == 0) {
    spr.createSprite(W, H);
    Serial.println("[voice] spr recreated on main thread");
  }
  // Finish a cooperative cancel: the task has freed the buffer and flipped
  // back to NONE. Now it's safe to recreate the sprite and clear leftovers.
  if (voiceCancelling && voiceState == VOICE_NONE && !recBuffer) {
    voiceCancelling = false;
    voiceCancelRequested = false;
    voiceResponse[0] = 0;
    voiceTranscript[0] = 0;
    voiceError[0] = 0;
    if (spr.width() == 0) {
      spr.createSprite(W, H);
      Serial.println("[voice] spr recreated after cancel unwind");
    }
  }
  
  t++;
  uint32_t now = millis();

  if (tamaMutex && xSemaphoreTake(tamaMutex, 0) == pdTRUE) {
    tama = networkTama;
    xSemaphoreGive(tamaMutex);
  }
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on active prompt or attention state, otherwise off
  bool promptActive = (tama.promptId[0] && !responseSent);
  if ((activeState == P_ATTENTION || promptActive) && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? HIGH : LOW);
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = factoryResetWarningOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Expired double-press timer → execute deferred single-press action
  if (btnADoublePending && (millis() - lastBtnAReleaseTime) > DOUBLE_PRESS_MS) {
    btnADoublePending = false;
    if (voiceState == VOICE_NONE) {
      if (inPrompt) {
        sendApprovalAsync("once");
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
      } else if (sessionsOpen) {
        beep(1800, 30);
        uint8_t n = tama.sessLineCount;
        uint8_t max = n > 6 ? n - 6 : 0;
        sessScroll = (sessScroll >= max) ? 0 : sessScroll + 1;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % menuCount();
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    // On the response/error screen a lone A does nothing — only a double A
    // continues the conversation (handled in wasReleased), B closes.
  }

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    btnADoublePending = false;
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA && voiceState == VOICE_NONE) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; spr.fillSprite(0x0000); }
    else if (factoryResetWarningOpen) { factoryResetWarningOpen = false; characterInvalidate(); spr.fillSprite(0x0000); }
    else if (sessionsOpen) { sessionsOpen = false; characterInvalidate(); }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    uint32_t brel = millis();
    if (!btnALong && !swallowBtnA) {
      if (voiceState == VOICE_RECORDING) {
        // Single A during recording → stop and send
        voiceStopAndSend();
        btnADoublePending = false;
      } else if (voiceState == VOICE_RESPONSE || voiceState == VOICE_ERROR) {
        // Double A → continue the conversation with a new recording; a
        // lone A (after the window) dismisses, run from the deferred timer.
        if (btnADoublePending && (brel - lastBtnAReleaseTime) < DOUBLE_PRESS_MS) {
          btnADoublePending = false;
          voiceContinue();
        } else {
          btnADoublePending = true;
          lastBtnAReleaseTime = brel;
        }
      } else if (voiceState == VOICE_PROCESSING) {
        // Single A during processing → cancel
        voiceCancel();
        btnADoublePending = false;
        beep(600, 60);
      } else {
        // Normal mode: double-press to start voice recording
        // If a menu or prompt is active, process the press immediately to avoid lag and double-press voice record triggers.
        if (inPrompt || resetOpen || factoryResetWarningOpen || sessionsOpen || settingsOpen || menuOpen) {
          btnADoublePending = false;
          if (inPrompt) {
            sendApprovalAsync("once");
            responseSent = true;
            uint32_t tookS = (millis() - promptArrivedMs) / 1000;
            statsOnApproval(tookS);
            beep(2400, 60);
            if (tookS < 5) triggerOneShot(P_HEART, 2000);
          } else if (resetOpen) {
            beep(1800, 30);
            resetSel = (resetSel + 1) % RESET_N;
          } else if (factoryResetWarningOpen) {
            if (factoryResetDoublePending && (brel - lastResetReleaseTime) < DOUBLE_PRESS_MS) {
              factoryResetDoublePending = false;
              factoryResetWarningOpen = false;
              executeFactoryReset();
            } else {
              factoryResetDoublePending = true;
              lastResetReleaseTime = brel;
              beep(1800, 50);
            }
          } else if (sessionsOpen) {
            beep(1800, 30);
            uint8_t n = tama.sessLineCount;
            uint8_t max = n > 6 ? n - 6 : 0;
            sessScroll = (sessScroll >= max) ? 0 : sessScroll + 1;
          } else if (settingsOpen) {
            beep(1800, 30);
            settingsSel = (settingsSel + 1) % SETTINGS_N;
          } else if (menuOpen) {
            beep(1800, 30);
            menuSel = (menuSel + 1) % menuCount();
          }
        } else {
          if (btnADoublePending && (brel - lastBtnAReleaseTime) < DOUBLE_PRESS_MS) {
            btnADoublePending = false;
            hermesSessionId[0] = 0;   // fresh conversation, drop prior session
            voiceStartRecording();
          } else {
            btnADoublePending = true;
            lastBtnAReleaseTime = brel;
          }
        }
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB
  if (M5.BtnB.pressedFor(600) && !btnBLong && !swallowBtnB && voiceState == VOICE_RESPONSE) {
    btnBLong = true;
    voiceCancel();
    btnADoublePending = false;
    beep(600, 60);
  }

  if (M5.BtnB.wasReleased()) {
    bool wasLong = btnBLong;
    btnBLong = false;
    if (swallowBtnB) { swallowBtnB = false; }
    else if (!wasLong) {
      btnADoublePending = false;
      if (voiceState != VOICE_NONE) {
        if (voiceState == VOICE_RESPONSE) {
          char wr[48][24];
          uint8_t n = wrapInto(voiceResponse, wr, 48, 19);
          const Palette& p = characterPalette();
          char q[2][24];
          uint8_t qn = voiceTranscript[0] ? wrapInto(voiceTranscript, q, 2, 20) : 0;
          int qStripH = 14 + (qn > 0 ? qn : 1) * 10;
          int hy = 6 + qStripH + 8;
          const int ty = hy + 20, lh = 11;
          uint8_t maxRows = (uint8_t)((H - 24 - ty) / lh);

          uint32_t shown = (millis() - voiceResponseStartMs) / 110;
          if (shown < n) {
            // Skip reveal typing animation
            voiceResponseStartMs = millis() - (n * 110);
            beep(1800, 30);
          } else {
            if (n <= maxRows) {
              voiceCancel();
              btnADoublePending = false;
              beep(600, 60);
            } else {
              if (voiceResponseScroll + maxRows < n) {
                uint8_t step = maxRows > 2 ? maxRows - 2 : 1;
                voiceResponseScroll += step;
                if (voiceResponseScroll + maxRows > n) {
                  voiceResponseScroll = n - maxRows;
                }
                beep(1800, 30);
              } else {
                voiceCancel();
                btnADoublePending = false;
                beep(600, 60);
              }
            }
          }
        } else {
          voiceCancel();
          btnADoublePending = false;
          beep(600, 60);
        }
      }
      else
      if (inPrompt) {
        sendApprovalAsync("deny");
        responseSent = true;
        statsOnDenial();
        beep(600, 60);
      } else if (resetOpen) {
        beep(2400, 30);
        applyReset(resetSel);
      } else if (factoryResetWarningOpen) {
        beep(600, 60);
        factoryResetWarningOpen = false;
        characterInvalidate();
        spr.fillSprite(0x0000);
      } else if (sessionsOpen) {
        beep(2400, 30);
        sessionsOpen = false;
        characterInvalidate();
      } else if (settingsOpen) {
        beep(2400, 30);
        applySetting(settingsSel);
      } else if (menuOpen) {
        beep(2400, 30);
        menuConfirm();
      } else if (displayMode == DISP_INFO) {
        beep(2400, 30);
        infoPage = (infoPage + 1) % INFO_PAGES;
      } else if (displayMode == DISP_PET) {
        beep(2400, 30);
        petPage = (petPage + 1) % PET_PAGES;
        applyDisplayMode();
      } else {
        beep(2400, 30);
        msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
      }
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Hermes data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && voiceState == VOICE_NONE;
  // Busy panel: Hermes is running. Same screen split as the resting clock
  // (buddy peeks up top, info panel below) but with the working buddy and
  // live session stats. Mutually exclusive with clocking (needs running>0).
  bool busyPanel = displayMode == DISP_NORMAL
                && !menuOpen && !settingsOpen && !resetOpen && !sessionsOpen
                && !inPrompt && !blePasskey()
                && voiceState == VOICE_NONE
                && tama.sessionsRunning > 0;
  // Both the clock and the busy panel react to orientation and want the buddy
  // shrunk to peek size at the top in portrait.
  bool clockLike = clocking || busyPanel;
  if (clockLike) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscape = clockLike && clockOrient != 0;
  bool peekTop = clockLike && !landscape;

  static bool wasPeekTop = false;
  static bool wasLandscape = false;
  if (peekTop != wasPeekTop || landscape != wasLandscape) {
    if (landscape) {
      // Portrait sprite is unused in landscape — free its ~64KB so the
      // landscape buffer has room (one 64KB sprite live at a time).
      if (spr.width() != 0) spr.deleteSprite();
    } else {
      if (landSpr.width() != 0) landSpr.deleteSprite();
      if (spr.width() == 0) spr.createSprite(W, H);
      if (peekTop) characterSetPeek(true);
      else applyDisplayMode();
    }
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasPeekTop = peekTop;
    wasLandscape = landscape;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Recording stops via double-press A or auto-timeout (REC_MAX_MS)

  if (voiceState != VOICE_NONE) {
    if (voiceState == VOICE_RECORDING) drawVoiceRecording();
    else if (voiceState == VOICE_PROCESSING) drawVoiceProcessing();
    else if (voiceState == VOICE_RESPONSE) drawVoiceResponse();
    else if (voiceState == VOICE_ERROR) {
      const Palette& p = characterPalette();
      spr.fillSprite(p.bg);
      spr.setTextDatum(MC_DATUM);
      spr.setTextColor(HOT, p.bg);
      spr.setTextSize(2);
      spr.drawString("Error", CX, H/2 - 30);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString(voiceError, CX, H/2 - 10);
      if (voiceTranscript[0]) spr.drawString(voiceTranscript, CX, H/2 + 5);
      spr.drawString("AA retry  B close", CX, H/2 + 30);
      spr.setTextDatum(TL_DATUM);
      drawInsetBorder(&spr, p.textDim);
    }
  } else if (napping || screenOff || landscape) {
    // skip sprite render — face-down, powered off, or a landscape panel
    // (clock or busy, both draw direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscape) {
    // Clock and busy share paintedOrient/lastSec; force a clean repaint when
    // the landscape owner flips so the other's glyphs don't linger.
    static bool prevLandClock = false;
    if (clocking != prevLandClock) { paintedOrient = 0; prevLandClock = clocking; }
    if (clocking) drawClock();
    else          drawBusyPanelLandscape(characterPalette());
  } else if (!napping && !screenOff) {
    if (voiceState == VOICE_NONE) {
      if (blePasskey()) drawPasskey();
      else if (clocking) drawClock();
      else if (displayMode == DISP_INFO) drawInfo();
      else if (displayMode == DISP_PET) drawPet();
      else if (busyPanel) drawBusyPanel(characterPalette());
      else                drawHUD();
      if (resetOpen) drawReset();
      else if (factoryResetWarningOpen) drawFactoryResetWarning();
      else if (sessionsOpen) drawSessions();
      else if (settingsOpen) drawSettings();
      else if (menuOpen) drawMenu();
      spr.pushSprite(0, 0);
    } else if (voiceState == VOICE_RESPONSE || voiceState == VOICE_ERROR) {
      spr.pushSprite(0, 0);
    }
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt && voiceState == VOICE_NONE) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Axp.ScreenBreath(8);
    dimmed = true;
    statsSave(); // Salva i token e le statistiche accumulate all'inizio del sonno
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && voiceState == VOICE_NONE && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
    statsSave(); // Salva i token e le statistiche accumulate allo spegnimento dello schermo
  }

  delay(screenOff ? 100 : 16);
}
