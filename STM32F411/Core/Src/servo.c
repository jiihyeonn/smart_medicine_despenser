#include "servo.h"

extern TIM_HandleTypeDef htim2;

void servo_set_angle(uint32_t channel, uint8_t angle) {
    // 0~180도 → 500~2500µs 변환
    uint16_t pulse = 500 + (angle * 2000 / 180);
    __HAL_TIM_SET_COMPARE(&htim2, channel, pulse);
}

void dispense_one(uint32_t channel) {
    servo_set_angle(channel, 91);   // 회전
    HAL_Delay(300);
    servo_set_angle(channel, 0);    // 복귀
    HAL_Delay(300);
}

void dispense_n(uint32_t channel, uint8_t count) {
    for(int i = 0; i < count; i++) {
        dispense_one(channel);
        HAL_Delay(200);
    }
}

