#include "pong.h"

#include <Arduino.h>
#include <GyverMAX7219.h>

#include "RadioConfig.h"

extern MAX7219<5, 1, RadioConfig::mtrxCs, RadioConfig::mtrxDat, RadioConfig::mtrxClk> mtrx;

static constexpr uint8_t kPw = RadioConfig::analyzWidth;
static constexpr uint8_t kPh = 8;
static constexpr uint8_t kPaddleH = 3;

static bool g_active = false;
static int8_t g_py = 3;
static int8_t g_aiy = 3;
static int8_t g_bx = 12;
static int8_t g_by = 4;
static int8_t g_vx = 1;
static int8_t g_vy = 1;
static uint8_t g_score_pl = 0;
static uint8_t g_score_ai = 0;
static uint32_t g_goal_fx_until = 0;
static int8_t g_goal_fx_side = 0;
static bool g_serve_wait = false;

static constexpr uint32_t kGoalFxMs = 520;

static void pong_reset_ball() {
    g_bx = kPw / 2;
    g_by = kPh / 2;
    g_vx = (random(0, 2) ? 1 : -1);
    g_vy = (random(0, 2) ? 1 : -1);
}

static void register_goal(int8_t side) {
    g_goal_fx_until = millis() + kGoalFxMs;
    g_goal_fx_side = side;
}

static bool mouth_tablo_mode_impl() {
    if (g_serve_wait) {
        return true;
    }
    if (g_goal_fx_until == 0) {
        return false;
    }
    return (int32_t)(millis() - g_goal_fx_until) < 0;
}

// Рот: только «поле» ожидания подачи / после гола — точка в центре и ракетки по краям.
// Счёт выводится на глазах (см. draw_eyes_follow_ball в core0.cpp).
static void draw_mouth_tablo() {
    mtrx.rect(0, 0, kPw - 1, kPh - 1, GFX_CLEAR);
    mtrx.dot((uint8_t)(kPw / 2), (uint8_t)(kPh / 2));
    for (uint8_t i = 0; i < kPaddleH; i++) {
        mtrx.dot(0, (uint8_t)(g_py + i));
        mtrx.dot(kPw - 1, (uint8_t)(g_aiy + i));
    }
}

bool pong_mouth_tablo_mode() {
    return mouth_tablo_mode_impl();
}

bool pong_active() {
    return g_active;
}

void pong_set_active(bool on) {
    g_active = on;
    if (!on) {
        g_serve_wait = false;
    }
}

void pong_reset() {
    g_py = (int8_t)((kPh - kPaddleH) / 2);
    g_aiy = (int8_t)((kPh - kPaddleH) / 2);
    pong_reset_ball();
}

void pong_start() {
    g_active = true;
    g_score_pl = 0;
    g_score_ai = 0;
    g_goal_fx_until = 0;
    g_goal_fx_side = 0;
    pong_reset();
    g_serve_wait = true;
}

void pong_step() {
    if (g_serve_wait) {
        return;
    }

    if (random(0, 3) != 0) {
        const int mid = g_aiy + kPaddleH / 2;
        if (mid < g_by && g_aiy < (int8_t)(kPh - kPaddleH)) {
            g_aiy++;
        } else if (mid > g_by && g_aiy > 0) {
            g_aiy--;
        }
    }

    g_by += g_vy;
    if (g_by < 0) {
        g_by = 0;
        g_vy = 1;
    } else if (g_by >= (int8_t)kPh) {
        g_by = kPh - 1;
        g_vy = -1;
    }

    // Ракетка в колонках 0 и kPw-1; мяч не должен в них «залипать» — отражаем в 1 и kPw-2.
    const int16_t next_bx = (int16_t)g_bx + g_vx;

    if (g_vx < 0 && next_bx <= 0) {
        const bool hit = (g_by >= g_py && g_by < g_py + (int8_t)kPaddleH);
        if (hit) {
            g_bx = 1;
            g_vx = 1;
        } else {
            if (g_score_ai < 9) {
                g_score_ai++;
            }
            pong_reset_ball();
            register_goal(-1);
            g_serve_wait = true;
        }
    } else if (g_vx > 0 && next_bx >= (int16_t)kPw - 1) {
        const bool hit = (g_by >= g_aiy && g_by < g_aiy + (int8_t)kPaddleH);
        if (hit) {
            g_bx = (int8_t)(kPw - 2);
            g_vx = -1;
        } else {
            if (g_score_pl < 9) {
                g_score_pl++;
            }
            pong_reset_ball();
            register_goal(1);
            g_serve_wait = true;
        }
    } else {
        g_bx = (int8_t)next_bx;
    }
}

void pong_draw() {
    if (mouth_tablo_mode_impl()) {
        draw_mouth_tablo();
        return;
    }
    mtrx.rect(0, 0, kPw - 1, kPh - 1, GFX_CLEAR);
    for (uint8_t i = 0; i < kPaddleH; i++) {
        mtrx.dot(0, (uint8_t)(g_py + i));
        mtrx.dot(kPw - 1, (uint8_t)(g_aiy + i));
    }
    mtrx.dot((uint8_t)constrain(g_bx, 0, kPw - 1), (uint8_t)constrain(g_by, 0, kPh - 1));
}

int8_t pong_ball_x() {
    return constrain(g_bx, 0, kPw - 1);
}

int8_t pong_ball_y() {
    return constrain(g_by, 0, kPh - 1);
}

uint8_t pong_score_player() {
    return g_score_pl;
}

uint8_t pong_score_ai() {
    return g_score_ai;
}

bool pong_goal_fx_active() {
    if (g_goal_fx_until == 0) {
        return false;
    }
    if ((int32_t)(millis() - g_goal_fx_until) >= 0) {
        g_goal_fx_until = 0;
        g_goal_fx_side = 0;
        return false;
    }
    return true;
}

int8_t pong_goal_fx_side() {
    return g_goal_fx_side;
}

bool pong_serve_waiting() {
    return g_serve_wait;
}

void pong_resume_after_goal() {
    g_serve_wait = false;
}

void pong_paddle_nudge(int8_t enc_dir) {
    g_py += enc_dir;
    g_py = constrain(g_py, 0, (int8_t)(kPh - kPaddleH));
}
