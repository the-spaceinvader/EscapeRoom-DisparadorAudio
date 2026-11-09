/*
 * Project : Escape room corridor audio device
 * Phase   : 5 - Remote node (exit barrier + ESP-NOW link to the Master, width set by the Master)
 * Target  : M5Stack AtomS3 Lite (board "M5AtomS3") + Unit Mini ToF-90 (U196) on the Grove port
 * Author  : <author>
 * Date    : <date>
 *
 * Core 1 (loop):  ToF barrier (Phase 1 logic), Serial status command.
 * Core 0 (task):  ESP-NOW. Sends each exit passage with a sequence number and resends
 *                 it until the Master acknowledges it; sends a heartbeat every second;
 *                 applies the corridor width sent by the Master (MSG_CONFIG) and
 *                 streams live ToF readings while the Master holds a lease (calibration).
 * Core 1 -> core 0 through a FreeRTOS queue. Wire format: see Protocol.h.
 *
 * The corridor width lives in the Master's NVS and is pushed here; this node keeps a copy in
 * its own NVS so that it starts with the right threshold before the Master's config arrives.
 *
 * Serial command over USB (115200 baud, line terminated):
 *   ?          print node state, link, width and counters
 *
 * Libraries: M5Unified, M5GFX, M5Unit-TOF (+ M5UnitUnified, M5Utility, M5HAL).
 * Serial Monitor: set Tools > USB CDC On Boot = Enabled, baud 115200.
 */

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedTOF.h>
#include <Preferences.h>
#include <atomic>
#include <stdarg.h>
#include "Protocol.h"

// ---------------------------------------------------------------- Pins and barrier parameters
constexpr int      PIN_SDA = 2;    // Grove port, yellow
constexpr int      PIN_SCL = 1;    // Grove port, white
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t I2C_FREQ_HZ = 400000;
constexpr uint8_t  TOF_I2C_ADDRESS = 0x29;

constexpr int      THRESHOLD_MARGIN_CM = 30;
constexpr int      MIN_THRESHOLD_CM    = 5;
constexpr int      MAX_THRESHOLD_CM    = 120;
constexpr int      HYSTERESIS_CM       = 10;
constexpr uint8_t  OCCUPIED_CONFIRM    = 2;
constexpr uint32_t REARM_MS            = 500;
constexpr uint32_t NO_DATA_WARN_MS     = 1000;

constexpr char     NVS_NAMESPACE[] = "escape";
constexpr char     NVS_KEY_WIDTH[] = "cw";

// ---------------------------------------------------------------- Communication parameters
constexpr uint8_t  EVENT_QUEUE_SIZE = 16;
constexpr uint8_t  RX_QUEUE_SIZE    = 8;

// ---------------------------------------------------------------- Types and globals
struct ExitItem { uint32_t detectMs; };
struct RxItem   { uint8_t data[16]; uint8_t len; };
struct InFlight {
    bool     active;
    uint16_t seq;
    uint32_t detectMs;
    uint32_t nextSendMs;
    uint8_t  attempts;
};

enum class BarrierState : uint8_t { Free, Occupied };

m5::unit::UnitUnified Units;
m5::unit::UnitToF90 tof;

// Barrier (core 1 only, except thresholdMm which the communication task may change)
BarrierState barrierState = BarrierState::Free;
uint8_t  belowCount   = 0;
uint32_t freeSinceMs  = 0;
uint32_t lastDataMs   = 0;
bool     noDataWarned = false;
uint32_t passageCount = 0;
std::atomic<int>      thresholdMm{(WIDTH_DEFAULT_CM - THRESHOLD_MARGIN_CM) * 10};
std::atomic<uint16_t> widthCm{WIDTH_DEFAULT_CM};

// Live reading window, written by core 1 and drained by the communication task
struct LiveWindow { int16_t lastMm; int16_t minMm; int16_t maxMm; uint8_t occupied; };
LiveWindow   liveWindow = {-1, -1, -1, 0};
portMUX_TYPE liveMux = portMUX_INITIALIZER_UNLOCKED;
int          lastRangeMm = -1;

// Shared between the two cores
QueueHandle_t eventQueue = nullptr;   // core 1 -> core 0
QueueHandle_t rxQueue    = nullptr;   // Wi-Fi callback -> core 0
std::atomic<bool>     tofFault{true};
std::atomic<uint32_t> statDropped{0};
std::atomic<uint32_t> statAcked{0};
std::atomic<uint32_t> statRetries{0};
std::atomic<uint16_t> lastSeq{0};
std::atomic<bool>     inFlightActive{false};
std::atomic<int8_t>   linkOk{-1};     // -1 unknown, 0 last send failed, 1 last send acknowledged by the MAC layer
std::atomic<bool>     sendBusy{false};
std::atomic<uint32_t> sendStartMs{0};
std::atomic<uint32_t> leaseUntilMs{0};      // live readings are streamed while millis() < this
std::atomic<uint32_t> statConfigs{0};

