#ifndef SERVO_H
#define SERVO_H

#include "stm32f4xx_hal.h"

void servo_set_angle(uint32_t channel, uint8_t angle);
void dispense_one(uint32_t channel);
void dispense_n(uint32_t channel, uint8_t count);

#endif
