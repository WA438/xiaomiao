/**
 * @file buzzer.h
 * @brief 蜂鸣器管理 — PWM 驱动音效
 */

#ifndef BUZZER_H
#define BUZZER_H

#include "config.h"

/**
 * @brief 初始化蜂鸣器 PWM
 */
void buzzer_init();

/**
 * @brief 蜂鸣器发声
 * @param freq 频率 Hz
 * @param duration_ms 持续时间 ms (0=持续, 需手动停止)
 */
void buzzer_beep(uint16_t freq, uint16_t duration_ms);

/**
 * @brief 停止蜂鸣器
 */
void buzzer_stop();

/**
 * @brief 短"滴"声 — 按键反馈
 */
void buzzer_click();

/**
 * @brief 扫描完成提示音
 */
void buzzer_scan_done();

/**
 * @brief 攻击启动/停止长鸣
 */
void buzzer_attack_toggle();

/**
 * @brief 连续短响报警 — Deauth 检测
 * @param repeat 重复次数
 */
void buzzer_alarm(uint8_t repeat);

/**
 * @brief 唤醒提示
 */
void buzzer_wake();

/**
 * @brief 非阻塞更新（用于连续报警）
 */
void buzzer_update();

#endif // BUZZER_H