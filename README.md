# 🤖 Hermes M5Stick Firmware

**Firmware per M5StickC Plus 2** — un companion device fisico che si collega via Wi-Fi a **Hermes Agent**, monitora le sessioni, mostra dettagli token, e permette l'invio di comandi vocali.

![M5StickC Plus 2](https://img.shields.io/badge/hardware-M5StickC%20Plus%202-1a73e8?style=flat-square)
![PlatformIO](https://img.shields.io/badge/framework-PlatformIO-ff5a00?style=flat-square)
![ESP32](https://img.shields.io/badge/soc-ESP32--S3-black?style=flat-square)

---

## ✨ Funzionalità

- 📊 **Dashboard in tempo reale** — mostra sessioni correnti, token usati, stato connessione
- 🎤 **Invio vocale** — doppio click A per registrare (max 4s), trascritto via **Groq STT** e inviato a Hermes
- 🔔 **Notifiche visive e sonore** — LED, beep, animazioni, orientamento automatico del display
- 🐾 **18 animali ASCII** + supporto **GIF characters** via SPIFFS (es. `bufo`)
- 🔄 **Stati animati** — sleep, idle, busy, celebrate, dizzy, heart
- 📟 **Orologio** — sincronizzazione NTP, orientamento auto portrait/landscape
- ⚙️ **Menu impostazioni** — luminosità, suono, LED, informazioni Wi-Fi/Hermes

## 🏗️ Architettura

```
                  +--------------------------------+
                  |      M5StickC PLUS2            |
                  |  (Hermes Desktop Buddy)        |
                  +---------------+----------------+
                                  |
                                  | Wi-Fi (HTTP GET/POST)
                                  v
                  +---------------+----------------+
                  |     Hermes API Gateway         |
                  |       (Porta 8642)             |
                  +--------------------------------+
```

### Endpoint API utilizzati

| Endpoint | Metodo | Descrizione |
|----------|--------|-------------|
| `/v1/runs/current` | GET | Polling stato run corrente |
| `/v1/runs/current/stop` | POST | Ferma run in esecuzione |
| `/v1/chat/completions` | POST | Invia messaggi vocali/testo |
| `/api/sessions` | GET | Lista sessioni Hermes |

### Pipeline Vocale

```
Microfono M5Stick → Groq STT (whisper-large-v3-turbo) → Hermes Chat → Risposta su display
```

## 🎮 Controlli

| Input | Contesto | Azione |
|-------|----------|--------|
| **A** (frontale) | Normale / Info | Schermata successiva |
| **Doppio A** | Normale | **Avvia registrazione vocale** |
| **A lungo** | Qualsiasi | Apre menu impostazioni |
| **B** (laterale) | Normale / Info | Pagina successiva / Indietro |
| **Power** (breve) | Qualsiasi | Spegne/accende display |
| **Power** (lungo) | Qualsiasi | Spegne dispositivo |
| **Scuotimento** | Normale | Attiva stato "dizzy" |
| **Face-down** | Normale | Attiva modalità risparmio energetico "nap" |

## 📟 Stati

| Stato | Trigger | Descrizione |
|-------|---------|-------------|
| `sleep` | Wi-Fi/API disconnesso | Occhi chiusi, low-power |
| `idle` | Connesso, nessun run attivo | Animazione rilassata (cambia in base all'umore) |
| `busy` | Run in esecuzione | Sudore, animazioni di lavoro |
| `celebrate` | Level up (50K token) o buon umore | Festa, balzi |
| `dizzy` | Scuotimento | Occhi a spirale |
| `heart` | Mood alto | Cuoricini volanti |

### Sistema Pet (Mood & Energia)

- **Livello**: sale ogni 50K token processati
- **Mood** (0-4 cuori): scala in base al tempo dall'ultimo utilizzo (≤2h = 4 cuori, >48h = 0)
- **Energia** (0-5): parte da 3 al boot, si ricarica a 5 quando si esce dalla pausa "nap", cala di 1 ogni 2 ore
- **Fed bar**: progresso verso il prossimo level up

## 🛠️ Requisiti

- **Hardware**: M5StickC Plus 2 (con microfono PDM e speaker integrati)
- **Software**: [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) (`pip install platformio`)
- **Backend**: Hermes Agent con API Server abilitato + **Groq API key** per STT vocale

## 🚀 Setup & Flash

1. Clona il repo:
   ```bash
   git clone https://github.com/Syax89/hermes-m5stick-firmware.git
   cd hermes-m5stick-firmware
   ```

2. Installa le dipendenze e compila:
   ```bash
   pio pkg install
   pio run --environment m5stickc-plus
   ```

3. Carica il firmware via USB:
   ```bash
   pio run --environment m5stickc-plus -t upload
   ```

4. Per cancellare NVS e fare un flash pulito:
   ```bash
   pio run --environment m5stickc-plus -t erase && pio run -t upload
   ```

5. Per caricare un character GIF (es. `bufo`) su SPIFFS, usa il **desktop bridge** o:
   ```bash
   pio run --environment m5stickc-plus -t uploadfs
   ```

## 📦 Struttura del Progetto

```
hermes-m5stick-firmware/
├── src/
│   ├── main.cpp               # Loop principale, state machine, UI screens
│   ├── buddy.cpp/h            # Dispatch e render animali ASCII
│   ├── buddy_common.h         # Strutture dati comuni ai buddies
│   ├── buddies/               # 18 animali ASCII (robot, capybara, cat, duck, etc.)
│   ├── character.cpp/h        # Decodifica e render GIF
│   ├── data.h                 # Client Wi-Fi, polling API, audio pipeline
│   ├── stats.h                # Statistiche NVS, settings, livelli, mood
│   ├── ble_bridge.cpp/h       # Bluetooth LE (presente ma non utilizzato attualmente)
│   ├── xfer.h                 # Trasferimento character GIF via seriale
│   ├── setup_wizard.h         # Configurazione Wi-Fi guidata
│   ├── M5StickCPlus.cpp/h     # Fix init statico M5 wrapper
├── characters/                # Pacchetti character GIF (es. bufo)
│   └── bufo/                  # 13 frame + manifest.json + README.md
├── scripts/
│   └── patch_dfr.py           # Auto-patch per compatibilità ESP32 Arduino Core v3+
├── platformio.ini             # Configurazione PlatformIO
└── README.md
```

## 🔧 Risoluzione Problemi

### Compilazione — `analogWriteResolution` error
Se incontri:
```
error: too many arguments to function 'void analogWriteResolution(uint8_t)'
```
Lo script `scripts/patch_dfr.py` lo risolve automaticamente. Se non funziona, applica manualmente:
```bash
sed -i 's/analogWriteResolution(_pin0,10);/analogWriteResolution(10);/' \
  .pio/libdeps/m5stickc-plus/DFRobot_GP8XXX/DFRobot_GP8XXX.cpp
```

### Prima accensione
Il primo avvio mostra il **WiFi Setup Wizard** per configurare SSID, password, IP di Hermes e API key. La configurazione avviene tramite seriale USB.

## 🤝 Contributi

Sentiti libero di aprire issue e PR! Questo è un progetto in evoluzione 🚀

## 📄 Licenza

MIT
