/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    rf_test.h
*  @brief   DECT NR+ PHY control for RF certification tests (TX, RX, CW)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#ifndef RF_TEST_H_
#define RF_TEST_H_

#include <stdbool.h>
#include <stdint.h>

struct rf_tx_stats {
	uint32_t sent;
	uint32_t ok;
	uint32_t lbt_busy;
	uint32_t failed;
	int32_t last_err;
};

struct rf_rx_stats {
	uint32_t pcc_ok;
	uint32_t pcc_crc_err;
	uint32_t pdc_ok;
	uint32_t pdc_crc_err;
	uint32_t valid;        /* PDC with the test magic */
	uint32_t lost;         /* sequence-number gaps */
	int32_t rssi_sum_x2;   /* RSSI-2, 0.5 dBm units */
	int16_t rssi_min_x2;
	int16_t rssi_max_x2;
	int32_t snr_sum_x4;    /* SNR, 0.25 dB units */
	uint32_t snr_count;
	uint32_t rssi_count;
};

/* Brings the PHY up (init, configure, activate) and reads the modem band list. */
int rf_test_init(void);

/* Starts the mode stored in settings. */
int rf_test_start(void);
int rf_test_stop(void);
bool rf_test_running(void);
uint8_t rf_test_active_mode(void);

void rf_test_tx_stats(struct rf_tx_stats *out);
void rf_test_rx_stats(struct rf_rx_stats *out);
void rf_test_rx_stats_clear(void);
void rf_test_print_rx_stats(const char *prefix);

/* Last temperature (C) and voltage (mV) reported by the modem. */
void rf_test_modem_health(int16_t *temp, uint16_t *mv);

#endif /* RF_TEST_H_ */
