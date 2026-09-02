/**
 * @file buttons.h
 * @brief 按键管理 — 6 键扫描、去抖、长按检测、重复触发
 */

#ifndef BUTTONS_H
#define BUTTONS_H

#include "config.h"

// 按键事件
enum ButtonEvent : uint8_t {
    BTN_EVENT_NONE = 0,
    BTN_EVENT_PRESS,      // 短按
    BTN_EVENT_LONG_PRESS, // 长按
    BTN_EVENT_REPEAT,     // 连续重复
    BTN_EVENT_RELEASE     // 释放
};

// 按键 ID
enum ButtonID : uint8_t {
    BTN_ID_UP = 0,
    BTN_ID_DOWN,
    BTN_ID_LEFT,
    BTN_ID_RIGHT,
    BTN_ID_A,
    BTN_ID_B,
    BTN_ID_COUNT
};

/**
 * @brief 初始化所有按键引脚
 */
void buttons_init();

/**
 * @brief 非阻塞按键扫描，返回事件
 * @param btn 按键 ID
 * @return ButtonEvent 事件类型
 */
ButtonEvent buttons_get_event(ButtonID btn);

/**
 * @brief 检查按键当前是否按下
 */
bool buttons_is_pressed(ButtonID btn);

/**
 * @brief 检查任意按键是否按下（用于锁屏解锁）
 */
bool buttons_any_pressed();

/**
 * @brief 获取上次按键活动时间
 */
uint32_t buttons_last_activity();

/**
 * @brief 更新按键活动时间戳
 */
void buttons_update_activity();

/**
 * @brief 重置所有按键状态
 */
void buttons_reset_all();

#endif // BUTTONS_H