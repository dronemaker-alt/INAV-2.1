/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#include <platform.h>
#include "drivers/io.h"
#include "drivers/pwm_mapping.h"
#include "drivers/timer.h"
#include "drivers/bus.h"

const timerHardware_t timerHardware[] = {
    { TIM5, IO_TAG(PA3), TIM_Channel_4, 0, IOCFG_AF_PP_UP, GPIO_AF_TIM5, TIM_USE_PPM}, // PPM
  //MOTORS:
    { TIM3, IO_TAG(PB0), TIM_Channel_3, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM3, TIM_USE_MC_MOTOR }, 
    { TIM3, IO_TAG(PB1), TIM_Channel_4, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM3, TIM_USE_MC_MOTOR }, 
    { TIM4, IO_TAG(PB8), TIM_Channel_3, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM4, TIM_USE_MC_MOTOR }, 
    { TIM4, IO_TAG(PB9), TIM_Channel_4, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM4, TIM_USE_MC_MOTOR }, 
    { TIM4, IO_TAG(PB6), TIM_Channel_1, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM4, TIM_USE_MC_MOTOR }, 
    { TIM4, IO_TAG(PB7), TIM_Channel_2, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM4, TIM_USE_MC_MOTOR },
    { TIM8, IO_TAG(PC6), TIM_Channel_1, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM8, TIM_USE_MC_MOTOR },
    { TIM8, IO_TAG(PC7), TIM_Channel_2, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM8, TIM_USE_MC_MOTOR }, 
    { TIM9, IO_TAG(PA3), TIM_Channel_2, 1, IOCFG_AF_PP_UP, GPIO_AF_TIM9, TIM_USE_PPM},
};

const int timerHardwareCount = sizeof(timerHardware) / sizeof(timerHardware[0]);
