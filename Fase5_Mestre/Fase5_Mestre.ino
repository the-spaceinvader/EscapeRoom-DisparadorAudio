/*
 * Project : Escape room corridor audio device
 * Phase   : 5 - Master with configuration mode (AP + webapp), live calibration and debugging
 * Target  : M5Stack AtomS3 Lite (board "M5AtomS3") + ABC Port
 *           + Unit Mini ToF-90 (U196, P.A) + Unit AudioPlayer (U197, P.C)
 * Author  : <author>
 * Date    : <date>
 *
 * States: Ready -> Playing -> Waiting -> FadeOut -> Rearm -> Ready (see SPEC.md).
 * Entry passage = Master ToF barrier (Phase 1). Exit passage = message from the Remote
 * over ESP-NOW. Audio = direct UART frames on Serial1 (Phase 2). Parameters are kept in NVS.
 *
 * Modes (SPEC.md):
 *   Normal         the AP and the webapp are off (always the mode at boot).
 *   Configuration  a long press (LONG_PRESS_MS) on the button toggles the AP. The AP switches
 *                  itself off after AP_IDLE_TIMEOUT_MS without HTTP requests. The game keeps
 *                  running normally; the AP uses the same Wi-Fi channel as ESP-NOW.
 *
 * Core 1 (loop):  barrier, audio, game state machine, LED, Serial.
 * Core 0 (tasks): "comm"  ESP-NOW: validates, deduplicates (bootId, seq) and acknowledges the
 *                         Remote's exit events, pushes the Remote's corridor width, renews the
 *                         live-range lease. Wire format: see Protocol.h (identical Remote copy).
 *                 "web"   AP on/off and the HTTP server (WebServer). It never touches the game
 *                         variables: it reads a snapshot published by the loop and sends
 *                         commands to the loop through a queue.
 * Remote offline failsafe: in Playing, if the Remote has been offline for the max wait,
 * the fade-out starts.
 *
 * LED (one dim RGB LED, see serviceLed): game state / errors as the base colour, white pulses
 * on top while the AP is on (1 pulse per 3 s without clients, 2 with a client).
 *
 * Serial command over USB (115200 baud, line terminated):
 *   ?          print state, parameters and remaining times
 *
 * Libraries: M5Unified, M5GFX, M5Unit-TOF (+ M5UnitUnified, M5Utility, M5HAL), ArduinoJson.
 * Serial Monitor: set Tools > USB CDC On Boot = Enabled, baud 115200.
 * Partition scheme: if the sketch does not fit, use Tools > Partition Scheme > "Huge APP".
 */

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedTOF.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <atomic>
#include <stdarg.h>
#include "Protocol.h"
#include "WebPage.h"

// ---------------------------------------------------------------- Pins and buses
constexpr int      PIN_SDA      = 38;   // ABC Port P.A, yellow
constexpr int      PIN_SCL      = 39;   // ABC Port P.A, white
constexpr int      AUDIO_RX_PIN = 6;    // ESP32 RX (unit TX)
constexpr int      AUDIO_TX_PIN = 5;    // ESP32 TX (unit RX)
constexpr uint32_t SERIAL_BAUD  = 115200;
constexpr uint32_t AUDIO_BAUD   = 9600;
constexpr uint32_t I2C_FREQ_HZ  = 400000;
constexpr uint8_t  TOF_I2C_ADDRESS = 0x29;

// ---------------------------------------------------------------- Barrier parameters (Phase 1)
constexpr int      THRESHOLD_MARGIN_CM = 30;
constexpr int      MIN_THRESHOLD_CM    = 5;
constexpr int      MAX_THRESHOLD_CM    = 120;
constexpr int      HYSTERESIS_CM       = 10;
constexpr uint8_t  OCCUPIED_CONFIRM    = 2;
constexpr uint32_t REARM_MS            = 500;
constexpr uint32_t NO_DATA_WARN_MS     = 1000;

// ---------------------------------------------------------------- Audio timing (Phase 2)
constexpr uint32_t BOOT_WAIT_MS     = 5000;
constexpr uint32_t CMD_GAP_MS       = 600;
constexpr uint32_t QUERY_TIMEOUT_MS = 1000;
constexpr uint32_t RX_IDLE_MS       = 30;
// Fixed time per volume level. 50 ms: clean in the Phase 3 step test (100 and 50 ms,
// plain and verified); 33 and 20 ms were not tested and are left as margin.
constexpr uint32_t FADE_IN_STEP_MS  = 50;
constexpr uint32_t FADE_OUT_STEP_MS = FADE_IN_STEP_MS;  // floor between volume commands in a fade-out

// ---------------------------------------------------------------- Configuration mode (Phase 5)
constexpr uint32_t LONG_PRESS_MS        = 2000;
constexpr uint32_t AP_IDLE_TIMEOUT_MS   = 10UL * 60UL * 1000UL;   // 10 min without HTTP requests
constexpr char     AP_SSID[]            = "EscapeRoom-Audio";
constexpr char     AP_PASSWORD[]        = "mission2escape";       // fixed WPA2 password (8..63 chars)
constexpr uint8_t  AP_MAX_CLIENTS       = 4;
const IPAddress    AP_IP(192, 168, 4, 1);
const IPAddress    AP_MASK(255, 255, 255, 0);
constexpr uint16_t LOG_CAPACITY         = 200;    // events kept in RAM (not persistent)
constexpr uint8_t  LOG_TEXT_LEN         = 96;
constexpr uint8_t  LOG_PAGE_ENTRIES     = 40;     // max entries per /api/log reply
constexpr uint32_t SNAPSHOT_PERIOD_MS   = 100;
constexpr uint32_t CALIB_ACTIVE_MS      = 3000;   // calibration tab counts as open for this long after a poll
constexpr uint8_t  LED_LEVEL            = 24;     // 0..255, kept low: the LED can be seen in a dark corridor

// ---------------------------------------------------------------- Adjustable parameters (NVS)
constexpr char     NVS_NAMESPACE[] = "escape";
constexpr uint8_t  DEF_VOLUME = 20,  MIN_VOLUME = 1,    MAX_VOLUME = 30;
constexpr uint16_t DEF_FADE_OUT_MS = 3000, MIN_FADE_OUT_MS = 500, MAX_FADE_OUT_MS = 10000;
constexpr uint16_t DEF_SILENCE_S = 10, MIN_SILENCE_S = 1, MAX_SILENCE_S = 120;
constexpr uint16_t DEF_MAX_WAIT_S = 60, MAX_MAX_WAIT_S = 600;
// Corridor width limits (WIDTH_MIN_CM, WIDTH_MAX_CM, WIDTH_DEFAULT_CM) come from Protocol.h.

struct Params {
    uint8_t  volume;
    uint16_t fadeOutMs;
    uint16_t silenceS;
    uint16_t maxWaitS;
    uint16_t widthCm;          // entry barrier (Master)
    uint16_t widthRemoteCm;    // exit barrier (Remote), pushed over ESP-NOW
};
Params params;

// ---------------------------------------------------------------- Protocol frames
constexpr uint8_t CMD_PLAY_CTRL = 0x04;
constexpr uint8_t CMD_VOLUME    = 0x06;
constexpr uint8_t CMD_LOOP_MODE = 0x0B;

// ---------------------------------------------------------------- Types and globals
enum class GameState : uint8_t { Ready, Playing, Waiting, FadeOut, Rearm };
enum class CardState : uint8_t { Waiting, Checking, Ok, Error };
enum class FadeState : uint8_t { None, Out, In };
enum class BarrierState : uint8_t { Free, Occupied };

const char* const GAME_STATE_NAMES[] = {"READY", "PLAYING", "WAITING", "FADE-OUT", "REARM"};
const char* const CARD_STATE_NAMES_PT[] = {"a aguardar", "a verificar", "pronto", "erro"};

m5::unit::UnitUnified Units;
m5::unit::UnitToF90 tof;

GameState gameState = GameState::Ready;
uint32_t  windowStartMs  = 0;   // last passage while Waiting
uint32_t  maxWaitStartMs = 0;   // first exit passage of the cycle
uint32_t  playingSinceMs = 0;   // last entry into Playing (Remote offline failsafe)

