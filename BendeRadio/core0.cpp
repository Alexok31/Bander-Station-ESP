#include "core0.h"

#include <WiFi.h>

#include <EEManager.h>
#include <EncButton.h>
#include <FastLED.h>
#include <GyverMAX7219.h>

#include "pong.h"
#include "soc/timer_group_reg.h"
#include "soc/timer_group_struct.h"
#include "tmr.h"

const char* stations[] = {
    "https://uk3.internet-radio.com/proxy/majesticjukebox?mp=/live",
    "http://prmstrm.1.fm:8000/electronica",
    "http://prmstrm.1.fm:8000/x",
    "http://stream81.metacast.eu/radio1rock128",
};

// data
MAX7219<5, 1, RadioConfig::mtrxCs, RadioConfig::mtrxDat, RadioConfig::mtrxClk> mtrx;
Tmr square_tmr;
Data data;
EEManager memory(data);
Audio audio;
String streamname;
const char* reconnect = nullptr;
volatile bool wifiConnecting = false;

// func
// ========================= MATRIX =========================
void upd_bright() {
    uint8_t m = data.bright_mouth, e = data.bright_eyes;
    uint8_t br[] = {m, m, m, e, e};
    mtrx.setBright(br);
}

// Pong: на табло ярче крайние «ротовые» модули; счёт на глазах — подсвечиваем их (bright_eyes).
static void pong_sync_matrix_brightness() {
    if (pong_mouth_tablo_mode()) {
        uint8_t m = (uint8_t)constrain((int)data.bright_mouth, 0, 15);
        uint8_t side = (uint8_t)constrain((int)m + 5, 0, 15);
        uint8_t mid = (uint8_t)constrain((int)m - 2, 0, 15);
        const uint8_t pulse = (uint8_t)(((millis() / 90) & 1) ? 1u : 0u);
        side = (uint8_t)constrain((int)side + (int)pulse, 0, 15);
        uint8_t e = (uint8_t)constrain((int)data.bright_eyes, 0, 15);
        uint8_t br[] = {side, mid, side, e, e};
        mtrx.setBright(br);
    } else {
        upd_bright();
    }
}
void print_val(char c, uint8_t v) {
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    mtrx.setCursor(8 * 0 + 2, 1);
    mtrx.print(c);
    mtrx.setCursor(8 * 1 + 2, 1);
    mtrx.print(v / 10);
    mtrx.setCursor(8 * 2 + 2, 1);
    mtrx.print(v % 10);
    mtrx.update();
}
// ========================= EYES =========================
void draw_eye(uint8_t i) {
    uint8_t x = RadioConfig::analyzWidth + i * 8;
    mtrx.rect(1 + x, 1, 6 + x, 6, GFX_FILL);
    mtrx.lineV(0 + x, 2, 5);
    mtrx.lineV(7 + x, 2, 5);
    mtrx.lineH(0, 2 + x, 5 + x);
    mtrx.lineH(7, 2 + x, 5 + x);
}
void draw_eyeb(uint8_t i, int x, int y, int w = 2) {
    x += RadioConfig::analyzWidth + i * 8;
    mtrx.rect(x, y, x + w - 1, y + w - 1, GFX_CLEAR);
}

// Pong: на табло счёт на глазах (левый — игрок, правый — ИИ). В розыгрыше — зрачок следует за мячом.
static void draw_eyes_follow_ball(int8_t ball_x, int8_t ball_y) {
    if (pong_mouth_tablo_mode()) {
        const uint8_t bx = RadioConfig::analyzWidth;
        mtrx.rect(bx, 0, bx + 16 - 1, 7, GFX_CLEAR);
        mtrx.setCursor(bx + 2, 1);
        mtrx.print((char)('0' + (pong_score_player() % 10)));
        mtrx.setCursor(bx + 8 + 2, 1);
        mtrx.print((char)('0' + (pong_score_ai() % 10)));
        return;
    }

    if (!data.state) {
        draw_eye(0);
        draw_eye(1);
        mtrx.rect(RadioConfig::analyzWidth, 0, RadioConfig::analyzWidth + 16 - 1, 3, GFX_CLEAR);
        draw_eyeb(0, 3, 5);
        draw_eyeb(1, 3, 5);
        return;
    }
    const int8_t pw = RadioConfig::analyzWidth;

    for (uint8_t i = 0; i < 2; i++) {
        draw_eye(i);
        const uint8_t base = RadioConfig::analyzWidth + i * 8;
        const uint8_t pup_x = (uint8_t)map(ball_x, 0, pw - 1, 2, 4);
        const uint8_t pup_y = (uint8_t)map(ball_y, 0, 7, 2, 4);
        int16_t dx = 0;
        if (ball_x < pw / 2) {
            dx = (i == 0) ? -1 : 0;
        } else if (ball_x > pw / 2) {
            dx = (i == 0) ? 0 : 1;
        }
        const uint8_t px = (uint8_t)constrain((int)pup_x + dx, 2, 4);
        mtrx.rect(base + px, pup_y, base + px + 1, pup_y + 1, GFX_CLEAR);
    }
}

