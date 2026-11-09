/*
 * Project : Escape room corridor audio device
 * File    : Protocol.h - ESP-NOW protocol shared by Master and Remote (Phase 5)
 *
 * KEEP THIS FILE IDENTICAL in Fase5_Mestre/ and Fase5_Remoto/ (compare with diff).
 *
 * Messages are unicast between the two paired nodes (fixed MAC addresses).
 * Exit passages use an application-level ACK: the Remote resends the same sequence
 * number until it is acknowledged, and the Master executes each (bootId, seq) once.
 * Doc: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_now.html
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

// ---------------------------------------------------------------- Pairing (fixed MAC addresses)
// Station-interface MAC of each board. Both boards print their own MAC at boot.
constexpr uint8_t MASTER_MAC[6] = {0xAC, 0xA7, 0x04, 0x00, 0x35, 0x4C};
constexpr uint8_t REMOTE_MAC[6] = {0xAC, 0xA7, 0x04, 0x00, 0x42, 0x00};

// Wi-Fi channel used by ESP-NOW on both nodes. The Master's AP (Phase 5) uses the same one.
constexpr uint8_t ESPNOW_CHANNEL = 1;

// ---------------------------------------------------------------- Timing
constexpr uint32_t HEARTBEAT_PERIOD_MS = 1000;   // Remote -> Master
constexpr uint32_t REMOTE_OFFLINE_MS   = 3000;   // Master: no packet for this long = Remote offline
constexpr uint32_t SEND_BUSY_TIMEOUT_MS = 50;    // give up waiting for a send callback

// Retry delay after the n-th transmission of the same event (n starts at 1).
inline uint32_t retryDelayMs(uint8_t attempt) {
    switch (attempt) {
        case 1:  return 40;
        case 2:  return 80;
        case 3:  return 160;
        case 4:  return 320;
        default: return 500;
    }
}

// ---------------------------------------------------------------- Messages
constexpr uint8_t PROTO_VERSION = 1;

enum MsgType : uint8_t {
    MSG_EXIT_EVENT   = 1,   // Remote -> Master
    MSG_EVENT_ACK    = 2,   // Master -> Remote
    MSG_HEARTBEAT    = 3,   // Remote -> Master
    MSG_CONFIG       = 4,   // Master -> Remote (Phase 5): corridor width, resent until acknowledged
    MSG_RANGE        = 5,   // Remote -> Master (Phase 5): live ToF reading for the calibration
    MSG_CONFIG_ACK   = 6,   // Remote -> Master (Phase 5): echoes the width now in use
    MSG_STREAM_LEASE = 7,   // Master -> Remote (Phase 5): "send MSG_RANGE for the next few seconds"
};

constexpr uint8_t HB_FLAG_TOF_FAULT = 0x01;   // Remote ToF has no data / not initialised

struct __attribute__((packed)) MsgHeader {
    uint8_t  version;
    uint8_t  type;
    uint32_t bootId;     // random value chosen by the Remote at every boot
};

struct __attribute__((packed)) ExitEventMsg {
    MsgHeader header;
    uint16_t  seq;       // 1, 2, 3 ... per bootId (wraps at 65535)
    uint16_t  ageMs;     // time since the detection, refreshed on every transmission
};

struct __attribute__((packed)) EventAckMsg {
    MsgHeader header;
    uint16_t  seq;
};

struct __attribute__((packed)) HeartbeatMsg {
    MsgHeader header;
    uint16_t  lastSeq;   // last sequence number assigned
    uint8_t   flags;     // HB_FLAG_*
};

// For the Phase 5 messages sent by the Master, header.bootId is the Remote's bootId (learnt from
// its heartbeat): a Remote that rebooted ignores messages meant for its previous session.
struct __attribute__((packed)) ConfigMsg {
    MsgHeader header;
    uint16_t  widthCm;   // corridor width at the Remote barrier
};

struct __attribute__((packed)) ConfigAckMsg {
    MsgHeader header;
    uint16_t  widthCm;   // width in use after applying the config
};

struct __attribute__((packed)) RangeMsg {
    MsgHeader header;
    int16_t   lastMm;    // last reading, negative = no target
    int16_t   minMm;     // lowest valid reading since the previous RangeMsg, negative = none
    int16_t   maxMm;     // highest valid reading since the previous RangeMsg, negative = none
    uint8_t   occupied;  // 1 = barrier currently occupied
};

// Remote: send a RangeMsg every RANGE_PERIOD_MS while a lease is valid; the Master renews it
// every LEASE_RENEW_MS and the Remote stops by itself LEASE_VALID_MS after the last one.
constexpr uint32_t RANGE_PERIOD_MS = 200;
constexpr uint32_t LEASE_RENEW_MS  = 1000;
constexpr uint32_t LEASE_VALID_MS  = 2500;
constexpr uint32_t CONFIG_RETRY_MS = 500;

// Corridor width limits (cm), shared so that both nodes validate the same way.
constexpr uint16_t WIDTH_MIN_CM = 40;
constexpr uint16_t WIDTH_MAX_CM = 120;
constexpr uint16_t WIDTH_DEFAULT_CM = 80;

// ---------------------------------------------------------------- Transport helpers
// The send callback signature changed in ESP-IDF 5.5.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
#define NET_TX_CB_PARAMS const esp_now_send_info_t* info, esp_now_send_status_t status
#else
#define NET_TX_CB_PARAMS const uint8_t* mac, esp_now_send_status_t status
#endif

inline bool macIsZero(const uint8_t* mac) {
    for (uint8_t i = 0; i < 6; i++) if (mac[i] != 0) return false;
    return true;
}

// Station mode (no network joined), fixed channel, power save off, one unicast peer.
inline bool netInit(const uint8_t* peerMac, esp_now_recv_cb_t rxCb, esp_now_send_cb_t txCb) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    if (esp_now_init() != ESP_OK) return false;
    if (esp_now_register_recv_cb(rxCb) != ESP_OK) return false;
    if (esp_now_register_send_cb(txCb) != ESP_OK) return false;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peerMac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}
