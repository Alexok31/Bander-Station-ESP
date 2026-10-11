#pragma once

#include <cstdint>

bool pong_active();
void pong_set_active(bool on);
bool pong_menu_active();
void pong_open_menu();
void pong_start();
void pong_reset();
void pong_step();
uint16_t pong_step_interval_ms();
uint16_t pong_score();
uint16_t pong_record();
void pong_save_records();
void pong_draw();
void pong_paddle_nudge(int8_t enc_dir);
int8_t pong_ball_x();
int8_t pong_ball_y();
uint8_t pong_score_player();
uint8_t pong_score_ai();
bool pong_goal_fx_active();
int8_t pong_goal_fx_side();
bool pong_serve_waiting();
void pong_resume_after_goal();
bool pong_mouth_tablo_mode();