void anim_search() {
    static int8_t pos = 4, dir = 1;
    static Tmr tmr(50);
    if (tmr) {
        pos += dir;
        if (pos >= 6) dir = -1;
        if (pos <= 0) dir = 1;
        mtrx.rect(RadioConfig::analyzWidth, 2, RadioConfig::analyzWidth + 16 - 1, 5, GFX_FILL);
        draw_eyeb(0, pos, 3);
        draw_eyeb(1, pos, 3);
        mtrx.update();
    }
}
void change_state() {
    mtrx.clear();
    if (data.state) {
        upd_bright();
        square_tmr.start(600);
        draw_eye(0);
        draw_eye(1);
        draw_eyeb(0, 2, 2, 4);
        draw_eyeb(1, 2, 2, 4);
    } else {
        mtrx.setBright((uint8_t)0);
        draw_eye(0);
        draw_eye(1);
        mtrx.rect(RadioConfig::analyzWidth, 0, RadioConfig::analyzWidth + 16 - 1, 3, GFX_CLEAR);
        draw_eyeb(0, 3, 5);
        draw_eyeb(1, 3, 5);
    }
    mtrx.update();
}

// ========================= ANALYZ =========================
// Уровень волны: как «vol» в цикле матрицы, но g_pcm_vis читаем здесь — совпадает с тем, что в PCM-дебаге.
static uint8_t pcm_wave_level_after_gate() {
    uint8_t vw = g_pcm_vis;
    if (g_pcm_level_adc <= data.trsh) {
        return 0;
    }
    if (g_pcm_level_adc < data.trsh + 120) {
        return (uint8_t)((uint32_t)vw * (uint32_t)(g_pcm_level_adc - data.trsh) / 120u);
    }
    return vw;
}

void analyz0(uint8_t vol) {
    static uint32_t wave_phase;
    // Фаза всегда растёт (время + громкость) — иначе при vol≈0 шум визуально «мертвый».
    wave_phase += 10u + (uint32_t)vol * 6u;
    const uint16_t offs = (uint16_t)(wave_phase >> 1);
    for (uint8_t i = 0; i < RadioConfig::analyzWidth; i++) {
        int16_t val = inoise8(i * 50, offs);
        val -= 128;
        val = val * vol / 100;
        val += 128;
        val = map(val, 45, 255 - 45, 0, 7);
        mtrx.dot(i, val);
    }
}

// Режим 2: бегущая волна — каждый кадр сдвиг влево, справа уровень из g_pcm_wave_latest_half (обновляет PCM-колбэк).
static void pcm_wave_scroll_running() {
    const uint8_t n = (uint8_t)RadioConfig::pcmWaveBarCount;
    const uint8_t steps = RadioConfig::pcmWaveScrollStepsPerFrame;
    const uint8_t edge = g_pcm_wave_latest_half;
    for (uint8_t st = 0; st < steps; st++) {
        for (uint8_t i = 0; i < n - 1u; i++) {
            g_pcm_wave_amp[i] = g_pcm_wave_amp[i + 1u];
        }
        g_pcm_wave_amp[n - 1u] = edge;
    }
}

static void analyz_pcm_wave() {
    constexpr int8_t c0 = 3;
    constexpr int8_t c1 = 4;
    pcm_wave_scroll_running();
    const uint8_t n = (uint8_t)RadioConfig::pcmWaveBarCount;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t half = g_pcm_wave_amp[i];
        int16_t y0 = c0 - (int16_t)half;
        int16_t y1 = c1 + (int16_t)half;
        y0 = (int16_t)constrain((int32_t)y0, 0, 7);
        y1 = (int16_t)constrain((int32_t)y1, 0, 7);
        const int16_t x = (int16_t)i * 2;
        mtrx.lineV((int)x, (int)y0, (int)y1, GFX_FILL);
    }
}