uint32_t bootId = 0;
String   lineBuffer;

// ---------------------------------------------------------------- Logging
void logMsg(const char* fmt, ...) {
    char buf[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.printf("[%8lu] %s\n", static_cast<unsigned long>(millis()), buf);
}

// ---------------------------------------------------------------- Width (NVS and threshold)
int computeThresholdMm(uint16_t width) {
    int thresholdCm = static_cast<int>(width) - THRESHOLD_MARGIN_CM;
    thresholdCm = constrain(thresholdCm, MIN_THRESHOLD_CM, MAX_THRESHOLD_CM);
    return thresholdCm * 10;
}

void loadWidth() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    uint16_t value = prefs.getUShort(NVS_KEY_WIDTH, WIDTH_DEFAULT_CM);
    prefs.end();
    if (value < WIDTH_MIN_CM || value > WIDTH_MAX_CM) value = WIDTH_DEFAULT_CM;
    widthCm = value;
    thresholdMm = computeThresholdMm(value);
}

// Applies a width received from the Master; stores it only when it changed (flash wear).
bool applyWidth(uint16_t value) {
    if (value < WIDTH_MIN_CM || value > WIDTH_MAX_CM) return false;
    if (value != widthCm) {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putUShort(NVS_KEY_WIDTH, value);
        prefs.end();
        widthCm = value;
        thresholdMm = computeThresholdMm(value);
        logMsg("CONFIG width %u cm (threshold %d cm)", value, thresholdMm.load() / 10);
    }
    return true;
}

// ---------------------------------------------------------------- ESP-NOW (callbacks and sending)
void onNetRx(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    // Runs in the Wi-Fi task: only filter by sender and copy to the queue.
    if (memcmp(info->src_addr, MASTER_MAC, 6) != 0) return;
    if (len <= 0 || len > static_cast<int>(sizeof(RxItem::data))) return;
    RxItem item;
    memcpy(item.data, data, len);
    item.len = static_cast<uint8_t>(len);
    xQueueSend(rxQueue, &item, 0);
}

void onNetTx(NET_TX_CB_PARAMS) {
    linkOk = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0;
    sendBusy = false;
}