// Remote link (shared with the communication task)
constexpr uint8_t RX_QUEUE_SIZE   = 8;
constexpr uint8_t EXIT_QUEUE_SIZE = 16;
struct RxItem    { uint8_t data[16]; uint8_t len; uint32_t rxMs; };
struct ExitEvent { uint32_t eventMs; uint16_t seq; uint16_t ageMs; };
QueueHandle_t rxQueue   = nullptr;   // Wi-Fi callback -> core 0
QueueHandle_t exitQueue = nullptr;   // core 0 -> core 1 (accepted exit events)
std::atomic<uint32_t> remoteLastRxMs{0};   // 0 = never heard
std::atomic<uint8_t>  remoteFlags{0};
std::atomic<uint32_t> remoteBootId{0};
std::atomic<int8_t>   linkOk{-1};
std::atomic<bool>     sendBusy{false};
std::atomic<uint32_t> sendStartMs{0};
std::atomic<uint32_t> statEvents{0}, statDuplicates{0}, statGaps{0}, statHeartbeats{0}, statQueueFull{0};
std::atomic<uint32_t> statEntries{0};
std::atomic<uint16_t> remoteWidthWanted{WIDTH_DEFAULT_CM};   // width the Remote must use (= params.widthRemoteCm)
std::atomic<uint32_t> cfgConfirmedBootId{0};                 // Remote session that acknowledged ...
std::atomic<uint16_t> cfgConfirmedWidth{0};                  // ... this width
std::atomic<uint32_t> calibSeenMs{0};                        // last /api/calib request (0 = never)
bool     remoteWasOnline  = false;
uint32_t remoteOfflineSinceMs = 0;   // when the Remote was declared offline (boot time at start)
bool     netReady = false;

// Barrier
BarrierState barrierState = BarrierState::Free;
uint8_t  belowCount   = 0;
uint32_t freeSinceMs  = 0;
uint32_t lastDataMs   = 0;
bool     noDataWarned = false;
int      thresholdMm  = 0;

// Audio
CardState cardState = CardState::Waiting;
FadeState fadeState = FadeState::None;
bool      cardRemoved = false;     // unit reported the card was taken out (BB 44 01 03)
bool      playing   = false;
bool      fadeOutFinished = false;
uint8_t   currentVolume   = 0;     // last volume value sent to the unit
bool      txLogEnabled    = true;

struct Frame { uint8_t bytes[8]; uint8_t len; };
constexpr uint8_t QUEUE_SIZE = 12;
Frame    queue[QUEUE_SIZE];
uint8_t  queueHead = 0, queueCount = 0;
uint32_t lastQueueSendMs = 0;

uint8_t  fadeFrom = 0, fadeTo = 0;
uint16_t fadeTicks = 0, fadeTick = 0;
uint32_t fadeIntervalMs = 0, fadeLastMs = 0;
uint32_t queryDeadlineMs = 0;

uint8_t  rxBuf[32];
uint8_t  rxLen = 0;
uint32_t rxLastMs = 0;

String   lineBuffer;

// Webapp / AP (the "web" task owns the AP and the server; the loop only requests and reads)
WebServer server(80);
std::atomic<bool>     apWanted{false};
std::atomic<bool>     apRunning{false};
std::atomic<uint8_t>  apClients{0};
std::atomic<uint32_t> lastHttpMs{0};
uint32_t ledFlashStartMs = 0;       // long-press confirmation flashes (loop only)

enum CmdType : uint8_t { CMD_SET_PARAMS, CMD_AUDIO_START, CMD_AUDIO_STOP };
struct Cmd { CmdType type; Params params; };
QueueHandle_t cmdQueue = nullptr;   // web task -> loop

// Types are declared before the first function so the Arduino-generated prototypes can use them.
struct LiveRange { int16_t last; int16_t minMm; int16_t maxMm; uint8_t occupied; uint32_t updatedMs; };
struct LedColor { uint8_t r, g, b; };
enum class LedPattern : uint8_t { Solid, Slow, Fast };   // Slow: 1 s period, Fast: 250 ms period
struct Snapshot {
    GameState gameState;
    CardState cardState;
    FadeState fadeState;
    bool      cardRemoved, playing, remoteWasOnline;
    uint8_t   currentVolume, errorMask;
    int32_t   windowLeftMs, maxWaitLeftMs, failsafeLeftMs;
    Params    params;
};

// ---------------------------------------------------------------- Event log (ring buffer in RAM)
struct LogEntry { uint32_t id; uint32_t ms; char text[LOG_TEXT_LEN]; };
LogEntry     logRing[LOG_CAPACITY];
uint32_t     logNextId = 1;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// Called from both cores. Prints to Serial and keeps the last LOG_CAPACITY entries for the webapp.
void logMsg(const char* fmt, ...) {
    char buf[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const uint32_t nowMs = millis();
    Serial.printf("[%8lu] %s\n", static_cast<unsigned long>(nowMs), buf);

    portENTER_CRITICAL(&logMux);
    LogEntry& e = logRing[(logNextId - 1) % LOG_CAPACITY];
    e.id = logNextId++;
    e.ms = nowMs;
    strncpy(e.text, buf, LOG_TEXT_LEN - 1);
    e.text[LOG_TEXT_LEN - 1] = '\0';
    portEXIT_CRITICAL(&logMux);
}

void printHex(const char* tag, const uint8_t* data, uint8_t len) {
    Serial.printf("%s", tag);
    for (uint8_t i = 0; i < len; i++) Serial.printf(" %02X", data[i]);
    Serial.println();
}

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_SW:        return "software reset";
        case ESP_RST_PANIC:     return "panic (exception)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake-up";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_USB:       return "USB";
        default:                return "unknown";
    }
}

// ---------------------------------------------------------------- Live ranges (calibration)
LiveRange    masterLive = {-1, -1, -1, 0, 0};
LiveRange    remoteLive = {-1, -1, -1, 0, 0};
portMUX_TYPE liveMux = portMUX_INITIALIZER_UNLOCKED;

// Merges a reading (or a window of readings) into the live range. Negative = no target.
void liveUpdate(LiveRange& l, int last, int minMm, int maxMm, bool occupied) {
    portENTER_CRITICAL(&liveMux);
    l.last = static_cast<int16_t>(last);
    if (minMm >= 0 && (l.minMm < 0 || minMm < l.minMm)) l.minMm = static_cast<int16_t>(minMm);
    if (maxMm >= 0 && (l.maxMm < 0 || maxMm > l.maxMm)) l.maxMm = static_cast<int16_t>(maxMm);
    l.occupied  = occupied ? 1 : 0;
    l.updatedMs = millis();
    portEXIT_CRITICAL(&liveMux);
}

// Copies the live range and restarts its min/max window.
LiveRange liveTake(LiveRange& l) {
    portENTER_CRITICAL(&liveMux);
    const LiveRange copy = l;
    l.minMm = -1;
    l.maxMm = -1;
    portEXIT_CRITICAL(&liveMux);
    return copy;
}

// ---------------------------------------------------------------- Parameters (NVS)
void paramsDefaults() {
    params = {DEF_VOLUME, DEF_FADE_OUT_MS, DEF_SILENCE_S, DEF_MAX_WAIT_S, WIDTH_DEFAULT_CM, WIDTH_DEFAULT_CM};
}

// Clamps every value to its limits. Returns true if something had to be corrected.
bool paramsSanitize() {
    const Params before = params;
    params.volume        = constrain(params.volume, MIN_VOLUME, MAX_VOLUME);
    params.fadeOutMs     = constrain(params.fadeOutMs, MIN_FADE_OUT_MS, MAX_FADE_OUT_MS);
    params.silenceS      = constrain(params.silenceS, MIN_SILENCE_S, MAX_SILENCE_S);
    params.maxWaitS      = constrain(params.maxWaitS, params.silenceS, MAX_MAX_WAIT_S);
    params.widthCm       = constrain(params.widthCm, WIDTH_MIN_CM, WIDTH_MAX_CM);
    params.widthRemoteCm = constrain(params.widthRemoteCm, WIDTH_MIN_CM, WIDTH_MAX_CM);
    return memcmp(&before, &params, sizeof(Params)) != 0;
}

// Checks a whole proposed set at once (the order the fields are edited in does not matter).
// Returns true if valid; otherwise fills `error` with every problem found.
bool paramsValidate(const Params& p, String& error) {
    error = "";
    auto bad = [&](const char* text) { if (error.length()) error += "; "; error += text; };
    if (p.volume < MIN_VOLUME || p.volume > MAX_VOLUME)                   bad("Volume fora dos limites");
    if (p.fadeOutMs < MIN_FADE_OUT_MS || p.fadeOutMs > MAX_FADE_OUT_MS)   bad("Fade-out fora dos limites");
    if (p.silenceS < MIN_SILENCE_S || p.silenceS > MAX_SILENCE_S)         bad("Janela de silêncio fora dos limites");
    if (p.maxWaitS > MAX_MAX_WAIT_S)                                      bad("Espera máxima fora dos limites");
    if (p.maxWaitS < p.silenceS)                                          bad("A espera máxima não pode ser menor que a janela de silêncio");
    if (p.widthCm < WIDTH_MIN_CM || p.widthCm > WIDTH_MAX_CM)             bad("Largura da entrada fora dos limites");
    if (p.widthRemoteCm < WIDTH_MIN_CM || p.widthRemoteCm > WIDTH_MAX_CM) bad("Largura da saída fora dos limites");
    return error.length() == 0;
}