// ========================= SYSTEM =========================
void audio_showstreamtitle(const char* info) {
}

void syncWifiWithAudioSilence() {
    if (!RadioConfig::wifiSleepWhenSilent) {
        return;
    }
    const bool silent = !data.state || data.vol <= 0;
    WiFi.setSleep(silent);
}

void core0(void* p) {
    // ========================= SETUP =========================
    EncButton eb(RadioConfig::encS1, RadioConfig::encS2, RadioConfig::encBtn);
    eb.setClickTimeout(480);
    Tmr viz_tmr(42);
    Tmr eye_tmr(150);
    Tmr matrix_tmr(1000);
    Tmr angry_tmr(800);
    Tmr pong_tmr(145);
    square_tmr.timerMode(1);
    matrix_tmr.timerMode(1);
    angry_tmr.timerMode(1);
    bool pulse = 0;
    uint8_t pcm_pulse_l = 0;

    EEPROM.begin(memory.blockSize());
    memory.begin(0, 'b');

    mtrx.begin();
    mtrx.setBright((uint8_t)0);
    mtrx.clear();
    mtrx.update();
    delay(RadioConfig::coldStartMatrixZeroMs);
    upd_bright();
    mtrx.update();

    delay(RadioConfig::coldStartAfterMatrixMs);

    audio.setBufsize(RadioConfig::radioBuffer, -1);
    audio.setPinout(RadioConfig::i2sBclk, RadioConfig::i2sLrc, RadioConfig::i2sDout);
    audio.setVolume(data.state ? data.vol : 0);
    data.station = constrain(data.station, 0, sizeof(stations) / sizeof(char*) - 1);
    reconnect = stations[data.station];
    syncWifiWithAudioSilence();

    // ========================= LOOP =========================
    for (;;) {
        square_tmr.tick();
        matrix_tmr.tick();
        angry_tmr.tick();
        memory.tick();

        const bool eb_e = (!wifiConnecting && eb.tick());

        // Только core0 трогает MAX7219: вызов anim_search с core1 в setup() давал гонку и мигание при Wi‑Fi.
        if (wifiConnecting) {
            anim_search();
        } else if (pong_active()) {
            if (pong_tmr.tick()) {
                pong_step();
                (void)pong_goal_fx_active();
                pong_draw();
                draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                pong_sync_matrix_brightness();
                mtrx.update();
            }
            if (eb_e) {
                if (eb.turn() && !eb.pressing()) {
                    pong_paddle_nudge(eb.dir());
                    pong_draw();
                    draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                    pong_sync_matrix_brightness();
                    mtrx.update();
                }
                if (eb.hasClicks()) {
                    const uint8_t n = eb.getClicks();
                    if (n == 1 && pong_serve_waiting()) {
                        pong_resume_after_goal();
                        pong_draw();
                        draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                        pong_sync_matrix_brightness();
                        mtrx.update();
                    } else if (n == 4) {
                        pong_set_active(false);
                        upd_bright();
                        mtrx.update();
                    }
                }
                memory.update();
            }
        } else {
            if (data.state && !square_tmr.state()) {
                if (eye_tmr) {
                    draw_eye(0);
                    draw_eye(1);
                    if (angry_tmr.state()) {
                        draw_eyeb(0, 3, 3);
                        draw_eyeb(1, 3, 3);
                        mtrx.lineH(0, RadioConfig::analyzWidth, RadioConfig::analyzWidth + 16 - 1, GFX_CLEAR);
                        mtrx.lineH(1, RadioConfig::analyzWidth + 5, RadioConfig::analyzWidth + 5 + 6 - 1, GFX_CLEAR);
                        mtrx.lineH(2, RadioConfig::analyzWidth + 6, RadioConfig::analyzWidth + 6 + 4 - 1, GFX_CLEAR);
                        mtrx.lineH(3, RadioConfig::analyzWidth + 7, RadioConfig::analyzWidth + 7 + 2 - 1, GFX_CLEAR);
                    } else {
                        if (eb.pressing()) {
                            draw_eyeb(0, 4, 3, 3);
                            draw_eyeb(1, 1, 3, 3);
                        } else {
                            static uint16_t pos;
                            pos += 15;
                            uint8_t x = inoise8(pos);
                            uint8_t y = inoise8(pos + UINT16_MAX / 4);
                            x = constrain(x, 40, 255 - 40);
                            y = constrain(y, 40, 255 - 40);
                            x = map(x, 40, 255 - 40, 2, 5);
                            y = map(y, 40, 255 - 40, 2, 5);
                            if (pulse) {
                                pulse = 0;
                                int8_t sx = random(-1, 1);
                                int8_t sy = random(-1, 1);
                                draw_eyeb(0, x + sx, y + sy, 3);
                                draw_eyeb(1, x + sx, y + sy, 3);
                            } else {
                                draw_eyeb(0, x, y);
                                draw_eyeb(1, x, y);
                            }
                        }
                    }
                    mtrx.update();
                }
            }

            // Режим 2 — при выкл. радио; режим 0 (волна) тоже без data.state — иначе при state==0 кадр не идёт и волна замирает.
            if (viz_tmr && !matrix_tmr.state() && (data.state || data.mode == 2 || data.mode == 0)) {
                uint8_t vol = g_pcm_vis;
                if (g_pcm_level_adc <= data.trsh) {
                    vol = 0;
                } else if (g_pcm_level_adc < data.trsh + 120) {
                    vol = (uint8_t)((uint32_t)vol * (uint32_t)(g_pcm_level_adc - data.trsh) / 120u);
                }
                if (vol > pcm_pulse_l + 12) {
                    pulse = 1;
                }
                pcm_pulse_l = (uint8_t)((pcm_pulse_l * 3u + vol) / 4u);

                mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
                switch (data.mode) {
                    case 0:
                        analyz0(pcm_wave_level_after_gate());
                        break;
                    case 2:
                        analyz_pcm_wave();
                        break;
                    default:
                        // Был режим 1 (удалён) или мусор в EEPROM — сброс на волну.
                        data.mode = 0;
                        analyz0(pcm_wave_level_after_gate());
                        break;
                }
                mtrx.update();
            }

            if (eb_e) {
                static bool station_changed = 0;

                if (eb.turn()) {
                    if (eb.pressing()) {
                        switch (eb.getClicks()) {
                            case 0:
                                data.station += eb.dir();
                                data.station = constrain(data.station, 0, sizeof(stations) / sizeof(char*) - 1);
                                print_val('s', data.station);
                                matrix_tmr.start();
                                station_changed = 1;
                                break;
                            case 2: {
                                const int8_t d = eb.dir();
                                data.bright_mouth += d;
                                data.bright_eyes += d;
                                data.bright_mouth = constrain(data.bright_mouth, 0, 16);
                                data.bright_eyes = constrain(data.bright_eyes, 0, 16);
                                upd_bright();
                                break;
                            }
                            default:
                                break;
                        }
                    } else {
                        if (data.state) {
                            angry_tmr.start();
                            data.vol += eb.dir();
                            data.vol = constrain(data.vol, 0, 21);
                            audio.setVolume(data.vol);
                            syncWifiWithAudioSilence();
                            print_val('v', data.vol);
                            matrix_tmr.start();
                        }
                    }
                }

                if (eb.hasClicks()) {
                    switch (eb.getClicks()) {
                        case 1:
                            data.state = !data.state;
                            if (!data.state) {
                                audio.setVolume(0);
                                audio.stopSong();
                            } else {
                                reconnect = stations[data.station];
                                audio.setVolume(data.vol);
                            }
                            syncWifiWithAudioSilence();
                            change_state();
                            break;
                        case 2:
                            data.mode = (data.mode == 0) ? 2 : 0;
                            print_val('m', data.mode);
                            matrix_tmr.start();
                            break;
                        case 3:
                            data.trsh = (uint16_t)constrain((int)g_pcm_level_adc * 2 / 3, 4, 3800);
                            break;
                        case 4:
                            pong_start();
                            pong_tmr.start();
                            pong_draw();
                            draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                            pong_sync_matrix_brightness();
                            mtrx.update();
                            break;
                    }
                }

                if (eb.release()) {
                    if (station_changed) {
                        station_changed = 0;
                        reconnect = stations[data.station];
                    }
                }
                memory.update();
            }
        }

        // vTaskDelay(1);
        TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;  // write enable
        TIMERG0.wdt_feed = 1;                        // feed dog
        TIMERG0.wdt_wprotect = 0;                    // write protect
    }
}