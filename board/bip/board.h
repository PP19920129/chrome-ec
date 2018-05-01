/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Bip board configuration */

#ifndef __CROS_EC_BOARD_H
#define __CROS_EC_BOARD_H

/* Select Baseboard features */
#define OCTOPUS_EC_ITE8320
#define OCTOPUS_CHARGER_BQ25703
#include "baseboard.h"

/* Optional features */
#define CONFIG_SYSTEM_UNLOCKED /* Allow dangerous commands while in dev. */

#ifndef __ASSEMBLER__

#include "gpio_signal.h"
#include "registers.h"

enum adc_channel {
	ADC_VBUS_C0,
	ADC_VBUS_C1,
	ADC_CH_COUNT
};

/* TODO(b/75972988): Fill out correctly */
enum pwm_channel {
	PWM_CH_COUNT
};

/* List of possible batteries */
enum battery_type {
	BATTERY_PANASONIC,
	BATTERY_TYPE_COUNT,
};

#endif /* !__ASSEMBLER__ */

#endif /* __CROS_EC_BOARD_H */
