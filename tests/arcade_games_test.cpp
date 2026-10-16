#include "../BendeRadio/ArcadeGames.h"
#include <cassert>

constexpr bool tennisWallTest() {
    SoloPong::Game tennis(24);
    tennis.serve(1);
    tennis.nudge(1);
    if (tennis.left != 3 || tennis.intervalMs() != 180) return false;
    tennis.x = 1; tennis.y = 3; tennis.vx = -1; tennis.vy = 1;
    if (tennis.step(1) || tennis.score != 1 || tennis.vx != 1 || tennis.intervalMs() != 176) return false;
    for (int y = 0; y < 8; ++y) {
        tennis.x = 22; tennis.y = y; tennis.vx = 1; tennis.vy = 0;
        if (tennis.step(1) || tennis.waiting || tennis.vx != -1 || tennis.score != 1) return false;
    }
    tennis.score = UINT16_MAX;
    tennis.x = 1; tennis.y = 4; tennis.vx = -1; tennis.vy = 0;
    if (tennis.step(1) || tennis.score != UINT16_MAX || tennis.intervalMs() != 90) return false;
    tennis.serve(1);
    tennis.x = 1; tennis.y = 7; tennis.vx = -1; tennis.vy = 1;
    if (!tennis.step(1) || !tennis.waiting || tennis.score != 0 || tennis.x != 0 || tennis.y != 7) return false;
    tennis.serve(0);
    return !tennis.waiting && tennis.score == 0 && tennis.intervalMs() == 180 && tennis.vx == 1;
}
static_assert(tennisWallTest(), "Wall returns, scoring, acceleration, miss and restart must work");

int main() {
    SoloSnake::Game snake(24);
    snake.serve(1);
    snake.food = 4 * 24 + 13;
    assert(!snake.step(2));
    assert(snake.score() == 1 && snake.length == 4);
    snake.turn(1); // down
    assert(!snake.step(3));
    snake.turn(-1); // right
    snake.turn(-1); // ignored until the next movement step
    assert(!snake.step(4));
    assert(snake.direction == 0 && snake.body[0] == 5 * 24 + 14);
    snake.reset();
    snake.body[0] = 4 * 24 + 23;
    snake.body[1] = 4 * 24 + 22;
    snake.body[2] = 4 * 24 + 21;
    snake.waiting = false;
    assert(!snake.step(5) && snake.body[0] == 4 * 24); // wraps at right edge
    snake.body[0] = 7 * 24 + 5;
    snake.body[1] = 7 * 24 + 4;
    snake.body[2] = 7 * 24 + 3;
    snake.direction = 1;
    assert(!snake.step(5) && snake.body[0] == 5); // wraps at bottom
    snake.body[0] = 5 * 24 + 5;
    snake.body[1] = 5 * 24 + 6;
    snake.body[2] = 6 * 24 + 6;
    snake.direction = 0;
    assert(snake.step(5) && snake.waiting && snake.hitSelf);
    assert(snake.hitCell == 5 * 24 + 6);

    SoloDodge::Game dodge(24);
    dodge.serve(0);
    dodge.rows[6] = uint32_t(1) << dodge.player;
    assert(dodge.step(1));
    assert(dodge.waiting);
    dodge.serve(0);
    dodge.rows[7] = uint32_t(1) << (dodge.player + 2);
    assert(dodge.nudge(1)); // moving into an obstacle is a hit too

    assert(tennisWallTest());
}