// Writes only the values that changed, to spare flash cycles.
void paramsSave() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    if (prefs.getUChar("vol", 0xFF) != params.volume)        prefs.putUChar("vol", params.volume);
    if (prefs.getUShort("fo", 0) != params.fadeOutMs)        prefs.putUShort("fo", params.fadeOutMs);
    if (prefs.getUShort("sw", 0) != params.silenceS)         prefs.putUShort("sw", params.silenceS);
    if (prefs.getUShort("mw", 0) != params.maxWaitS)         prefs.putUShort("mw", params.maxWaitS);
    if (prefs.getUShort("cw", 0) != params.widthCm)          prefs.putUShort("cw", params.widthCm);
    if (prefs.getUShort("cwr", 0) != params.widthRemoteCm)   prefs.putUShort("cwr", params.widthRemoteCm);
    prefs.end();
}

void paramsLoad() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);   // read/write so a missing namespace is created silently
    params.volume        = prefs.getUChar("vol", DEF_VOLUME);
    params.fadeOutMs     = prefs.getUShort("fo", DEF_FADE_OUT_MS);
    params.silenceS      = prefs.getUShort("sw", DEF_SILENCE_S);
    params.maxWaitS      = prefs.getUShort("mw", DEF_MAX_WAIT_S);
    params.widthCm       = prefs.getUShort("cw", WIDTH_DEFAULT_CM);
    params.widthRemoteCm = prefs.getUShort("cwr", WIDTH_DEFAULT_CM);
    prefs.end();
    if (paramsSanitize()) {
        logMsg("WARN stored parameters out of limits, corrected");
        paramsSave();
    }
    remoteWidthWanted = params.widthRemoteCm;
}

int thresholdCmFor(uint16_t widthCm) {
    return constrain(static_cast<int>(widthCm) - THRESHOLD_MARGIN_CM, MIN_THRESHOLD_CM, MAX_THRESHOLD_CM);
}

int computeThresholdMm() { return thresholdCmFor(params.widthCm) * 10; }

// ---------------------------------------------------------------- LED
constexpr LedColor LED_OFF     = {0, 0, 0};
constexpr LedColor LED_GREEN   = {0, LED_LEVEL, 0};
constexpr LedColor LED_BLUE    = {0, 0, LED_LEVEL};
constexpr LedColor LED_CYAN    = {0, LED_LEVEL, LED_LEVEL};
constexpr LedColor LED_YELLOW  = {LED_LEVEL, LED_LEVEL, 0};
constexpr LedColor LED_RED     = {LED_LEVEL, 0, 0};
constexpr LedColor LED_ORANGE  = {LED_LEVEL, LED_LEVEL / 3, 0};
constexpr LedColor LED_MAGENTA = {LED_LEVEL, 0, LED_LEVEL};
constexpr LedColor LED_WHITE   = {LED_LEVEL, LED_LEVEL, LED_LEVEL};

void ledShow(const LedColor& c) {
    static LedColor last = {255, 255, 255};
    if (c.r == last.r && c.g == last.g && c.b == last.b) return;
    last = c;
    M5.Led.setAllColor(c.r, c.g, c.b);
    M5.Led.display();
}