// Sends one frame unless the previous send has not completed yet.
bool netSend(const void* data, size_t len, uint32_t nowMs) {
    if (sendBusy && nowMs - sendStartMs < SEND_BUSY_TIMEOUT_MS) return false;
    sendBusy = true;
    sendStartMs = nowMs;
    if (esp_now_send(MASTER_MAC, static_cast<const uint8_t*>(data), len) != ESP_OK) {
        sendBusy = false;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Communication task (core 0)
// Set when a CONFIG arrived and its acknowledgement still has to go out.
bool pendingConfigAck = false;

void handleRx(const RxItem& rx, InFlight& inFlight, uint32_t nowMs) {
    if (rx.len < sizeof(MsgHeader)) return;
    MsgHeader header;
    memcpy(&header, rx.data, sizeof(header));
    if (header.version != PROTO_VERSION || header.bootId != bootId) return;

    if (header.type == MSG_EVENT_ACK && rx.len == sizeof(EventAckMsg)) {
        EventAckMsg ack;
        memcpy(&ack, rx.data, sizeof(ack));
        if (inFlight.active && ack.seq == inFlight.seq) {
            logMsg("ACK seq %u after %u transmission(s), %lu ms since detection", inFlight.seq, inFlight.attempts,
                   static_cast<unsigned long>(nowMs - inFlight.detectMs));
            inFlight.active = false;
            inFlightActive = false;
            statAcked++;
        }
    } else if (header.type == MSG_CONFIG && rx.len == sizeof(ConfigMsg)) {
        ConfigMsg cfg;
        memcpy(&cfg, rx.data, sizeof(cfg));
        if (applyWidth(cfg.widthCm)) {
            statConfigs++;
            pendingConfigAck = true;   // always answer, so a lost ACK does not leave the Master retrying
        } else {
            logMsg("ERR CONFIG width %u cm out of limits, ignored", cfg.widthCm);
        }
    } else if (header.type == MSG_STREAM_LEASE) {
        leaseUntilMs = nowMs + LEASE_VALID_MS;
    }
}

void commTask(void*) {
    InFlight inFlight = {};
    uint32_t nextHeartbeatMs = 0;
    uint32_t nextRangeMs = 0;
    int8_t   reportedLink = -1;

    for (;;) {
        const uint32_t nowMs = millis();

        RxItem rx;
        while (xQueueReceive(rxQueue, &rx, 0) == pdTRUE) handleRx(rx, inFlight, nowMs);

        // Take the next event only when the previous one has been acknowledged (stop-and-wait).
        if (!inFlight.active) {
            ExitItem item;
            if (xQueueReceive(eventQueue, &item, 0) == pdTRUE) {
                inFlight = {true, static_cast<uint16_t>(lastSeq + 1), item.detectMs, nowMs, 0};
                lastSeq = inFlight.seq;
                inFlightActive = true;
            }
        }

        // Priority: exit events, then the config acknowledgement, heartbeat and live range.
        if (inFlight.active && nowMs >= inFlight.nextSendMs) {
            ExitEventMsg msg;
            msg.header = {PROTO_VERSION, MSG_EXIT_EVENT, bootId};
            msg.seq    = inFlight.seq;
            msg.ageMs  = static_cast<uint16_t>(min<uint32_t>(nowMs - inFlight.detectMs, 0xFFFF));
            if (netSend(&msg, sizeof(msg), nowMs)) {
                inFlight.attempts++;
                inFlight.nextSendMs = nowMs + retryDelayMs(inFlight.attempts);
                if (inFlight.attempts > 1) {
                    statRetries++;
                    logMsg("RETRY seq %u (transmission %u, age %u ms)", msg.seq, inFlight.attempts, msg.ageMs);
                } else {
                    logMsg("SEND seq %u (age %u ms)", msg.seq, msg.ageMs);
                }
            }
        } else if (pendingConfigAck) {
            ConfigAckMsg ack;
            ack.header  = {PROTO_VERSION, MSG_CONFIG_ACK, bootId};
            ack.widthCm = widthCm;
            if (netSend(&ack, sizeof(ack), nowMs)) pendingConfigAck = false;
        } else if (nowMs >= nextHeartbeatMs) {
            HeartbeatMsg hb;
            hb.header  = {PROTO_VERSION, MSG_HEARTBEAT, bootId};
            hb.lastSeq = lastSeq;
            hb.flags   = tofFault ? HB_FLAG_TOF_FAULT : 0;
            if (netSend(&hb, sizeof(hb), nowMs)) nextHeartbeatMs = nowMs + HEARTBEAT_PERIOD_MS;
        } else if (nowMs >= nextRangeMs && static_cast<int32_t>(leaseUntilMs.load() - nowMs) > 0) {
            RangeMsg msg;
            portENTER_CRITICAL(&liveMux);
            msg.lastMm   = liveWindow.lastMm;
            msg.minMm    = liveWindow.minMm;
            msg.maxMm    = liveWindow.maxMm;
            msg.occupied = liveWindow.occupied;
            portEXIT_CRITICAL(&liveMux);
            msg.header = {PROTO_VERSION, MSG_RANGE, bootId};
            if (netSend(&msg, sizeof(msg), nowMs)) {
                nextRangeMs = nowMs + RANGE_PERIOD_MS;
                portENTER_CRITICAL(&liveMux);      // restart the min/max window
                liveWindow.minMm = -1;
                liveWindow.maxMm = -1;
                portEXIT_CRITICAL(&liveMux);
            }
        }

        const int8_t link = linkOk;
        if (link != reportedLink && link >= 0) {
            logMsg("LINK to Master: %s", link ? "OK" : "FAILED (no MAC-level ACK)");
            reportedLink = link;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Called from the barrier logic (core 1). If the queue is full the oldest event is dropped.
void queueExitPassage(uint32_t detectMs) {
    const ExitItem item = {detectMs};
    if (xQueueSend(eventQueue, &item, 0) != pdTRUE) {
        ExitItem oldest;
        xQueueReceive(eventQueue, &oldest, 0);
        xQueueSend(eventQueue, &item, 0);
        statDropped++;
        logMsg("WARN event queue full: oldest event dropped");
    }
}

// ---------------------------------------------------------------- Barrier (Phase 1)
bool i2cDevicePresent(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

void failStop(const char* message) {
    while (true) {            // the communication task keeps running (heartbeat with ToF fault flag)
        Serial.println(message);
        delay(2000);
    }
}

void initBarrier() {
    Wire.end();
    Wire.begin(PIN_SDA, PIN_SCL, I2C_FREQ_HZ);
    if (!i2cDevicePresent(TOF_I2C_ADDRESS)) {
        // Help diagnosing swapped SDA/SCL wires: try the opposite order once.
        Wire.end();
        Wire.begin(PIN_SCL, PIN_SDA, I2C_FREQ_HZ);
        if (i2cDevicePresent(TOF_I2C_ADDRESS)) {
            Serial.println("0x29 answers only with SDA/SCL SWAPPED. Swap PIN_SDA/PIN_SCL.");
        }
        failStop("ERROR: ToF not found at 0x29. Check wiring and power.");
    }
    if (!Units.add(tof, Wire) || !Units.begin()) {
        failStop("ERROR: ToF unit failed to initialise.");
    }
    lastDataMs  = millis();
    tofFault    = false;
    Serial.println("ToF found at 0x29.");
}

// Feeds one reading (mm, negative = invalid). Returns true on a new passage.
bool updateBarrier(int rangeMm, uint32_t nowMs) {
    const int threshold = thresholdMm;
    const bool below = (rangeMm >= 0) && (rangeMm < threshold);
    const bool clear = (rangeMm < 0) || (rangeMm > threshold + HYSTERESIS_CM * 10);

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

// Records a reading for the live stream (last, min and max of the current window).
void recordLive(int rangeMm) {
    portENTER_CRITICAL(&liveMux);
    liveWindow.lastMm = static_cast<int16_t>(rangeMm);
    if (rangeMm >= 0) {
        if (liveWindow.minMm < 0 || rangeMm < liveWindow.minMm) liveWindow.minMm = static_cast<int16_t>(rangeMm);
        if (liveWindow.maxMm < 0 || rangeMm > liveWindow.maxMm) liveWindow.maxMm = static_cast<int16_t>(rangeMm);
    }
    liveWindow.occupied = (barrierState == BarrierState::Occupied) ? 1 : 0;
    portEXIT_CRITICAL(&liveMux);
}

void serviceBarrier() {
    const uint32_t nowMs = millis();
    if (tof.updated()) {
        lastDataMs   = nowMs;
        noDataWarned = false;
        tofFault     = false;
        lastRangeMm  = static_cast<int>(tof.range());
        const bool passage = updateBarrier(lastRangeMm, nowMs);
        recordLive(lastRangeMm);
        if (passage) {
            passageCount++;
            logMsg("EXIT passage #%lu (range %d mm)", static_cast<unsigned long>(passageCount), lastRangeMm);
            queueExitPassage(nowMs);
        }
    }
    if (!noDataWarned && nowMs - lastDataMs > NO_DATA_WARN_MS) {
        logMsg("WARNING: no data from the ToF.");
        noDataWarned = true;
        tofFault = true;
    }
}

// ---------------------------------------------------------------- Serial command
void printStatus() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    uint8_t channel = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    Serial.printf("STATUS Remote bootId=%08lX MAC=%02X:%02X:%02X:%02X:%02X:%02X channel=%u\n",
                  static_cast<unsigned long>(bootId), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], channel);
    Serial.printf("        lastSeq=%u inFlight=%d waiting=%u acked=%lu retries=%lu dropped=%lu link=%d tofFault=%d\n",
                  static_cast<unsigned>(lastSeq), inFlightActive.load(),
                  static_cast<unsigned>(uxQueueMessagesWaiting(eventQueue)),
                  static_cast<unsigned long>(statAcked), static_cast<unsigned long>(statRetries),
                  static_cast<unsigned long>(statDropped), static_cast<int>(linkOk), tofFault.load());
    Serial.printf("        barrier=%s range=%d mm width=%u cm threshold=%d cm passages=%lu configs=%lu\n",
                  barrierState == BarrierState::Occupied ? "OCCUPIED" : "FREE", lastRangeMm,
                  static_cast<unsigned>(widthCm), thresholdMm.load() / 10, static_cast<unsigned long>(passageCount),
                  static_cast<unsigned long>(statConfigs));
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
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println();
    Serial.println("Phase 5 - Remote");

    bootId = esp_random();
    if (bootId == 0) bootId = 1;

    loadWidth();
    eventQueue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(ExitItem));
    rxQueue    = xQueueCreate(RX_QUEUE_SIZE, sizeof(RxItem));

    if (!netInit(MASTER_MAC, onNetRx, onNetTx)) {
        failStop("ERROR: ESP-NOW initialisation failed.");
    }
    xTaskCreatePinnedToCore(commTask, "comm", 6144, nullptr, 3, nullptr, 0);

    printStatus();      // shows the MAC even if the ToF is missing
    initBarrier();      // may halt this core; the communication task keeps sending heartbeats
    printStatus();
}

void loop() {
    M5.update();
    Units.update();
    serviceBarrier();
    serviceSerialInput();
}
