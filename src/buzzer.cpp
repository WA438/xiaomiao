/**
 * @file buzzer.cpp
 * @brief 蜂鸣器管理实现 — 使用 ledcSetup/ledcWrite 产生方波
 */

#include "buzzer.h"

static bool buzzer_active = false;
static uint32_t buzzer_stop_time = 0;
static uint8_t alarm_repeat = 0;
static uint8_t alarm_count = 0;
static uint32_t alarm_next = 0;
static bool alarm_state = false;

void buzzer_init() {
    ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RESOLUTION);
    ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
    ledcWrite(BUZZER_CHANNEL, 0);  // 静音
}

void buzzer_beep(uint16_t freq, uint16_t duration_ms) {
    ledcWriteTone(BUZZER_CHANNEL, freq);
    ledcWrite(BUZZER_CHANNEL, 128);  // 50% duty
    buzzer_active = true;
    if (duration_ms > 0) {
        buzzer_stop_time = millis() + duration_ms;
    } else {
        buzzer_stop_time = 0;  // 持续
    }
    alarm_repeat = 0;
}

void buzzer_stop() {
    ledcWrite(BUZZER_CHANNEL, 0);
    buzzer_active = false;
    buzzer_stop_time = 0;
    alarm_repeat = 0;
}

void buzzer_click() {
    buzzer_beep(1000, 30);
}

void buzzer_scan_done() {
    buzzer_beep(1500, 80);
    buzzer_stop_time = millis() + 80;
}

void buzzer_attack_toggle() {
    buzzer_beep(800, 300);
}

void buzzer_alarm(uint8_t repeat) {
    alarm_repeat = repeat;
    alarm_count = 0;
    alarm_next = millis();
    alarm_state = true;
    ledcWriteTone(BUZZER_CHANNEL, 2000);
    ledcWrite(BUZZER_CHANNEL, 128);
}

void buzzer_wake() {
    buzzer_beep(2000, 15);
}

void buzzer_update() {
    uint32_t now = millis();
    
    // 单次 beep 超时
    if (buzzer_active && buzzer_stop_time > 0 && now >= buzzer_stop_time && alarm_repeat == 0) {
        buzzer_stop();
        return;
    }
    
    // 连续报警
    if (alarm_repeat > 0 && alarm_count < alarm_repeat) {
        if (now >= alarm_next) {
            if (alarm_state) {
                // 响
                ledcWriteTone(BUZZER_CHANNEL, 2000);
                ledcWrite(BUZZER_CHANNEL, 128);
                alarm_next = now + 80;
            } else {
                // 停
                ledcWrite(BUZZER_CHANNEL, 0);
                alarm_next = now + 120;
                alarm_count++;
            }
            alarm_state = !alarm_state;
        }
        return;
    }
    
    if (alarm_repeat > 0 && alarm_count >= alarm_repeat) {
        buzzer_stop();
    }
}