// ---------------------------------------------------------------- Barrier (Phase 1)
bool i2cDevicePresent(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

// Fatal start-up error: print it and blink red fast (the LED table's "Master ToF has no data").
void failStop(const char* message) {
    uint32_t lastPrintMs = 0;
    while (true) {
        const uint32_t nowMs = millis();
        if (lastPrintMs == 0 || nowMs - lastPrintMs >= 2000) { Serial.println(message); lastPrintMs = nowMs; }
        ledShow((nowMs % 250) < 125 ? LED_RED : LED_OFF);
        delay(10);
    }
}

void initBarrier() {
    Wire.end();
    Wire.begin(PIN_SDA, PIN_SCL, I2C_FREQ_HZ);
    if (!i2cDevicePresent(TOF_I2C_ADDRESS)) {
        failStop("ERROR: ToF not found at 0x29. Check wiring and power.");
    }
    if (!Units.add(tof, Wire) || !Units.begin()) {
        failStop("ERROR: ToF unit failed to initialise.");
    }
    thresholdMm = computeThresholdMm();
    lastDataMs  = millis();
}

// Feeds one reading (mm, negative = invalid). Returns true on a new passage.
bool updateBarrier(int rangeMm, uint32_t nowMs) {
    const bool below = (rangeMm >= 0) && (rangeMm < thresholdMm);
    const bool clear = (rangeMm < 0) || (rangeMm > thresholdMm + HYSTERESIS_CM * 10);

    switch (barrierState) {
        case BarrierState::Free:
            if (below) {
                if (++belowCount >= OCCUPIED_CONFIRM) {
                    barrierState = BarrierState::Occupied;
                    freeSinceMs  = 0;
                    return true;
                }
            } else {
                belowCount = 0;
            }
            break;

        case BarrierState::Occupied:
            if (clear) {
                if (freeSinceMs == 0) freeSinceMs = nowMs;
                if (nowMs - freeSinceMs >= REARM_MS) {
                    barrierState = BarrierState::Free;
                    belowCount   = 0;
                    freeSinceMs  = 0;
                }
            } else {
                freeSinceMs = 0;
            }
            break;
    }
    return false;
}

// ---------------------------------------------------------------- Audio: low level (Phase 2)
void sendFrameNow(uint8_t cmd, const uint8_t* data, uint8_t dataLen) {
    uint8_t frame[8];
    frame[0] = cmd;
    frame[1] = static_cast<uint8_t>(~cmd);
    frame[2] = dataLen;
    uint8_t sum = frame[0] + frame[1] + frame[2];
    for (uint8_t i = 0; i < dataLen; i++) {
        frame[3 + i] = data[i];
        sum += data[i];
    }
    frame[3 + dataLen] = sum;
    Serial1.write(frame, 4 + dataLen);
    if (txLogEnabled) printHex("TX", frame, 4 + dataLen);
}

void enqueue(uint8_t cmd, const uint8_t* data, uint8_t dataLen) {
    if (queueCount >= QUEUE_SIZE) { logMsg("ERR audio queue full"); return; }
    Frame& f = queue[(queueHead + queueCount) % QUEUE_SIZE];
    f.bytes[0] = cmd;
    f.len = dataLen;
    for (uint8_t i = 0; i < dataLen; i++) f.bytes[1 + i] = data[i];
    queueCount++;
}

void qVolume(uint8_t vol) { const uint8_t d[] = {0x01, vol}; enqueue(CMD_VOLUME, d, 2); }
void qLoopSingle()        { const uint8_t d[] = {0x01, 0x01}; enqueue(CMD_LOOP_MODE, d, 2); }
void qPlayTrack1()        { const uint8_t d[] = {0x06, 0x00, 0x01}; enqueue(CMD_PLAY_CTRL, d, 3); }
void qStop()              { const uint8_t d[] = {0x14}; enqueue(CMD_PLAY_CTRL, d, 1); }
void qQueryDevice()       { const uint8_t d[] = {0x08}; enqueue(CMD_PLAY_CTRL, d, 1); }

void serviceQueue() {
    if (queueCount == 0 || millis() - lastQueueSendMs < CMD_GAP_MS) return;
    Frame& f = queue[queueHead];
    const uint8_t cmd = f.bytes[0];
    const bool isDeviceQuery = (cmd == CMD_PLAY_CTRL && f.len == 1 && f.bytes[1] == 0x08);
    sendFrameNow(cmd, &f.bytes[1], f.len);
    if (cmd == CMD_VOLUME && f.len == 2) currentVolume = f.bytes[2];
    if (isDeviceQuery && cardState == CardState::Checking) queryDeadlineMs = millis() + QUERY_TIMEOUT_MS;
    queueHead = (queueHead + 1) % QUEUE_SIZE;
    queueCount--;
    lastQueueSendMs = millis();
}

// ---------------------------------------------------------------- Audio: fade
// Ramp of |to - from| levels over durationMs with at most one volume command per
// minStepMs. Linear interpolation, a command is sent only when the level changes.
void startRamp(uint8_t from, uint8_t to, uint32_t durationMs, uint32_t minStepMs) {
    const uint16_t delta = (from > to) ? from - to : to - from;
    fadeFrom = from;
    fadeTo = to;
    const uint32_t maxTicks = durationMs / minStepMs;
    fadeTicks = max<uint32_t>(1, min<uint32_t>(delta ? delta : 1, maxTicks));
    fadeIntervalMs = durationMs / fadeTicks;
    fadeTick = 0;
    fadeLastMs = millis();
    logMsg("RAMP %u -> %u, %u steps every %lu ms (total %lu ms)", from, to, fadeTicks,
           static_cast<unsigned long>(fadeIntervalMs), static_cast<unsigned long>(durationMs));
}

void serviceFade() {
    if (fadeState == FadeState::None) return;
    if (millis() - fadeLastMs < fadeIntervalMs) return;
    fadeLastMs += fadeIntervalMs;
    fadeTick++;
    const int32_t level = static_cast<int32_t>(fadeFrom) +
                          (static_cast<int32_t>(fadeTo) - fadeFrom) * static_cast<int32_t>(fadeTick) / fadeTicks;
    if (level != currentVolume) {
        const uint8_t d[] = {0x01, static_cast<uint8_t>(level)};
        sendFrameNow(CMD_VOLUME, d, 2);
        currentVolume = static_cast<uint8_t>(level);
        lastQueueSendMs = millis();  // keep queued commands away from the ramp
    }
    if (fadeTick >= fadeTicks) {
        if (fadeState == FadeState::Out) {
            logMsg("FADE-OUT done: stop + restore volume");
            qStop();
            qVolume(params.volume);
            playing = false;
            fadeOutFinished = true;
        } else {
            logMsg("FADE-IN done");
        }
        fadeState = FadeState::None;
    }
}

// ---------------------------------------------------------------- Audio: actions used by the game logic
bool audioReady() { return cardState == CardState::Ok; }

// The volume was already set at boot and after every stop, so only the play command is sent.
void audioPlay() {
    fadeState = FadeState::None;
    qPlayTrack1();
    playing = true;
}

void audioStartFadeOut(uint32_t durationMs) {
    fadeOutFinished = false;
    fadeState = FadeState::Out;
    startRamp(currentVolume, 0, durationMs, FADE_OUT_STEP_MS);
}

// Cancels a fade-out in progress and goes back up to the configured volume.
void audioStartFadeIn() {
    fadeOutFinished = false;
    fadeState = FadeState::In;
    const uint16_t delta = (currentVolume > params.volume) ? currentVolume - params.volume
                                                           : params.volume - currentVolume;
    startRamp(currentVolume, params.volume, max<uint16_t>(1, delta) * FADE_IN_STEP_MS, FADE_IN_STEP_MS);
}

// ---------------------------------------------------------------- Audio: unit answers
void decodeFrame(const uint8_t* b, uint8_t n) {
    if (n >= 6 && b[0] == 0xAA && b[1] == 0x55) {
        logMsg("UNIT error code %u", b[4]);
        if (b[4] == 0x02 || b[4] == 0x03) { cardState = CardState::Error; logMsg("CARD ERROR: drive not found"); }
    } else if (n >= 5 && b[0] == 0xBB) {
        // BB 44 01 03 03 = card removed, BB 44 01 02 02 = card inserted (verified in Phase 3)
        if (b[3] == 0x03)      { cardRemoved = true;  logMsg("CARD removed"); }
        else if (b[3] == 0x02) { cardRemoved = false; logMsg("CARD inserted"); }
        else                   logMsg("UNIT card event %02X", b[3]);
    } else if (n >= 5 && b[0] == 0x04 && b[1] == 0xFB && b[2] == 0x02 && b[3] == 0x08) {
        const uint8_t d = b[4];
        const bool hasSd = (d == 2 || d == 3 || d == 6);
        logMsg("UNIT online devices code %u (SD %s)", d, hasSd ? "present" : "ABSENT");
        if (cardState == CardState::Checking) {
            cardState = hasSd ? CardState::Ok : CardState::Error;
            logMsg(hasSd ? "AUDIO READY" : "CARD ERROR: no SD card");
        }
    }
}

void serviceUnitRx() {
    while (Serial1.available() && rxLen < sizeof(rxBuf)) {
        rxBuf[rxLen++] = Serial1.read();
        rxLastMs = millis();
    }
    if (rxLen > 0 && millis() - rxLastMs >= RX_IDLE_MS) {
        printHex("RX", rxBuf, rxLen);
        decodeFrame(rxBuf, rxLen);
        rxLen = 0;
    }
    if (cardState == CardState::Checking && queryDeadlineMs && millis() > queryDeadlineMs) {
        cardState = CardState::Error;
        queryDeadlineMs = 0;
        logMsg("CARD ERROR: no answer from the unit (no SD card, wrong RX/TX pins or unit not powered)");
    }
}

void serviceBoot() {
    if (cardState != CardState::Waiting || millis() < BOOT_WAIT_MS) return;
    cardState = CardState::Checking;
    logMsg("Boot wait over: initialising the unit");
    qStop();   // the unit keeps playing across an ESP reset: always start silent (SPEC)
    qVolume(params.volume);
    qLoopSingle();
    qQueryDevice();
}

// ---------------------------------------------------------------- ESP-NOW (core 0)
void onNetRx(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    // Runs in the Wi-Fi task: only filter by sender and copy to the queue.
    if (memcmp(info->src_addr, REMOTE_MAC, 6) != 0) return;
    if (len <= 0 || len > static_cast<int>(sizeof(RxItem::data))) return;
    RxItem item;
    memcpy(item.data, data, len);
    item.len  = static_cast<uint8_t>(len);
    item.rxMs = millis();
    xQueueSend(rxQueue, &item, 0);
}

void onNetTx(NET_TX_CB_PARAMS) {
    linkOk = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0;
    sendBusy = false;
}

// Waits briefly for a previous send to complete, then sends. Runs on core 0 only.
bool netSendRaw(const void* data, size_t len) {
    const uint32_t startMs = millis();
    while (sendBusy && millis() - sendStartMs < SEND_BUSY_TIMEOUT_MS) {
        if (millis() - startMs > 30) return false;
        vTaskDelay(1);
    }
    sendBusy = true;
    sendStartMs = millis();
    if (esp_now_send(REMOTE_MAC, static_cast<const uint8_t*>(data), len) != ESP_OK) {
        sendBusy = false;
        return false;
    }
    return true;
}

// Deduplication state, owned by the communication task.
uint32_t sessionBootId = 0;
uint16_t lastSeq = 0;
bool     haveSeq = false;

void handleExitEvent(const RxItem& rx) {
    if (rx.len != sizeof(ExitEventMsg)) return;
    ExitEventMsg msg;
    memcpy(&msg, rx.data, sizeof(msg));

    if (sessionBootId != msg.header.bootId) {   // new Remote session: sequence numbers restart
        sessionBootId = msg.header.bootId;
        haveSeq = false;
    }
    const int16_t diff = static_cast<int16_t>(msg.seq - lastSeq);
    const bool isNew = !haveSeq || diff > 0;

    if (isNew) {
        const ExitEvent ev = {rx.rxMs - msg.ageMs, msg.seq, msg.ageMs};
        if (xQueueSend(exitQueue, &ev, 0) != pdTRUE) {
            statQueueFull++;
            logMsg("ERR exit queue full: seq %u not acknowledged, the Remote will resend", msg.seq);
            return;   // no ACK
        }
        if (haveSeq && diff > 1) {
            statGaps++;
            logMsg("WARN sequence gap: expected %u, got %u", static_cast<uint16_t>(lastSeq + 1), msg.seq);
        }
        lastSeq = msg.seq;
        haveSeq = true;
        statEvents++;
    } else {
        statDuplicates++;
        logMsg("duplicate seq %u ignored (already executed), acknowledging again", msg.seq);
    }

    EventAckMsg ack;
    ack.header = {PROTO_VERSION, MSG_EVENT_ACK, msg.header.bootId};
    ack.seq    = msg.seq;
    if (!netSendRaw(&ack, sizeof(ack))) logMsg("WARN could not send ACK for seq %u", msg.seq);
}

void handleHeartbeat(const RxItem& rx) {
    if (rx.len != sizeof(HeartbeatMsg)) return;
    HeartbeatMsg hb;
    memcpy(&hb, rx.data, sizeof(hb));
    if (hb.header.bootId != remoteBootId) {
        remoteBootId = hb.header.bootId;
        logMsg("Remote session started (bootId %08lX)", static_cast<unsigned long>(hb.header.bootId));
    }
    remoteFlags = hb.flags;
    statHeartbeats++;
}

void handleConfigAck(const RxItem& rx) {
    if (rx.len != sizeof(ConfigAckMsg)) return;
    ConfigAckMsg ack;
    memcpy(&ack, rx.data, sizeof(ack));
    cfgConfirmedWidth  = ack.widthCm;
    cfgConfirmedBootId = ack.header.bootId;
    if (ack.widthCm == remoteWidthWanted) logMsg("Remote confirmed width %u cm", ack.widthCm);
}

void handleRange(const RxItem& rx) {
    if (rx.len != sizeof(RangeMsg)) return;
    RangeMsg msg;
    memcpy(&msg, rx.data, sizeof(msg));
    liveUpdate(remoteLive, msg.lastMm, msg.minMm, msg.maxMm, msg.occupied != 0);
}

bool remoteOnline(uint32_t nowMs) {
    const uint32_t last = remoteLastRxMs;
    return last != 0 && nowMs - last < REMOTE_OFFLINE_MS;
}

// Pushes the Remote's corridor width until it confirms it (also after a Remote reboot, since
// the confirmation is tied to the Remote's bootId) and keeps the live-range lease alive while
// the calibration tab is open.
void serviceRemoteSettings(uint32_t nowMs) {
    static uint32_t nextConfigMs = 0, nextLeaseMs = 0;
    const uint32_t bootId = remoteBootId;
    if (bootId == 0 || !remoteOnline(nowMs)) return;

    const uint16_t wanted = remoteWidthWanted;
    if (nowMs >= nextConfigMs && !(cfgConfirmedBootId == bootId && cfgConfirmedWidth == wanted)) {
        ConfigMsg msg;
        msg.header  = {PROTO_VERSION, MSG_CONFIG, bootId};
        msg.widthCm = wanted;
        if (netSendRaw(&msg, sizeof(msg))) nextConfigMs = nowMs + CONFIG_RETRY_MS;
        return;
    }
    const uint32_t seen = calibSeenMs;
    if (nowMs >= nextLeaseMs && seen != 0 && nowMs - seen < CALIB_ACTIVE_MS) {
        MsgHeader msg = {PROTO_VERSION, MSG_STREAM_LEASE, bootId};
        if (netSendRaw(&msg, sizeof(msg))) nextLeaseMs = nowMs + LEASE_RENEW_MS;
    }
}

void commTask(void*) {
    for (;;) {
        RxItem rx;
        while (xQueueReceive(rxQueue, &rx, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (rx.len < sizeof(MsgHeader)) continue;
            MsgHeader header;
            memcpy(&header, rx.data, sizeof(header));
            if (header.version != PROTO_VERSION) continue;

            remoteLastRxMs = rx.rxMs;   // any valid packet proves the link
            switch (header.type) {
                case MSG_EXIT_EVENT: handleExitEvent(rx); break;
                case MSG_HEARTBEAT:  handleHeartbeat(rx); break;
                case MSG_CONFIG_ACK: handleConfigAck(rx); break;
                case MSG_RANGE:      handleRange(rx);     break;
                default: break;
            }
        }
        serviceRemoteSettings(millis());
    }
}

// Re-adds the Remote as a peer if a Wi-Fi mode change dropped it.
void ensurePeer() {
    if (!netReady || esp_now_is_peer_exist(REMOTE_MAC)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, REMOTE_MAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) logMsg("WARN ESP-NOW peer had to be re-added");
    else logMsg("ERR could not re-add the ESP-NOW peer");
}

// ESP-NOW and the AP share the radio: both must stay on ESPNOW_CHANNEL.
void enforceChannel() {
    uint8_t channel = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    if (channel != ESPNOW_CHANNEL) {
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        esp_wifi_get_channel(&channel, &second);
        logMsg("WARN Wi-Fi channel was changed, now %u (expected %u)", channel, ESPNOW_CHANNEL);
    }
}

// ---------------------------------------------------------------- Game logic
void setGameState(GameState next) {
    logMsg("STATE %s -> %s", GAME_STATE_NAMES[static_cast<uint8_t>(gameState)],
           GAME_STATE_NAMES[static_cast<uint8_t>(next)]);
    if (next == GameState::Playing) playingSinceMs = millis();
    gameState = next;
}

void onEntryPassage(uint32_t nowMs) {
    switch (gameState) {
        case GameState::Ready:
            if (!audioReady()) { logMsg("ERR entry refused: audio not ready"); return; }
            audioPlay();
            setGameState(GameState::Playing);
            break;
        case GameState::Playing:
            logMsg("entry ignored (already playing)");
            break;
        case GameState::Waiting:
            windowStartMs = nowMs;
            logMsg("entry: silence window restarted");
            break;
        case GameState::FadeOut:
            logMsg("entry: fade-out cancelled, fading in");
            audioStartFadeIn();
            setGameState(GameState::Playing);
            break;
        case GameState::Rearm:
            logMsg("entry during rearm: play queued behind stop + volume");
            audioPlay();
            setGameState(GameState::Playing);
            break;
    }
}

void onExitPassage(uint32_t nowMs) {
    switch (gameState) {
        case GameState::Playing:
            windowStartMs  = nowMs;
            maxWaitStartMs = nowMs;
            setGameState(GameState::Waiting);
            break;
        case GameState::Waiting:
            // A late event from the Remote carries its real (older) time: never move the window back.
            if (static_cast<int32_t>(nowMs - windowStartMs) > 0) windowStartMs = nowMs;
            logMsg("exit: silence window restarted");
            break;
        case GameState::Ready:
        case GameState::FadeOut:
        case GameState::Rearm:
            logMsg("exit ignored (%s)", GAME_STATE_NAMES[static_cast<uint8_t>(gameState)]);
            break;
    }
}

// Debugging "stop audio": immediate stop with the volume restored, then Rearm -> Ready.
void audioStopNow() {
    if (gameState == GameState::Ready || gameState == GameState::Rearm) {
        logMsg("WEB stop audio refused (%s)", GAME_STATE_NAMES[static_cast<uint8_t>(gameState)]);
        return;
    }
    logMsg("WEB stop audio: immediate stop");
    fadeState = FadeState::None;
    fadeOutFinished = false;
    qStop();
    qVolume(params.volume);
    playing = false;
    setGameState(GameState::Rearm);
}

void serviceTimers() {
    const uint32_t nowMs = millis();
    switch (gameState) {
        case GameState::Playing:
            // Failsafe: with the Remote offline no exit can arrive. Counted from the later of
            // "Remote went offline" and "entered Playing". Waiting needs no failsafe (its timers end it).
            // Uses remoteWasOnline (updated together with remoteOfflineSinceMs in serviceRemote),
            // not a fresh remoteOnline(): a newer clock reading could see "offline" one step early.
            if (!remoteWasOnline) {
                const uint32_t sinceMs = (static_cast<int32_t>(playingSinceMs - remoteOfflineSinceMs) > 0)
                                             ? playingSinceMs : remoteOfflineSinceMs;
                if (nowMs - sinceMs >= static_cast<uint32_t>(params.maxWaitS) * 1000UL) {
                    logMsg("Remote offline for the max wait: failsafe, starting fade-out");
                    audioStartFadeOut(params.fadeOutMs);
                    setGameState(GameState::FadeOut);
                }
            }
            break;
        case GameState::Waiting: {
            const bool windowOver  = nowMs - windowStartMs  >= static_cast<uint32_t>(params.silenceS) * 1000UL;
            const bool maxWaitOver = nowMs - maxWaitStartMs >= static_cast<uint32_t>(params.maxWaitS) * 1000UL;
            if (windowOver || maxWaitOver) {
                logMsg("%s elapsed: starting fade-out", windowOver ? "silence window" : "max wait");
                audioStartFadeOut(params.fadeOutMs);
                setGameState(GameState::FadeOut);
            }
            break;
        }
        case GameState::FadeOut:
            if (fadeOutFinished) setGameState(GameState::Rearm);
            break;
        case GameState::Rearm:
            if (queueCount == 0) setGameState(GameState::Ready);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- Inputs
void serviceBarrier() {
    const uint32_t nowMs = millis();
    if (tof.updated()) {
        lastDataMs   = nowMs;
        noDataWarned = false;
        const int rangeMm = static_cast<int>(tof.range());
        const bool passage = updateBarrier(rangeMm, nowMs);
        liveUpdate(masterLive, rangeMm, rangeMm, rangeMm, barrierState == BarrierState::Occupied);
        if (passage) {
            statEntries++;
            logMsg("ENTRY passage (range %d mm)", rangeMm);
            onEntryPassage(nowMs);
        }
    }
    if (!noDataWarned && nowMs - lastDataMs > NO_DATA_WARN_MS) {
        logMsg("WARNING: no data from the ToF.");
        noDataWarned = true;
    }
}

// Link state transitions and the exit events accepted by the communication task.
void serviceRemote() {
    const uint32_t nowMs = millis();
    const bool online = remoteOnline(nowMs);
    if (online != remoteWasOnline) {
        remoteWasOnline = online;
        if (online) {
            logMsg("REMOTE ONLINE%s", (remoteFlags & HB_FLAG_TOF_FAULT) ? " (ToF fault reported)" : "");
        } else {
            remoteOfflineSinceMs = nowMs;
            logMsg("REMOTE OFFLINE (no packet for %lu ms)", static_cast<unsigned long>(REMOTE_OFFLINE_MS));
        }
    }
    ExitEvent ev;
    while (xQueueReceive(exitQueue, &ev, 0) == pdTRUE) {
        logMsg("EXIT passage (Remote seq %u, age %u ms)", ev.seq, ev.ageMs);
        onExitPassage(ev.eventMs);
    }
}

// Long press toggles the AP. A short click does nothing.
void serviceButton() {
    if (M5.BtnA.wasHold()) {
        const bool next = !apWanted;
        apWanted = next;
        ledFlashStartMs = millis() | 1;
        logMsg("Long press: AP %s requested", next ? "ON" : "OFF");
    }
}

// ---------------------------------------------------------------- Errors, snapshot and commands
constexpr int32_t NO_TIME = INT32_MIN;
enum ErrorBit : uint8_t {
    ERR_CARD = 0x01,          // card missing, removed or the unit does not answer
    ERR_TOF_MASTER = 0x02,    // entry ToF without data
    WARN_REMOTE_OFFLINE = 0x04,
    WARN_TOF_REMOTE = 0x08,
    ERR_NET = 0x10            // ESP-NOW not running
};

Snapshot     snap = {};
portMUX_TYPE snapMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t     lastSnapshotMs = 0;

uint8_t computeErrorMask() {
    uint8_t mask = 0;
    if (cardState == CardState::Error || cardRemoved) mask |= ERR_CARD;
    if (noDataWarned) mask |= ERR_TOF_MASTER;
    if (!remoteWasOnline) mask |= WARN_REMOTE_OFFLINE;
    else if (remoteFlags & HB_FLAG_TOF_FAULT) mask |= WARN_TOF_REMOTE;
    if (!netReady) mask |= ERR_NET;
    return mask;
}

// Time left until the Remote-offline failsafe fires (Playing only), may be negative briefly.
int32_t failsafeLeftMs(uint32_t nowMs) {
    const uint32_t sinceMs = (static_cast<int32_t>(playingSinceMs - remoteOfflineSinceMs) > 0)
                                 ? playingSinceMs : remoteOfflineSinceMs;
    return static_cast<int32_t>(params.maxWaitS) * 1000 - static_cast<int32_t>(nowMs - sinceMs);
}

// Loop side: publishes what the web task may read (it never touches the game variables).
void publishSnapshot() {
    const uint32_t nowMs = millis();
    if (nowMs - lastSnapshotMs < SNAPSHOT_PERIOD_MS) return;
    lastSnapshotMs = nowMs;
    Snapshot s = {};
    s.gameState = gameState;
    s.cardState = cardState;
    s.fadeState = fadeState;
    s.cardRemoved = cardRemoved;
    s.playing = playing;
    s.remoteWasOnline = remoteWasOnline;
    s.currentVolume = currentVolume;
    s.errorMask = computeErrorMask();
    s.windowLeftMs = s.maxWaitLeftMs = s.failsafeLeftMs = NO_TIME;
    if (gameState == GameState::Waiting) {
        s.windowLeftMs  = static_cast<int32_t>(params.silenceS) * 1000 - static_cast<int32_t>(nowMs - windowStartMs);
        s.maxWaitLeftMs = static_cast<int32_t>(params.maxWaitS) * 1000 - static_cast<int32_t>(nowMs - maxWaitStartMs);
    }
    if (gameState == GameState::Playing && !remoteWasOnline) s.failsafeLeftMs = failsafeLeftMs(nowMs);
    s.params = params;
    portENTER_CRITICAL(&snapMux);
    snap = s;
    portEXIT_CRITICAL(&snapMux);
}

Snapshot readSnapshot() {
    portENTER_CRITICAL(&snapMux);
    const Snapshot s = snap;
    portEXIT_CRITICAL(&snapMux);
    return s;
}

// Applies a validated parameter set (loop only): stores it in NVS and puts it into effect.
void applyParams(const Params& next) {
    const Params old = params;
    params = next;
    thresholdMm = computeThresholdMm();
    remoteWidthWanted = params.widthRemoteCm;
    if (params.volume != old.volume && audioReady() && fadeState == FadeState::None) qVolume(params.volume);
    paramsSave();
    logMsg("PARAMS saved from the webapp: vol=%u fo=%u ms sw=%u s mw=%u s cw=%u cm cwr=%u cm", params.volume,
           params.fadeOutMs, params.silenceS, params.maxWaitS, params.widthCm, params.widthRemoteCm);
}

void serviceCommands() {
    Cmd cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
            case CMD_SET_PARAMS:
                applyParams(cmd.params);
                break;
            case CMD_AUDIO_START:
                if (gameState == GameState::Ready) {
                    logMsg("WEB start audio (same as an entry passage)");
                    onEntryPassage(millis());
                } else {
                    logMsg("WEB start audio refused (%s)", GAME_STATE_NAMES[static_cast<uint8_t>(gameState)]);
                }
                break;
            case CMD_AUDIO_STOP:
                audioStopNow();
                break;
        }
    }
}

// ---------------------------------------------------------------- LED
// Base colour by priority: errors, then the game state. White pulses on top while the AP is on.
//   Ready green | Playing blue | Waiting cyan | Fade-out yellow slow | Rearm yellow
//   card error red | entry ToF error red fast | Remote offline orange slow | Remote ToF error magenta slow
//   AP on: 1 white pulse per 3 s (no client) or 2 pulses (client connected)
//   long-press confirmation: 3 fast white flashes
void serviceLed() {
    static uint32_t lastTickMs = 0;
    const uint32_t nowMs = millis();
    if (nowMs - lastTickMs < 20) return;
    lastTickMs = nowMs;

    LedColor color = LED_GREEN;
    LedPattern pattern = LedPattern::Solid;
    const uint8_t errors = computeErrorMask();
    if (errors & ERR_CARD)                    { color = LED_RED; }
    else if (errors & ERR_TOF_MASTER)         { color = LED_RED; pattern = LedPattern::Fast; }
    else if (errors & WARN_REMOTE_OFFLINE)    { color = LED_ORANGE; pattern = LedPattern::Slow; }
    else if (errors & WARN_TOF_REMOTE)        { color = LED_MAGENTA; pattern = LedPattern::Slow; }
    else {
        switch (gameState) {
            case GameState::Ready:   color = LED_GREEN; break;
            case GameState::Playing: color = LED_BLUE; break;
            case GameState::Waiting: color = LED_CYAN; break;
            case GameState::FadeOut: color = LED_YELLOW; pattern = LedPattern::Slow; break;
            case GameState::Rearm:   color = LED_YELLOW; break;
        }
    }
    bool on = true;
    if (pattern == LedPattern::Slow) on = (nowMs % 1000) < 500;
    else if (pattern == LedPattern::Fast) on = (nowMs % 250) < 125;
    LedColor out = on ? color : LED_OFF;

    if (apRunning) {
        const uint32_t phase = nowMs % 3000;
        const bool pulse = (apClients > 0) ? (phase < 250 || (phase >= 500 && phase < 750)) : (phase < 1000);
        if (pulse) out = LED_WHITE;
    }
    if (ledFlashStartMs) {
        const uint32_t t = nowMs - ledFlashStartMs;
        if (t < 600) out = ((t % 200) < 100) ? LED_WHITE : LED_OFF;
        else ledFlashStartMs = 0;
    }
    ledShow(out);
}

// ---------------------------------------------------------------- Webapp: AP and HTTP (core 0)
void touchHttp() { lastHttpMs = millis(); }

void sendJson(int code, JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

void sendError(int code, const char* text) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = text;
    sendJson(code, doc);
}

void handlePage() {
    touchHttp();
    server.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
}

void addParams(JsonObject o, const Params& p) {
    o["volume"] = p.volume;
    o["fadeOutMs"] = p.fadeOutMs;
    o["silenceS"] = p.silenceS;
    o["maxWaitS"] = p.maxWaitS;
    o["widthCm"] = p.widthCm;
    o["widthRemoteCm"] = p.widthRemoteCm;
}

void addRange(JsonObject o, const char* key, uint16_t lo, uint16_t hi) {
    JsonArray a = o[key].to<JsonArray>();
    a.add(lo);
    a.add(hi);
}

void handleStatus() {
    touchHttp();
    const Snapshot s = readSnapshot();
    const uint32_t nowMs = millis();
    JsonDocument doc;
    doc["uptimeMs"] = nowMs;

    JsonObject game = doc["game"].to<JsonObject>();
    game["state"] = GAME_STATE_NAMES[static_cast<uint8_t>(s.gameState)];
    if (s.windowLeftMs != NO_TIME)   game["windowLeftMs"] = s.windowLeftMs;
    if (s.maxWaitLeftMs != NO_TIME)  game["maxWaitLeftMs"] = s.maxWaitLeftMs;
    if (s.failsafeLeftMs != NO_TIME) game["failsafeLeftMs"] = s.failsafeLeftMs;

    JsonObject audio = doc["audio"].to<JsonObject>();
    audio["card"] = s.cardRemoved ? "removido" : CARD_STATE_NAMES_PT[static_cast<uint8_t>(s.cardState)];
    audio["cardOk"] = (s.cardState == CardState::Ok);
    audio["playing"] = s.playing;
    audio["volume"] = s.currentVolume;

    const uint32_t lastRx = remoteLastRxMs;
    const uint32_t bootId = remoteBootId;
    const bool cfgConfirmed = bootId != 0 && cfgConfirmedBootId == bootId && cfgConfirmedWidth == s.params.widthRemoteCm;
    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lX", static_cast<unsigned long>(bootId));
    JsonObject remote = doc["remote"].to<JsonObject>();
    remote["online"] = s.remoteWasOnline;
    if (lastRx) remote["lastRxAgeMs"] = nowMs - lastRx;
    remote["bootId"] = bootId ? bootText : "-";
    remote["tofFault"] = (remoteFlags & HB_FLAG_TOF_FAULT) != 0;
    remote["events"] = statEvents.load();
    remote["duplicates"] = statDuplicates.load();
    remote["gaps"] = statGaps.load();
    remote["heartbeats"] = statHeartbeats.load();
    remote["queueFull"] = statQueueFull.load();
    remote["widthWanted"] = s.params.widthRemoteCm;
    remote["cfgConfirmed"] = cfgConfirmed;

    JsonArray errors = doc["errors"].to<JsonArray>();
    auto addError = [&](const char* code, const char* level) {
        JsonObject e = errors.add<JsonObject>();
        e["code"] = code;
        e["level"] = level;
    };
    if (s.errorMask & ERR_CARD)
        addError((s.cardState == CardState::Error) ? "card_missing" : "card_removed", "error");
    if (s.errorMask & ERR_TOF_MASTER)       addError("tof_master", "error");
    if (s.errorMask & ERR_NET)              addError("net_off", "error");
    if (s.errorMask & WARN_REMOTE_OFFLINE)  addError("remote_offline", "warn");
    if (s.errorMask & WARN_TOF_REMOTE)      addError("tof_remote", "warn");
    if (s.remoteWasOnline && !cfgConfirmed) addError("cfg_pending", "info");

    addParams(doc["params"].to<JsonObject>(), s.params);
    JsonObject limits = doc["limits"].to<JsonObject>();
    addRange(limits, "volume", MIN_VOLUME, MAX_VOLUME);
    addRange(limits, "fadeOutMs", MIN_FADE_OUT_MS, MAX_FADE_OUT_MS);
    addRange(limits, "silenceS", MIN_SILENCE_S, MAX_SILENCE_S);
    addRange(limits, "maxWaitS", s.params.silenceS, MAX_MAX_WAIT_S);
    addRange(limits, "widthCm", WIDTH_MIN_CM, WIDTH_MAX_CM);

    JsonObject ap = doc["ap"].to<JsonObject>();
    ap["clients"] = apClients.load();
    ap["idleLeftMs"] = static_cast<int32_t>(AP_IDLE_TIMEOUT_MS) - static_cast<int32_t>(nowMs - lastHttpMs);
    sendJson(200, doc);
}

void addBarrier(JsonObject o, const LiveRange& l, uint16_t widthCm, uint32_t count) {
    o["last"] = l.last;
    o["min"] = l.minMm;
    o["max"] = l.maxMm;
    o["occupied"] = l.occupied != 0;
    o["thresholdCm"] = thresholdCmFor(widthCm);
    o["count"] = count;
}

void handleCalib() {
    touchHttp();
    const uint32_t nowMs = millis();
    calibSeenMs = nowMs | 1;   // never 0
    const Snapshot s = readSnapshot();
    const LiveRange m = liveTake(masterLive);
    const LiveRange r = liveTake(remoteLive);
    JsonDocument doc;
    addBarrier(doc["master"].to<JsonObject>(), m, s.params.widthCm, statEntries);
    JsonObject remote = doc["remote"].to<JsonObject>();
    addBarrier(remote, r, s.params.widthRemoteCm, statEvents);
    if (r.updatedMs != 0 && nowMs - r.updatedMs < 5000) remote["ageMs"] = nowMs - r.updatedMs;
    sendJson(200, doc);
}

void handleLog() {
    touchHttp();
    const uint32_t since = server.hasArg("since") ? static_cast<uint32_t>(server.arg("since").toInt()) : 0;
    JsonDocument doc;
    JsonArray entries = doc["entries"].to<JsonArray>();
    uint32_t lastId = since;

    portENTER_CRITICAL(&logMux);
    const uint32_t newest = logNextId - 1;
    portEXIT_CRITICAL(&logMux);
    const uint32_t oldest = (newest > LOG_CAPACITY) ? newest - LOG_CAPACITY + 1 : 1;
    uint32_t first = max<uint32_t>(since + 1, oldest);
    if (since > newest) first = oldest;   // the device restarted: start over
    for (uint32_t id = first; id <= newest && entries.size() < LOG_PAGE_ENTRIES; id++) {
        LogEntry e;
        portENTER_CRITICAL(&logMux);
        e = logRing[(id - 1) % LOG_CAPACITY];
        portEXIT_CRITICAL(&logMux);
        if (e.id != id) continue;   // overwritten while we were reading
        JsonObject o = entries.add<JsonObject>();
        o["t"] = e.ms;
        o["m"] = e.text;
        lastId = id;
    }
    if (entries.size() == 0 && since > newest) lastId = newest;
    doc["last"] = lastId;
    sendJson(200, doc);
}

// Reads an integer field from the request body; returns false if missing or not a number.
bool readField(JsonDocument& doc, const char* key, long minValue, long maxValue, long& out) {
    if (!doc[key].is<long>()) return false;
    out = doc[key].as<long>();
    return out >= minValue && out <= maxValue;
}

void handleParams() {
    touchHttp();
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) { sendError(400, "Pedido inválido"); return; }
    long vol, fo, sw, mw, cw, cwr;
    if (!readField(doc, "volume", 0, 255, vol) || !readField(doc, "fadeOutMs", 0, 65535, fo) ||
        !readField(doc, "silenceS", 0, 65535, sw) || !readField(doc, "maxWaitS", 0, 65535, mw) ||
        !readField(doc, "widthCm", 0, 65535, cw) || !readField(doc, "widthRemoteCm", 0, 65535, cwr)) {
        sendError(400, "Valores em falta ou inválidos");
        return;
    }
    Cmd cmd;
    cmd.type = CMD_SET_PARAMS;
    cmd.params = {static_cast<uint8_t>(vol), static_cast<uint16_t>(fo), static_cast<uint16_t>(sw),
                  static_cast<uint16_t>(mw), static_cast<uint16_t>(cw), static_cast<uint16_t>(cwr)};
    String error;
    if (!paramsValidate(cmd.params, error)) { sendError(400, error.c_str()); return; }
    if (xQueueSend(cmdQueue, &cmd, 0) != pdTRUE) { sendError(503, "Ocupado, tente de novo"); return; }
    JsonDocument reply;
    reply["ok"] = true;
    sendJson(200, reply);
}

void handleAudio() {
    touchHttp();
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) { sendError(400, "Pedido inválido"); return; }
    const String action = doc["action"] | "";
    const Snapshot s = readSnapshot();
    Cmd cmd = {};
    if (action == "start") {
        if (s.gameState != GameState::Ready)    { sendError(409, "O áudio só pode ser iniciado em Pronto"); return; }
        if (s.cardState != CardState::Ok)       { sendError(409, "Áudio não está pronto (cartão)"); return; }
        cmd.type = CMD_AUDIO_START;
    } else if (action == "stop") {
        if (s.gameState == GameState::Ready || s.gameState == GameState::Rearm) {
            sendError(409, "Não há áudio a parar");
            return;
        }
        cmd.type = CMD_AUDIO_STOP;
    } else {
        sendError(400, "Ação desconhecida");
        return;
    }
    if (xQueueSend(cmdQueue, &cmd, 0) != pdTRUE) { sendError(503, "Ocupado, tente de novo"); return; }
    JsonDocument reply;
    reply["ok"] = true;
    sendJson(200, reply);
}

