/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    main.c
*  @brief   DECT NR+ RF certification test firmware entry point
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "at_cmd.h"
#include "rf_test.h"
#include "settings.h"
#include "status_led.h"

LOG_MODULE_REGISTER(main, CONFIG_FCC_LOG_LEVEL);

int main(void)
{
	(void)status_led_init();
	(void)at_init();

	int settings_err = settings_load();
	int phy_err = rf_test_init();

	at_printf("\r\n+READY: IRISS DECT NR+ FCC test firmware v%s\r\n", CONFIG_FCC_FIRMWARE_VER);
	if (settings_err == -ENOENT) {
		at_printf("+EEPROM: no saved settings, using defaults\r\n");
	} else if (settings_err) {
		at_printf("+EEPROM: settings not loaded (%d), using defaults\r\n", settings_err);
	} else {
		at_printf("+EEPROM: settings loaded\r\n");
	}
	if (phy_err) {
		at_printf("+ERROR: DECT PHY init failed (%d / 0x%x)\r\n", phy_err, phy_err);
		status_led_set(false, true);
	}
	at_print_config();

	const struct fcc_settings *s = settings_get();

	if (!phy_err && s->autostart && s->mode != TEST_MODE_IDLE) {
		int err = rf_test_start();

		if (err) {
			at_printf("+TEST: autostart failed (%d)\r\n", err);
		} else {
			at_printf("+TEST: STARTED %s (autostart)\r\n", settings_mode_name(s->mode));
		}
	}

	at_run();
	return 0;
}
