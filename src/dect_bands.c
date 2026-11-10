/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    dect_bands.c
*  @brief   DECT NR+ band, carrier, MCS and TX power tables
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#include <string.h>
#include <zephyr/kernel.h>
#include "dect_bands.h"

/* Same ranges as the NCS dect_shell sample (dect_common.h). */
static const struct {
	uint8_t band;
	uint16_t min;
	uint16_t max;
} band_table[] = {
	{ 1, 1657, 1677 },
	{ 2, 1680, 1700 },
	{ 4, 525, 551 },
	{ 9, 1703, 1711 },
	{ 22, 1691, 1711 },
};

/* Bits per transmission, index = subslots - 1 (nrfxlib dectphy.rst). */
static const uint16_t tbs_bits[DECT_MCS_MAX + 1][DECT_SUBSLOTS_MAX] = {
	{ 0, 136, 264, 400, 536, 664, 792, 920, 1064, 1192, 1320, 1448, 1576, 1704, 1864, 1992 },
	{ 32, 296, 552, 824, 1096, 1352, 1608, 1864, 2104, 2360, 2616, 2872, 3128, 3384, 3704,
	  3960 },
	{ 56, 456, 856, 1256, 1640, 2024, 2360, 2744, 3192, 3576, 3960, 4320, 4768, 5152, 5536,
	  0 },
	{ 88, 616, 1128, 1672, 2168, 2680, 3192, 3704, 4256, 4768, 5280, 0, 0, 0, 0, 0 },
	{ 144, 936, 1736, 2488, 3256, 4024, 4832, 5600, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const int8_t power_dbm[] = { -40, -30, -20, -16, -12, -8, -4, 0,
				    4,   7,   10,  13,  16,  19,  21, 23 };

static struct dect_band_info modem_bands[8];
static int modem_band_count;

bool dect_band_is_known(uint8_t band)
{
	for (size_t i = 0; i < ARRAY_SIZE(band_table); i++) {
		if (band_table[i].band == band) {
			return true;
		}
	}
	return false;
}

bool dect_band_carrier_range(uint8_t band, uint16_t *min, uint16_t *max)
{
	for (int i = 0; i < modem_band_count; i++) {
		if (modem_bands[i].band == band) {
			*min = modem_bands[i].min_carrier;
			*max = modem_bands[i].max_carrier;
			/* Nordic: never use band 4 outside 525-551 (LTE interference). */
			if (band == 4) {
				*min = MAX(*min, 525);
				*max = MIN(*max, 551);
			}
			return true;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(band_table); i++) {
		if (band_table[i].band == band) {
			*min = band_table[i].min;
			*max = band_table[i].max;
			return true;
		}
	}
	return false;
}

bool dect_carrier_valid(uint8_t band, uint16_t carrier)
{
	uint16_t min, max;

	return dect_band_carrier_range(band, &min, &max) && carrier >= min && carrier <= max;
}

uint8_t dect_band_group_index(uint8_t band)
{
	return (band == 4) ? 1 : 0;
}

uint32_t dect_carrier_khz(uint16_t carrier)
{
	return 450144U + (uint32_t)carrier * 864U;
}

uint16_t dect_tb_bytes(uint8_t mcs, uint8_t subslots)
{
	if (mcs > DECT_MCS_MAX || subslots == 0 || subslots > DECT_SUBSLOTS_MAX) {
		return 0;
	}
	return tbs_bits[mcs][subslots - 1] / 8;
}

int dect_power_code(int8_t dbm)
{
	for (size_t i = 0; i < ARRAY_SIZE(power_dbm); i++) {
		if (power_dbm[i] == dbm) {
			return (int)i;
		}
	}
	return -1;
}

const char *dect_power_table_str(void)
{
	return "-40,-30,-20,-16,-12,-8,-4,0,4,7,10,13,16,19,21,23";
}

void dect_bands_set_modem_list(const struct dect_band_info *list, int count)
{
	count = MIN(count, (int)ARRAY_SIZE(modem_bands));
	memcpy(modem_bands, list, count * sizeof(*list));
	modem_band_count = count;
}

int dect_bands_get_modem_list(const struct dect_band_info **list)
{
	*list = modem_bands;
	return modem_band_count;
}
