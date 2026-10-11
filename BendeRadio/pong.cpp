#include "pong.h"

#include <Arduino.h>
#include <GyverMAX7219.h>
#include <Preferences.h>

#include "ArcadeGames.h"
#include "RadioConfig.h"

extern MAX7219<5, 1, RadioConfig::mtrxCs, RadioConfig::mtrxDat, RadioConfig::mtrxClk> mtrx;

namespace {
constexpr uint8_t width = RadioConfig::analyzWidth;
constexpr uint8_t gameCount = 3;
static_assert(width == 24, "Arcade field and snake storage must fit the 24x8 mouth");
SoloPong::Game tennis(width);
SoloSnake::Game snake(width);
SoloDodge::Game dodge(width);
bool active = false;
bool menu = true;
uint8_t selected = 0; // 0 tennis, 1 snake, 2 dodge
uint16_t records[gameCount] = {};
bool dirty[gameCount] = {};
bool recordsLoaded = false;
uint32_t missStarted = 0;
bool missEffect = false;
constexpr const char* recordKeys[gameCount] = {"pong_best", "snake_best", "dodge_best"};

void loadRecords() {
    if (recordsLoaded) return;
    Preferences prefs;
    if (prefs.begin("bende", true)) {
        for (uint8_t i = 0; i < gameCount; ++i) records[i] = prefs.getUShort(recordKeys[i], 0);
        prefs.end();
    }
    recordsLoaded = true;
}

uint16_t scoreNow() {
    return selected == 1 ? snake.score() : selected == 2 ? dodge.score : tennis.score;
}

bool waitingNow() {
    return selected == 1 ? snake.waiting : selected == 2 ? dodge.waiting : tennis.waiting;
}

void updateRecord() {
    if (menu) return;
    const uint16_t score = scoreNow();
    if (score > records[selected]) {
        records[selected] = score;
        dirty[selected] = true;
    }
}

void clearMouth() { mtrx.rect(0, 0, width - 1, 7, GFX_CLEAR); }

void drawTennis() {
    for (uint8_t y = 0; y < 3; ++y) {
        mtrx.dot(0, uint8_t(tennis.left + y));
        mtrx.dot(width - 1, uint8_t(tennis.right + y));
    }
    mtrx.dot(uint8_t(tennis.x), uint8_t(tennis.y));
}

void drawSnake() {
    for (uint16_t i = 0; i < snake.length; ++i)
        mtrx.dot(snake.body[i] % width, snake.body[i] / width);
    if (snake.length < SoloSnake::Game::capacity)
        mtrx.dot(snake.food % width, snake.food / width);
}

void drawDodge() {
    for (uint8_t y = 0; y < 8; ++y)
        for (uint8_t x = 0; x < width; ++x)
            if (dodge.rows[y] & (uint32_t(1) << x)) mtrx.dot(x, y);
    mtrx.dot(dodge.player, 7);
    if (dodge.player > 0) mtrx.dot(dodge.player - 1, 7);
    if (dodge.player + 1 < width) mtrx.dot(dodge.player + 1, 7);
}

void drawMenu() {
    // Turning the encoder previews one of three games on the mouth.
    if (selected == 0) {
        for (uint8_t y = 2; y <= 4; ++y) { mtrx.dot(3, y); mtrx.dot(width - 4, y); }
        mtrx.dot(width / 2, 3);
    } else if (selected == 1) {
        for (uint8_t x = 7; x <= 14; ++x) mtrx.dot(x, uint8_t(3 + ((x / 2) & 1)));
        mtrx.dot(18, 3);
    } else {
        mtrx.dot(width / 2, 7);
        mtrx.dot(width / 2 - 1, 7);
        mtrx.dot(width / 2 + 1, 7);
        mtrx.dot(7, 1); mtrx.dot(8, 1);
        mtrx.dot(17, 3); mtrx.dot(18, 3);
    }
}
}

void pong_save_records() {
    updateRecord();
    bool any = false;
    for (bool value : dirty) any |= value;
    if (!any) return;
    Preferences prefs;
    if (!prefs.begin("bende", false)) {
        Serial.println(F("[Game] record save failed"));
        return;
    }
    for (uint8_t i = 0; i < gameCount; ++i) if (dirty[i]) {
        if (prefs.putUShort(recordKeys[i], records[i]) == sizeof(uint16_t)) dirty[i] = false;
        else Serial.println(F("[Game] record write failed"));
    }
    prefs.end();
}

bool pong_active() { return active; }
void pong_set_active(bool on) {
    if (!on) pong_save_records();
    active = on;
}
bool pong_menu_active() { return menu; }
void pong_open_menu() {
    pong_save_records();
    menu = true;
    missEffect = false;
}
void pong_start() {
    loadRecords();
    menu = true;
    selected = 0;
    tennis.reset(); snake.reset(); dodge.reset();
    missEffect = false;
    active = true;
}
void pong_reset() {
    pong_save_records();
    tennis.reset(); snake.reset(); dodge.reset();
    missEffect = false;
}
void pong_step() {
    if (!active || menu || waitingNow()) return;
    const uint32_t seed = esp_random();
    const bool missed = selected == 1 ? snake.step(seed)
                      : selected == 2 ? dodge.step(seed) : tennis.step(seed);
    updateRecord();
    if (missed) {
        missStarted = millis();
        missEffect = true;
        pong_save_records();
    }
}
uint16_t pong_step_interval_ms() {
    return menu ? 100u : selected == 1 ? snake.intervalMs()
           : selected == 2 ? dodge.intervalMs() : tennis.intervalMs();
}
uint16_t pong_score() { return menu ? 0 : scoreNow(); }
uint16_t pong_record() { loadRecords(); return records[selected]; }
bool pong_serve_waiting() { return menu || waitingNow(); }
void pong_resume_after_goal() {
    if (!active || !pong_serve_waiting()) return;
    pong_save_records();
    menu = false;
    const uint32_t seed = esp_random();
    if (selected == 1) snake.serve(seed);
    else if (selected == 2) dodge.serve(seed);
    else tennis.serve(seed);
    missEffect = false;
}
void pong_paddle_nudge(int8_t dir) {
    if (!active || !dir) return;
    if (menu) selected = uint8_t((selected + (dir > 0 ? 1 : gameCount - 1)) % gameCount);
    else if (selected == 1) snake.turn(dir);
    else if (selected == 2 && dodge.nudge(dir)) {
        missStarted = millis();
        missEffect = true;
        pong_save_records();
    }
}
void pong_draw() {
    clearMouth();
    if (!active) return;
    if (menu) drawMenu();
    else if (selected == 1) drawSnake();
    else if (selected == 2) drawDodge();
    else drawTennis();
    if (missEffect && uint32_t(millis() - missStarted) < 520u) {
        mtrx.dot(width / 2 - 1, 3);
        mtrx.dot(width / 2 + 1, 3);
        mtrx.dot(width / 2, 4);
    }
}
int8_t pong_ball_x() { return tennis.x; }
int8_t pong_ball_y() { return tennis.y; }
uint8_t pong_score_player() { return uint8_t(scoreNow() % 10u); }
uint8_t pong_score_ai() { return uint8_t(pong_record() % 10u); }
bool pong_goal_fx_active() {
    if (missEffect && uint32_t(millis() - missStarted) >= 520u) missEffect = false;
    return missEffect;
}
int8_t pong_goal_fx_side() { return 0; }
bool pong_mouth_tablo_mode() { return menu || waitingNow(); }
