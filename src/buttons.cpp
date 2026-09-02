/**
 * @file buttons.cpp
 * @brief 按键管理实现
 */

#include "buttons.h"

static const uint8_t btn_pins[BTN_ID_COUNT] = {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B
};

struct ButtonState {
    bool raw;              // 当前原始状态 (LOW=按下)
    bool last_raw;         // 上一次原始状态
    bool stable;           // 去抖后稳定状态
    uint32_t last_change;  // 上次变化时间
    uint32_t press_time;   // 按下时间
    bool long_pressed;     // 已触发长按
    bool repeating;        // 重复模式
    uint32_t last_repeat;  // 上次重复时间
    bool event_handled;    // 事件已处理
};

static ButtonState states[BTN_ID_COUNT];
static uint32_t last_activity_time = 0;

void buttons_init() {
    for (uint8_t i = 0; i < BTN_ID_COUNT; i++) {
        pinMode(btn_pins[i], INPUT_PULLUP);
        states[i].raw = true;
        states[i].last_raw = true;
        states[i].stable = true;
        states[i].last_change = 0;
        states[i].press_time = 0;
        states[i].long_pressed = false;
        states[i].repeating = false;
        states[i].last_repeat = 0;
        states[i].event_handled = true;
    }
}

ButtonEvent buttons_get_event(ButtonID btn) {
    if (btn >= BTN_ID_COUNT) return BTN_EVENT_NONE;
    
    ButtonState& s = states[btn];
    uint32_t now = millis();
    
    // 读取原始状态（低电平 = 按下）
    s.raw = (digitalRead(btn_pins[btn]) == LOW);
    
    // 去抖
    if (s.raw != s.last_raw) {
        s.last_change = now;
    }
    s.last_raw = s.raw;
    
    if ((now - s.last_change) >= BTN_DEBOUNCE_MS) {
        // 稳定状态变化
        if (s.raw != s.stable) {
            s.stable = s.raw;
            
            if (s.stable) {
                // 按下
                s.press_time = now;
                s.long_pressed = false;
                s.repeating = false;
                s.event_handled = false;
                last_activity_time = now;
                return BTN_EVENT_PRESS;
            } else {
                // 释放
                s.press_time = 0;
                s.repeating = false;
                s.event_handled = true;
                return BTN_EVENT_RELEASE;
            }
        }
        
        // 检查长按
        if (s.stable && !s.long_pressed && (now - s.press_time) >= BTN_LONG_PRESS_MS) {
            s.long_pressed = true;
            if (btn == BTN_ID_B) {
                last_activity_time = now;
                return BTN_EVENT_LONG_PRESS;
            }
        }
        
        // 检查重复
        if (s.stable && !s.repeating && (now - s.press_time) >= BTN_REPEAT_DELAY) {
            s.repeating = true;
            s.last_repeat = now;
            last_activity_time = now;
            return BTN_EVENT_REPEAT;
        }
        
        if (s.stable && s.repeating && (now - s.last_repeat) >= BTN_REPEAT_RATE) {
            s.last_repeat = now;
            last_activity_time = now;
            return BTN_EVENT_REPEAT;
        }
    }
    
    return BTN_EVENT_NONE;
}

bool buttons_is_pressed(ButtonID btn) {
    if (btn >= BTN_ID_COUNT) return false;
    return states[btn].stable;
}

bool buttons_any_pressed() {
    for (uint8_t i = 0; i < BTN_ID_COUNT; i++) {
        if (states[i].stable) return true;
    }
    return false;
}

uint32_t buttons_last_activity() {
    return last_activity_time;
}

void buttons_update_activity() {
    last_activity_time = millis();
}

void buttons_reset_all() {
    for (uint8_t i = 0; i < BTN_ID_COUNT; i++) {
        states[i].press_time = 0;
        states[i].long_pressed = false;
        states[i].repeating = false;
        states[i].event_handled = true;
    }
}