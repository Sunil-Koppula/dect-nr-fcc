/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    settings.h
*  @brief   RF test settings persisted in EEPROM
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdint.h>

enum test_mode {
	TEST_MODE_IDLE = 0,
	TEST_MODE_TX = 1,     /* modulated DECT NR+ packets */
	TEST_MODE_RX = 2,     /* continuous receive + packet statistics */
	TEST_MODE_CW = 3,     /* unmodulated carrier (certification API) */
	TEST_MODE_COUNT
};

/* EEPROM record layout; append fields only and bump SETTINGS_VERSION. */
struct fcc_settings {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t mode;
	uint8_t band;
	uint16_t carrier;
	uint8_t mcs;
	int8_t tx_power_dbm;
	uint8_t subslots;
	uint8_t autostart;
	uint8_t lbt_enable;
	int8_t lbt_threshold_dbm;
	uint8_t lbt_period_symbols;
	uint8_t reserved0;
	uint16_t tx_gap_ms;
	uint16_t rx_report_s;
	uint32_t tx_count;
	uint32_t network_id;
	uint32_t crc;
} __packed;

#define SETTINGS_MAGIC   0x54434346u   /* "FCCT" */
#define SETTINGS_VERSION 1

void settings_defaults(struct fcc_settings *s);
/* Returns NULL if valid, otherwise a short reason. */
const char *settings_validate(const struct fcc_settings *s);

/* Loads from EEPROM; falls back to defaults (and returns <0) if missing or corrupt. */
int settings_load(void);
/* Writes the current settings and reads them back. */
int settings_save(void);

const struct fcc_settings *settings_get(void);
/* Validates, applies in RAM, then saves. Returns -EINVAL on validation failure
 * (reason in *why), or the EEPROM error (settings still applied in RAM). */
int settings_apply(const struct fcc_settings *s, const char **why);

const char *settings_mode_name(uint8_t mode);

#endif /* SETTINGS_H_ */
