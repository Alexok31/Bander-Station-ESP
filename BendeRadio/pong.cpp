#include "pong.h"

#include <Arduino.h>
#include <GyverMAX7219.h>
#include <Preferences.h>
#include <cstring>

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
constexpr uint32_t missDurationMs = 1600;
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
    return selected == 1 ? snake.score() : selected == 2 ? dodge.score
           : tennis.score;
}

bool waitingNow() {
    return selected == 1 ? snake.waiting : selected == 2 ? dodge.waiting
           : tennis.waiting;
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
    }
    for (uint8_t y = 0; y < 8; ++y) mtrx.dot(width - 1, y);
    mtrx.dot(uint8_t(tennis.x), uint8_t(tennis.y));
}

void drawSnake() {
    for (uint16_t i = 0; i < snake.length; ++i)
        mtrx.dot(snake.body[i] % width, snake.body[i] / width);
    // Food blinks; the solid dots are the snake. This makes a self-hit readable.
    if (snake.length < SoloSnake::Game::capacity && ((millis() / 350u) & 1u) == 0)
        mtrx.dot(snake.food % width, snake.food / width);
}

void drawLossBlink() {
    if (menu || !missEffect) return;
    const uint32_t elapsed = uint32_t(millis() - missStarted);
    if (elapsed >= missDurationMs) return;
    const bool lit = (elapsed / 200u) & 1u;
    if (selected == 0) {
        mtrx.dot(tennis.x, tennis.y, lit);
    } else if (selected == 1 && snake.hitSelf) {
        mtrx.dot(snake.hitCell % width, snake.hitCell / width, lit);
    } else if (selected == 2) {
        for (int16_t x = int16_t(dodge.player) - 1; x <= int16_t(dodge.player) + 1; ++x)
            if (x >= 0 && x < width && (dodge.rows[7] & (uint32_t(1) << x)))
                mtrx.dot(x, 7, lit);
    }
}

void drawDodge() {
    for (uint8_t y = 0; y < 8; ++y)
        for (uint8_t x = 0; x < width; ++x)
            if (dodge.rows[y] & (uint32_t(1) << x)) mtrx.dot(x, y);
    mtrx.dot(dodge.player, 7);
    if (dodge.player > 0) mtrx.dot(dodge.player - 1, 7);
    if (dodge.player + 1 < width) mtrx.dot(dodge.player + 1, 7);
}

struct MenuGlyph {
    uint8_t width;
    uint8_t rows[5];
};

const MenuGlyph* menuGlyph(char letter) {
    // Tiny English capitals, sized so every game name fits the 24-pixel mouth.
    static const MenuGlyph glyphs[] = {
        {3, {7, 2, 2, 2, 2}},      // T
        {3, {7, 4, 6, 4, 7}},      // E
        {4, {9, 13, 11, 9, 9}},    // N
        {1, {1, 1, 1, 1, 1}},      // I
        {3, {7, 4, 7, 1, 7}},      // S
        {3, {2, 5, 7, 5, 5}},      // A
        {3, {5, 5, 6, 5, 5}},      // K
        {4, {14, 9, 9, 9, 14}},   // D
        {3, {7, 5, 5, 5, 7}},      // O
        {3, {7, 4, 5, 5, 7}},      // G
    };
    constexpr char letters[] = "TENISAKDOG";
    for (uint8_t i = 0; i < sizeof(letters) - 1; ++i)
        if (letters[i] == letter) return &glyphs[i];
    return nullptr;
}

void drawMenu() {
    static const char* const names[] = {"TENNIS", "SNAKE", "DODGE"};
    const char* name = names[selected];
    const uint8_t length = strlen(name);
    int16_t textWidth = -1;
    for (uint8_t i = 0; i < length; ++i) {
        const MenuGlyph* glyph = menuGlyph(name[i]);
        if (glyph) textWidth += glyph->width + 1;
    }
    int16_t xStart = (width - textWidth) / 2;
    for (uint8_t i = 0; i < length; ++i) {
        const MenuGlyph* glyph = menuGlyph(name[i]);
        if (!glyph) continue;
        for (uint8_t row = 0; row < 5; ++row) {
            const uint8_t bits = glyph->rows[row];
            for (uint8_t col = 0; col < glyph->width; ++col) {
                const int16_t x = xStart + col;
                if (x >= 0 && x < width && (bits & (1u << (glyph->width - 1 - col)))) mtrx.dot(x, row + 2);
            }
        }
        xStart += glyph->width + 1;
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
                      : selected == 2 ? dodge.step(seed)
                      : tennis.step(seed);
    updateRecord();
    if (missed) {
        missStarted = millis();
        missEffect = true;
        pong_save_records();
    }
}
uint16_t pong_step_interval_ms() {
    if (!menu && waitingNow()) return 50u; // Smooth loss blinking, independent of game speed.
    return menu ? 100u : selected == 1 ? snake.intervalMs()
           : selected == 2 ? dodge.intervalMs()
           : tennis.intervalMs();
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
    else if (selected == 0) tennis.serve(seed);
    missEffect = false;
}
void pong_paddle_nudge(int8_t dir) {
    if (!active || !dir) return;
    if (menu) {
        selected = uint8_t((selected + (dir > 0 ? 1 : gameCount - 1)) % gameCount);
    }
    else if (selected == 0) tennis.nudge(dir);
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
    drawLossBlink();
}
int8_t pong_ball_x() { return tennis.x; }
int8_t pong_ball_y() { return tennis.y; }
uint8_t pong_score_player() { return uint8_t(scoreNow() % 10u); }
uint8_t pong_score_ai() { return uint8_t(pong_record() % 10u); }
bool pong_goal_fx_active() {
    if (missEffect && uint32_t(millis() - missStarted) >= missDurationMs) missEffect = false;
    return missEffect;
}
int8_t pong_goal_fx_side() { return 0; }
bool pong_mouth_tablo_mode() { return menu || waitingNow(); }