void handleNotFound() {
    touchHttp();
    server.send(404, "text/plain", "Not found");
}

void apStart() {
    WiFi.mode(WIFI_AP_STA);       // STA stays: ESP-NOW keeps using the station interface
    WiFi.setSleep(false);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD, ESPNOW_CHANNEL, 0, AP_MAX_CLIENTS)) {
        logMsg("ERR AP failed to start (check the password length: 8..63 characters)");
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        enforceChannel();
        ensurePeer();
        apWanted = false;
        return;
    }
    enforceChannel();
    ensurePeer();
    server.begin();
    lastHttpMs = millis();
    apRunning = true;
    logMsg("AP ON: SSID %s, IP %s, channel %u", AP_SSID, WiFi.softAPIP().toString().c_str(), ESPNOW_CHANNEL);
}

void apStop() {
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    enforceChannel();
    ensurePeer();
    apRunning = false;
    apClients = 0;
    logMsg("AP OFF");
}

void webTask(void*) {
    for (;;) {
        if (apWanted && !apRunning) apStart();
        else if (!apWanted && apRunning) apStop();

        if (apRunning) {
            server.handleClient();
            apClients = static_cast<uint8_t>(WiFi.softAPgetStationNum());
            if (millis() - lastHttpMs >= AP_IDLE_TIMEOUT_MS) {
                logMsg("AP idle for %lu min: switching off", static_cast<unsigned long>(AP_IDLE_TIMEOUT_MS / 60000UL));
                apWanted = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(apRunning ? 2 : 50));
    }
}

void setupWebServer() {
    server.on("/", HTTP_GET, handlePage);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/calib", HTTP_GET, handleCalib);
    server.on("/api/log", HTTP_GET, handleLog);
    server.on("/api/params", HTTP_POST, handleParams);
    server.on("/api/audio", HTTP_POST, handleAudio);
    server.onNotFound(handleNotFound);
    xTaskCreatePinnedToCore(webTask, "web", 10240, nullptr, 2, nullptr, 0);
}

// ---------------------------------------------------------------- Serial command
void printStatus() {
    const uint32_t nowMs = millis();
    Serial.printf("STATUS state=%s | vol=%u fo=%u ms sw=%u s mw=%u s cw=%u cm (threshold %d cm) cwr=%u cm\n",
                  GAME_STATE_NAMES[static_cast<uint8_t>(gameState)], params.volume, params.fadeOutMs,
                  params.silenceS, params.maxWaitS, params.widthCm, thresholdMm / 10, params.widthRemoteCm);
    Serial.printf("        audio card=%d removed=%d playing=%d fade=%d currentVol=%u queue=%u\n",
                  static_cast<int>(cardState), cardRemoved, playing, static_cast<int>(fadeState), currentVolume, queueCount);
    const uint32_t lastRx = remoteLastRxMs;
    Serial.printf("        remote %s lastRx=%s bootId=%08lX tofFault=%d widthConfirmed=%d | events=%lu duplicates=%lu gaps=%lu heartbeats=%lu queueFull=%lu\n",
                  remoteOnline(nowMs) ? "ONLINE" : "OFFLINE",
                  lastRx ? String(nowMs - lastRx).c_str() : "never", static_cast<unsigned long>(remoteBootId.load()),
                  (remoteFlags & HB_FLAG_TOF_FAULT) ? 1 : 0,
                  (remoteBootId != 0 && cfgConfirmedBootId == remoteBootId && cfgConfirmedWidth == params.widthRemoteCm) ? 1 : 0,
                  static_cast<unsigned long>(statEvents), static_cast<unsigned long>(statDuplicates),
                  static_cast<unsigned long>(statGaps), static_cast<unsigned long>(statHeartbeats),
                  static_cast<unsigned long>(statQueueFull));
    Serial.printf("        AP %s clients=%u\n", apRunning ? "ON" : "OFF", apClients.load());
    if (gameState == GameState::Playing && !remoteWasOnline) {
        Serial.printf("        failsafe fade-out in %ld ms\n", static_cast<long>(failsafeLeftMs(nowMs)));
    }
    if (gameState == GameState::Waiting) {
        const int32_t windowLeft  = static_cast<int32_t>(params.silenceS) * 1000 - static_cast<int32_t>(nowMs - windowStartMs);
        const int32_t maxWaitLeft = static_cast<int32_t>(params.maxWaitS) * 1000 - static_cast<int32_t>(nowMs - maxWaitStartMs);
        Serial.printf("        window left %ld ms, max wait left %ld ms\n", static_cast<long>(windowLeft),
                      static_cast<long>(maxWaitLeft));
    }
}

void handleLine(String line) {
    line.trim();
    if (line.isEmpty()) return;
    if (line == "?") printStatus();
    else logMsg("ERR unknown command");
}

void serviceSerialInput() {
    while (Serial.available()) {
        const char ch = Serial.read();
        if (ch == '\n' || ch == '\r') { handleLine(lineBuffer); lineBuffer = ""; }
        else if (lineBuffer.length() < 40) lineBuffer += ch;
    }
}

// ---------------------------------------------------------------- Arduino entry points
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.BtnA.setHoldThresh(LONG_PRESS_MS);
    M5.Led.setBrightness(255);   // the colour values already carry the (low) level
    ledShow(LED_OFF);
    Serial.begin(SERIAL_BAUD);
    Serial1.begin(AUDIO_BAUD, SERIAL_8N1, AUDIO_RX_PIN, AUDIO_TX_PIN);
    delay(500);
    Serial.println();
    Serial.println("Phase 5 - Master with webapp. Audio unit boots for 5 s...");
    logMsg("Reset reason: %s", resetReasonName(esp_reset_reason()));

    paramsLoad();
    initBarrier();

    rxQueue   = xQueueCreate(RX_QUEUE_SIZE, sizeof(RxItem));
    exitQueue = xQueueCreate(EXIT_QUEUE_SIZE, sizeof(ExitEvent));
    cmdQueue  = xQueueCreate(4, sizeof(Cmd));
    if (macIsZero(REMOTE_MAC)) {
        logMsg("ERR REMOTE_MAC not set in Protocol.h: ESP-NOW disabled");
    } else if (!netInit(REMOTE_MAC, onNetRx, onNetTx)) {
        logMsg("ERR ESP-NOW initialisation failed");
    } else {
        netReady = true;
        xTaskCreatePinnedToCore(commTask, "comm", 6144, nullptr, 3, nullptr, 0);
    }
    setupWebServer();
    uint8_t mac[6], channel = 0;
    wifi_second_chan_t second;
    WiFi.macAddress(mac);
    esp_wifi_get_channel(&channel, &second);
    logMsg("Master MAC %02X:%02X:%02X:%02X:%02X:%02X, ESP-NOW channel %u, link %s", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5], channel, netReady ? "ready" : "OFF");
    printStatus();
}

void loop() {
    M5.update();
    Units.update();
    serviceBarrier();
    serviceRemote();
    serviceButton();
    serviceCommands();
    serviceSerialInput();
    serviceBoot();
    serviceUnitRx();
    serviceFade();
    serviceQueue();
    serviceTimers();
    publishSnapshot();
    serviceLed();
}
