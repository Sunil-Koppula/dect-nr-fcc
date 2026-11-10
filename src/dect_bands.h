/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    dect_bands.h
*  @brief   DECT NR+ band, carrier, MCS and TX power tables
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#ifndef DECT_BANDS_H_
#define DECT_BANDS_H_

#include <stdbool.h>
#include <stdint.h>

#define DECT_MCS_MAX        4
#define DECT_SUBSLOTS_MAX   16
#define DECT_TB_BYTES_MAX   700
#define DECT_SUBSLOT_US_X100 20833   /* 10 ms frame / 48 subslots, in 0.01 us */

bool dect_band_is_known(uint8_t band);
/* Carrier range from the modem's band report when available, otherwise the built-in table. */
bool dect_band_carrier_range(uint8_t band, uint16_t *min, uint16_t *max);
bool dect_carrier_valid(uint8_t band, uint16_t carrier);
/* 0 = ~2 GHz bands, 1 = band 4 (~900 MHz). */
uint8_t dect_band_group_index(uint8_t band);
/* Nominal centre frequency in kHz (ETSI TS 103 636-2 5.4.2). */
uint32_t dect_carrier_khz(uint16_t carrier);

/* Transport block size in bytes; 0 if the MCS/length pair is not supported. */
uint16_t dect_tb_bytes(uint8_t mcs, uint8_t subslots);

/* PHY header power code (ETSI TS 103 636-4 table 6.2.1-3a); -1 if dBm is not in the table. */
int dect_power_code(int8_t dbm);
const char *dect_power_table_str(void);

/* Modem-reported band list (filled once the PHY band event arrives). */
struct dect_band_info {
	uint8_t band;
	uint8_t group;
	uint8_t power_class;
	int8_t rx_gain;
	uint16_t min_carrier;
	uint16_t max_carrier;
};
void dect_bands_set_modem_list(const struct dect_band_info *list, int count);
int dect_bands_get_modem_list(const struct dect_band_info **list);

#endif /* DECT_BANDS_H_ */
