#include "../BendeRadio/ArcadeGames.h"
#include <cassert>

int main() {
    SoloSnake::Game snake(24);
    snake.serve(1);
    snake.food = 4 * 24 + 13;
    assert(!snake.step(2));
    assert(snake.score() == 1 && snake.length == 4);
    snake.turn(1); // down
    assert(!snake.step(3));
    snake.turn(-1); // right
    assert(!snake.step(4));
    snake.turn(-1); // up
    while (!snake.waiting) snake.step(5);
    assert(snake.score() == 1); // collision keeps the final score

    SoloDodge::Game dodge(24);
    dodge.serve(0);
    dodge.rows[6] = uint32_t(1) << dodge.player;
    assert(dodge.step(1));
    assert(dodge.waiting);
    dodge.serve(0);
    dodge.rows[7] = uint32_t(1) << (dodge.player + 2);
    assert(dodge.nudge(1)); // moving into an obstacle is a hit too

    SoloPong::Game tennis(24);
    tennis.serve(1);
    tennis.x = 1; tennis.y = 2; tennis.vx = -1; tennis.vy = 1;
    tennis.left = 2;
    assert(!tennis.step(1));
    assert(tennis.score == 1 && tennis.vx == 1);
    tennis.x = 1; tennis.y = 0; tennis.vx = -1; tennis.vy = -1;
    tennis.left = 5;
    assert(tennis.step(1) && tennis.waiting);
    assert(tennis.score == 1);
}
