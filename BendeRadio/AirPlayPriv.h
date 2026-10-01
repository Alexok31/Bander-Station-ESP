#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <atomic>

#include "alac.h"

struct AirPlaySess {
    std::atomic<bool> ready{false};
    std::atomic<bool> advertised{false};
    std::atomic<bool> session{false};
    std::atomic<bool> playing{false};
    bool paused = false;  // guarded, PAUSE/FLUSH may outlive the RTP timeout
    bool encrypted = false;
    uint8_t aes_key[16]{};
    uint8_t aes_iv[16]{};
    int fmtp[12]{};
    uint16_t audio_port = 6001;
    uint16_t ctrl_port = 6002;
    uint16_t time_port = 6003;
    uint32_t last_rtp_ms = 0;  // guarded
    std::atomic<float> vol_gain{0.0f};
    bool enc_touched = false;
    std::atomic<bool> phone_vol_seen{false};
    alac_file* alac = nullptr;
};

extern AirPlaySess g_ap;
extern WiFiServer g_ap_rtsp;
extern WiFiClient g_ap_cli;
extern WiFiUDP g_ap_audio;
extern WiFiUDP g_ap_ctrl;
extern WiFiUDP g_ap_time;
extern std::atomic<bool> g_ap_owns;
extern uint8_t g_ap_mac[6];

bool airplay_crypto_begin();
size_t airplay_b64_decode(const char* s, uint8_t* out, size_t outmax);
bool airplay_rsa_oaep_decrypt(const uint8_t* in, size_t inlen, uint8_t* out, size_t* outlen);
bool airplay_apple_response(const uint8_t* challenge, size_t clen, char* b64, size_t b64max);
void airplay_aes_prepare();
void airplay_aes_decrypt(const uint8_t* in, size_t n, uint8_t* out);

void airplay_rtsp_poll();
void airplay_rtsp_hangup();
void airplay_rtsp_request_hangup();
bool airplay_rtsp_hangup_pending();
bool airplay_rtsp_alive();
void airplay_dacp_request(float db);
void airplay_dacp_poll();
void airplay_dacp_command(const char* cmd);
int airplay_db_to_ui(float db);
float airplay_ui_to_db(int vol);
float airplay_db_to_gain(float db);
bool airplay_rtp_start();
void airplay_rtp_stop();
void airplay_rtp_flush(bool hasBoundary = false, uint32_t timestamp = 0);
void airplay_session_clear();
void airplay_meta_clear();
void airplay_meta_on_flush();
void airplay_meta_on_play();
void airplay_meta_seek_rtp(uint32_t rtptime);
void airplay_meta_on_rtp(uint32_t rtptime);
void airplay_meta_set_npt_ms(uint32_t npt_ms);

bool airplay_take_speaker();
void airplay_release_speaker();
void airplay_hw_mute(bool mute);
void airplay_write_pcm(int16_t* stereo, int frames);
