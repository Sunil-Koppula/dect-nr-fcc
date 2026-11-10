/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    status_led.c
*  @brief   Test state on LD1 (green), LD2 (red) and LED2 (J1-73)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "status_led.h"

static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec ext = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static bool ready;

static void blink(struct k_timer *t)
{
	gpio_pin_toggle_dt(&red);
}

K_TIMER_DEFINE(blink_timer, blink, NULL);

int status_led_init(void)
{
	if (!gpio_is_ready_dt(&green) || !gpio_is_ready_dt(&red) || !gpio_is_ready_dt(&ext)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&ext, GPIO_OUTPUT_INACTIVE);
	ready = true;
	status_led_set(false, false);
	return 0;
}

void status_led_set(bool running, bool fault)
{
	if (!ready) {
		return;
	}
	k_timer_stop(&blink_timer);
	gpio_pin_set_dt(&green, running);
	gpio_pin_set_dt(&ext, running);
	gpio_pin_set_dt(&red, !running);
	if (fault) {
		k_timer_start(&blink_timer, K_MSEC(250), K_MSEC(250));
	}
}
