#ifndef __WIFI_ATKS_H__
#define __WIFI_ATKS_H__

#include <WiFi.h>
#include "scan_hosts.h"
#include <vector>

extern wifi_ap_record_t ap_record;

// Default target MAC (broadcast)[span_1](start_span)[span_1](end_span)
extern const uint8_t _default_target[6];

// Default Deauth Frame[span_2](start_span)[span_2](end_span)
const uint8_t deauth_frame_default[] = {0xc0, 0x00, 0x3a, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x02, 0x00};

extern uint8_t deauth_frame[]; //[span_3](start_span)[span_3](end_span)

extern uint8_t targetBssid[6]; //[span_4](start_span)[span_4](end_span)

/**
 * @brief Sends frame in frame_buffer using esp_wifi_80211_tx but bypasses blocking mechanism[span_5](start_span)[span_5](end_span)
 */
void send_raw_frame(const uint8_t *frame_buffer, int size); //[span_6](start_span)[span_6](end_span)

/**
 * @brief Prepare deauthentication frame with forged source AP from given ap_record[span_7](start_span)[span_7](end_span)
 */
void wsl_bypasser_send_raw_frame(
    const wifi_ap_record_t *ap_record, uint8_t chan, const uint8_t target[6] = _default_target
); //[span_8](start_span)[span_8](end_span)

void wifi_atk_info(const String &tssid, const String &mac, uint8_t channel); //[span_9](start_span)[span_9](end_span)

void wifi_atk_menu(); //[span_10](start_span)[span_10](end_span)

void target_atk_menu(const String &tssid, const String &mac, uint8_t channel); //[span_11](start_span)[span_11](end_span)

void target_atk(const String &tssid, const String &mac, uint8_t channel); //[span_12](start_span)[span_12](end_span)

void capture_handshake(const String &tssid, const String &mac, uint8_t channel); //[span_13](start_span)[span_13](end_span)

void beaconAttack(); //[span_14](start_span)[span_14](end_span)

void deauthFloodAttack(); //[span_15](start_span)[span_15](end_span)

// New enhanced deauth functions[span_16](start_span)[span_16](end_span)
void enhancedDeauthMenu(); //[span_17](start_span)[span_17](end_span)
void showTargetSelection(); //[span_18](start_span)[span_18](end_span)
std::vector<Host> buildTargetListFromScan(); //[span_19](start_span)[span_19](end_span)

#if !defined(LITE_VERSION)
void wifi_bruteforce_attack(const String &tssid, const String &mac, uint8_t channel);
#endif

#endif
