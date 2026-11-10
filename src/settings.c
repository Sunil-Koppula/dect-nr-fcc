/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    settings.c
*  @brief   RF test settings persisted in EEPROM (M24M02 block 2, offset 0)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <nrf_modem_dect_phy.h>
#include "settings.h"
#include "dect_bands.h"

LOG_MODULE_REGISTER(settings, CONFIG_FCC_LOG_LEVEL);

#define SETTINGS_OFFSET 0

static const struct device *const eeprom = DEVICE_DT_GET(DT_NODELABEL(fcc_eeprom));
static struct fcc_settings current;
static K_MUTEX_DEFINE(settings_lock);

#define LBT_SYMBOLS_MIN (NRF_MODEM_DECT_LBT_PERIOD_MIN / NRF_MODEM_DECT_SYMBOL_DURATION)
#define LBT_SYMBOLS_MAX (NRF_MODEM_DECT_LBT_PERIOD_MAX / NRF_MODEM_DECT_SYMBOL_DURATION)

static uint32_t record_crc(const struct fcc_settings *s)
{
	return crc32_ieee((const uint8_t *)s, offsetof(struct fcc_settings, crc));
}

void settings_defaults(struct fcc_settings *s)
{
	memset(s, 0, sizeof(*s));
	s->magic = SETTINGS_MAGIC;
	s->version = SETTINGS_VERSION;
	s->size = sizeof(*s);
	s->mode = TEST_MODE_IDLE;
	s->band = 4;
	s->carrier = 525;
	s->mcs = 2;
	s->tx_power_dbm = 19;
	s->subslots = 4;
	s->autostart = 0;
	s->lbt_enable = 0;
	s->lbt_threshold_dbm = -85;
	s->lbt_period_symbols = LBT_SYMBOLS_MIN;
	s->tx_gap_ms = 0;
	s->rx_report_s = 1;
	s->tx_count = 0;
	s->network_id = 0x12345678;
}

const char *settings_validate(const struct fcc_settings *s)
{
	if (s->mode >= TEST_MODE_COUNT) {
		return "mode";
	}
	if (!dect_band_is_known(s->band)) {
		return "band must be 1, 2, 4, 9 or 22";
	}
	if (!dect_carrier_valid(s->band, s->carrier)) {
		return "carrier not in band";
	}
	if (s->mcs > DECT_MCS_MAX) {
		return "mcs must be 0-4";
	}
	if (dect_power_code(s->tx_power_dbm) < 0) {
		return "tx power must be a table value";
	}
	/* Payload carries a 4-byte magic + sequence number. */
	if (dect_tb_bytes(s->mcs, s->subslots) < 4) {
		return "subslots not supported for this mcs";
	}
	if (s->lbt_period_symbols < LBT_SYMBOLS_MIN || s->lbt_period_symbols > LBT_SYMBOLS_MAX) {
		return "lbt period out of range";
	}
	if ((s->network_id & 0xff) == 0 || (s->network_id >> 8) == 0) {
		return "network id: both LSB 8 bits and MSB 24 bits must be non-zero";
	}
	return NULL;
}

int settings_load(void)
{
	struct fcc_settings s;
	int err;

	k_mutex_lock(&settings_lock, K_FOREVER);
	settings_defaults(&current);

	if (!device_is_ready(eeprom)) {
		LOG_ERR("EEPROM not ready");
		err = -ENODEV;
		goto out;
	}
	err = eeprom_read(eeprom, SETTINGS_OFFSET, &s, sizeof(s));
	if (err) {
		LOG_ERR("EEPROM read failed (%d)", err);
		goto out;
	}
	if (s.magic != SETTINGS_MAGIC || s.version != SETTINGS_VERSION || s.size != sizeof(s) ||
	    s.crc != record_crc(&s)) {
		LOG_WRN("No valid settings in EEPROM, using defaults");
		err = -ENOENT;
		goto out;
	}
	if (settings_validate(&s)) {
		LOG_WRN("Stored settings invalid (%s), using defaults", settings_validate(&s));
		err = -EINVAL;
		goto out;
	}
	current = s;
out:
	k_mutex_unlock(&settings_lock);
	return err;
}

static int save_locked(void)
{
	struct fcc_settings check;
	int err;

	if (!device_is_ready(eeprom)) {
		return -ENODEV;
	}
	current.crc = record_crc(&current);
	err = eeprom_write(eeprom, SETTINGS_OFFSET, &current, sizeof(current));
	if (err) {
		LOG_ERR("EEPROM write failed (%d)", err);
		return err;
	}
	err = eeprom_read(eeprom, SETTINGS_OFFSET, &check, sizeof(check));
	if (err) {
		return err;
	}
	if (memcmp(&check, &current, sizeof(check)) != 0) {
		LOG_ERR("EEPROM readback mismatch (write-protected?)");
		return -EIO;
	}
	return 0;
}

int settings_save(void)
{
	k_mutex_lock(&settings_lock, K_FOREVER);
	int err = save_locked();

	k_mutex_unlock(&settings_lock);
	return err;
}

const struct fcc_settings *settings_get(void)
{
	return &current;
}

int settings_apply(const struct fcc_settings *s, const char **why)
{
	const char *reason = settings_validate(s);

	if (reason) {
		*why = reason;
		return -EINVAL;
	}
	k_mutex_lock(&settings_lock, K_FOREVER);
	current = *s;
	current.magic = SETTINGS_MAGIC;
	current.version = SETTINGS_VERSION;
	current.size = sizeof(current);
	int err = save_locked();

	k_mutex_unlock(&settings_lock);
	return err;
}

const char *settings_mode_name(uint8_t mode)
{
	static const char *const names[] = { "IDLE", "TX", "RX", "CW" };

	return (mode < TEST_MODE_COUNT) ? names[mode] : "?";
}
