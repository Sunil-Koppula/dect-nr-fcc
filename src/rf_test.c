/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    rf_test.c
*  @brief   DECT NR+ PHY control for RF certification tests (TX, RX, CW)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_dect_phy.h>
#include "rf_test.h"
#include "settings.h"
#include "dect_bands.h"
#include "at_cmd.h"
#include "status_led.h"

LOG_MODULE_REGISTER(rf_test, CONFIG_FCC_LOG_LEVEL);

#define HANDLE_TX          1
#define HANDLE_RX          2
#define RX_CHUNK_MS        5000
#define CTRL_TIMEOUT       K_SECONDS(5)
#define STOP_TIMEOUT       K_SECONDS(3)
#define RETRY_DELAY_MS     10
#define MAX_CONSEC_FAILS   10
#define PAYLOAD_MAGIC0     0xFC
#define PAYLOAD_MAGIC1     0xC7
#define TX_DEVICE_ID       0x0F0C

enum ev_type { EV_DONE, EV_PCC, EV_PCC_ERR, EV_PDC, EV_PDC_ERR };

struct rf_ev {
	uint8_t type;
	uint8_t seq_valid;
	uint16_t seq;
	uint32_t handle;
	int32_t err;
	int16_t rssi2;
	int16_t snr;
};

K_MSGQ_DEFINE(ev_q, sizeof(struct rf_ev), 64, 4);
static K_MUTEX_DEFINE(lock);
static K_SEM_DEFINE(sem_ctrl, 0, 1);
static K_SEM_DEFINE(sem_stopped, 0, 1);

static volatile int ctrl_err;
static volatile int ctrl_expect = -1;
static volatile int16_t modem_temp = NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED;
static volatile uint16_t modem_mv;
static volatile uint32_t ev_dropped;

/* Everything below is guarded by `lock`. */
static uint8_t state = TEST_MODE_IDLE;
static struct fcc_settings cfg;
static bool phy_inited;
static bool phy_active;
static uint8_t active_group;
static bool in_flight;
static uint32_t in_flight_handle;
static bool op_due;
static int64_t op_due_at;
static int64_t next_report_at;
static uint16_t tx_seq;
static uint8_t consec_fails;
static struct rf_tx_stats tx;
static struct rf_rx_stats rx;
static bool rx_have_last;
static uint16_t rx_last_seq;

static uint8_t tx_buf[DECT_TB_BYTES_MAX];
static union nrf_modem_dect_phy_hdr tx_hdr;

/* ---- modem callback (interrupt context: copy and hand off only) ---- */

static void ctrl_done(int id, int err)
{
	if (id == ctrl_expect) {
		ctrl_err = err;
		ctrl_expect = -1;
		k_sem_give(&sem_ctrl);
	}
}

static void post(const struct rf_ev *ev)
{
	if (k_msgq_put(&ev_q, ev, K_NO_WAIT)) {
		ev_dropped++;
	}
}

static void on_bands(const struct nrf_modem_dect_phy_band_get_event *evt)
{
	struct dect_band_info list[8];
	int n = MIN((int)evt->band_count, (int)ARRAY_SIZE(list));

	for (int i = 0; i < n; i++) {
		list[i] = (struct dect_band_info){
			.band = evt->band[i].band_number,
			.group = evt->band[i].band_group_index,
			.power_class = evt->band[i].power_class,
			.rx_gain = evt->band[i].rx_gain,
			.min_carrier = evt->band[i].min_carrier,
			.max_carrier = evt->band[i].max_carrier,
		};
	}
	if (evt->err == NRF_MODEM_DECT_PHY_SUCCESS) {
		dect_bands_set_modem_list(list, n);
	}
}

static void phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
	struct rf_ev ev = { 0 };

	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		modem_temp = evt->init.temp;
		modem_mv = evt->init.voltage;
		ctrl_done(evt->id, evt->init.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEINIT:
		ctrl_done(evt->id, evt->deinit.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		ctrl_done(evt->id, evt->configure.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		modem_temp = evt->activate.temp;
		modem_mv = evt->activate.voltage;
		ctrl_done(evt->id, evt->activate.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
		ctrl_done(evt->id, evt->deactivate.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_BANDS:
		on_bands(&evt->band_get);
		ctrl_done(evt->id, evt->band_get.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG:
		ctrl_done(evt->id, evt->test_rf_tx_cw_control.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		if (evt->op_complete.temp != NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED) {
			modem_temp = evt->op_complete.temp;
		}
		if (evt->op_complete.voltage != NRF_MODEM_DECT_PHY_VOLTAGE_NOT_MEASURED) {
			modem_mv = evt->op_complete.voltage;
		}
		ev.type = EV_DONE;
		ev.handle = evt->op_complete.handle;
		ev.err = evt->op_complete.err;
		post(&ev);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
		ev.type = (evt->pcc.header_status == NRF_MODEM_DECT_PHY_HDR_STATUS_INVALID) ?
			  EV_PCC_ERR : EV_PCC;
		ev.rssi2 = evt->pcc.rssi_2;
		ev.snr = evt->pcc.snr;
		post(&ev);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
		ev.type = EV_PCC_ERR;
		post(&ev);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC: {
		const uint8_t *d = evt->pdc.data;

		ev.type = EV_PDC;
		ev.rssi2 = evt->pdc.rssi_2;
		ev.snr = evt->pdc.snr;
		if (d && evt->pdc.len >= 4 && d[0] == PAYLOAD_MAGIC0 && d[1] == PAYLOAD_MAGIC1) {
			ev.seq_valid = 1;
			ev.seq = sys_get_le16(&d[2]);
		}
		post(&ev);
		break;
	}
	case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
		ev.type = EV_PDC_ERR;
		post(&ev);
		break;
	default:
		break;
	}
}

/* ---- synchronous PHY control (caller holds `lock`) ---- */

static int ctrl_wait(int id, int call_ret)
{
	if (call_ret) {
		ctrl_expect = -1;
		LOG_ERR("PHY call for event %d failed: %d", id, call_ret);
		return call_ret;
	}
	if (k_sem_take(&sem_ctrl, CTRL_TIMEOUT)) {
		ctrl_expect = -1;
		LOG_ERR("PHY event %d timeout", id);
		return -ETIMEDOUT;
	}
	if (ctrl_err) {
		LOG_ERR("PHY event %d error 0x%x", id, ctrl_err);
	}
	return ctrl_err;
}

#define CTRL(id, call) ({ k_sem_reset(&sem_ctrl); ctrl_expect = (id); ctrl_wait((id), (call)); })

static int phy_configure_activate(uint8_t group)
{
	struct nrf_modem_dect_phy_config_params p = {
		.band_group_index = group,
		.harq_rx_process_count = 4,
		.harq_rx_expiry_time_us = 5000000,
	};
	int err = CTRL(NRF_MODEM_DECT_PHY_EVT_CONFIGURE, nrf_modem_dect_phy_configure(&p));

	if (err) {
		return err;
	}
	err = CTRL(NRF_MODEM_DECT_PHY_EVT_ACTIVATE,
		   nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY));
	if (err) {
		return err;
	}
	phy_active = true;
	active_group = group;
	return 0;
}

static int phy_up(uint8_t group)
{
	if (!phy_inited) {
		int err = CTRL(NRF_MODEM_DECT_PHY_EVT_INIT, nrf_modem_dect_phy_init());

		if (err) {
			return err;
		}
		phy_inited = true;
	}
	if (phy_active && active_group == group) {
		return 0;
	}
	if (phy_active) {
		int err = CTRL(NRF_MODEM_DECT_PHY_EVT_DEACTIVATE, nrf_modem_dect_phy_deactivate());

		if (err) {
			return err;
		}
		phy_active = false;
	}
	return phy_configure_activate(group);
}

static int phy_down(void)
{
	int err;

	if (phy_active) {
		err = CTRL(NRF_MODEM_DECT_PHY_EVT_DEACTIVATE, nrf_modem_dect_phy_deactivate());
		if (err) {
			return err;
		}
		phy_active = false;
	}
	if (phy_inited) {
		err = CTRL(NRF_MODEM_DECT_PHY_EVT_DEINIT, nrf_modem_dect_phy_deinit());
		if (err) {
			return err;
		}
		phy_inited = false;
	}
	return 0;
}

/* ---- TX / RX scheduling (caller holds `lock`) ---- */

static void fill_payload(void)
{
	uint16_t lfsr = 0x1FF;   /* PN9 filler so the spectrum looks like real traffic */

	for (size_t i = 4; i < sizeof(tx_buf); i++) {
		uint8_t byte = 0;

		for (int b = 0; b < 8; b++) {
			uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 4)) & 1;

			byte = (byte << 1) | (lfsr & 1);
			lfsr = (lfsr >> 1) | (bit << 8);
		}
		tx_buf[i] = byte;
	}
	tx_buf[0] = PAYLOAD_MAGIC0;
	tx_buf[1] = PAYLOAD_MAGIC1;
}

static void stop_on_failures(const char *what, int err)
{
	consec_fails++;
	if (consec_fails <= 3 || (consec_fails % 100) == 0) {
		at_printf("+%sERR: 0x%x\r\n", what, err);
	}
	if (consec_fails >= MAX_CONSEC_FAILS) {
		state = TEST_MODE_IDLE;
		op_due = false;
		at_printf("+TEST: STOPPED after %u consecutive %s errors (last 0x%x)\r\n",
			  consec_fails, what, err);
		status_led_set(false, true);
	} else {
		op_due = true;
		op_due_at = k_uptime_get() + RETRY_DELAY_MS;
	}
}

static void tx_schedule(void)
{
	uint16_t len = dect_tb_bytes(cfg.mcs, cfg.subslots);

	sys_put_le16(tx_seq, &tx_buf[2]);
	tx_hdr.hdr_type_1 = (struct nrf_modem_dect_phy_hdr_type_1){
		.packet_length = cfg.subslots - 1,
		.packet_length_type = 0,
		.header_format = 0,
		.short_network_id = cfg.network_id & 0xff,
		.transmitter_id_hi = TX_DEVICE_ID >> 8,
		.transmitter_id_lo = TX_DEVICE_ID & 0xff,
		.df_mcs = cfg.mcs,
		.reserved = 0,
		.transmit_power = dect_power_code(cfg.tx_power_dbm),
	};
	struct nrf_modem_dect_phy_tx_params p = {
		.start_time = 0,
		.handle = HANDLE_TX,
		.network_id = cfg.network_id,
		.phy_type = 0,
		.lbt_rssi_threshold_max = cfg.lbt_threshold_dbm,
		.carrier = cfg.carrier,
		.lbt_period = cfg.lbt_enable ?
			      cfg.lbt_period_symbols * NRF_MODEM_DECT_SYMBOL_DURATION : 0,
		.phy_header = &tx_hdr,
		.bs_cqi = NRF_MODEM_DECT_PHY_BS_CQI_NOT_USED,
		.data = tx_buf,
		.data_size = len,
	};
	int err = nrf_modem_dect_phy_tx(&p);

	if (err) {
		tx.failed++;
		tx.last_err = err;
		stop_on_failures("TX", err);
		return;
	}
	in_flight = true;
	in_flight_handle = HANDLE_TX;
	tx.sent++;
	tx_seq++;
}

static void rx_schedule(void)
{
	struct nrf_modem_dect_phy_rx_params p = {
		.start_time = 0,
		.handle = HANDLE_RX,
		.network_id = cfg.network_id,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = 0,   /* fast AGC: unknown level in the lab */
		.carrier = cfg.carrier,
		.duration = RX_CHUNK_MS * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = cfg.network_id & 0xff,
		.filter.is_short_network_id_used = 1,
		.filter.receiver_identity = 0,
	};
	int err = nrf_modem_dect_phy_rx(&p);

	if (err) {
		stop_on_failures("RX", err);
		return;
	}
	in_flight = true;
	in_flight_handle = HANDLE_RX;
}

static void handle_done(const struct rf_ev *ev)
{
	bool is_tx = (ev->handle == HANDLE_TX);

	in_flight = false;
	if (is_tx) {
		if (ev->err == NRF_MODEM_DECT_PHY_SUCCESS) {
			tx.ok++;
		} else if (ev->err == NRF_MODEM_DECT_PHY_ERR_LBT_CHANNEL_BUSY) {
			tx.lbt_busy++;
		} else if (ev->err != NRF_MODEM_DECT_PHY_ERR_OP_CANCELED) {
			tx.failed++;
			tx.last_err = ev->err;
		}
	}

	if (state != (is_tx ? TEST_MODE_TX : TEST_MODE_RX)) {
		k_sem_give(&sem_stopped);
		return;
	}

	bool ok = ev->err == NRF_MODEM_DECT_PHY_SUCCESS ||
		  ev->err == NRF_MODEM_DECT_PHY_ERR_LBT_CHANNEL_BUSY;

	if (!ok) {
		stop_on_failures(is_tx ? "TX" : "RX", ev->err);
		return;
	}
	consec_fails = 0;

	if (!is_tx) {
		rx_schedule();
		return;
	}
	if (cfg.tx_count && tx.sent >= cfg.tx_count) {
		state = TEST_MODE_IDLE;
		at_printf("+TXDONE: sent=%u ok=%u lbt_busy=%u failed=%u\r\n",
			  tx.sent, tx.ok, tx.lbt_busy, tx.failed);
		status_led_set(false, false);
		return;
	}
	if (cfg.tx_gap_ms == 0) {
		tx_schedule();
	} else {
		op_due = true;
		op_due_at = k_uptime_get() + cfg.tx_gap_ms;
	}
}

static void handle_rx_ev(const struct rf_ev *ev)
{
	switch (ev->type) {
	case EV_PCC:
		rx.pcc_ok++;
		break;
	case EV_PCC_ERR:
		rx.pcc_crc_err++;
		break;
	case EV_PDC_ERR:
		rx.pdc_crc_err++;
		break;
	case EV_PDC:
		rx.pdc_ok++;
		if (ev->rssi2 != NRF_MODEM_DECT_PHY_RSSI2_NOT_MEASURED) {
			if (rx.rssi_count == 0 || ev->rssi2 < rx.rssi_min_x2) {
				rx.rssi_min_x2 = ev->rssi2;
			}
			if (rx.rssi_count == 0 || ev->rssi2 > rx.rssi_max_x2) {
				rx.rssi_max_x2 = ev->rssi2;
			}
			rx.rssi_sum_x2 += ev->rssi2;
			rx.rssi_count++;
		}
		if (ev->snr != NRF_MODEM_DECT_PHY_SNR_NOT_MEASURED) {
			rx.snr_sum_x4 += ev->snr;
			rx.snr_count++;
		}
		if (ev->seq_valid) {
			rx.valid++;
			if (rx_have_last) {
				uint16_t diff = ev->seq - rx_last_seq;

				/* Large jumps mean the transmitter restarted; don't count them. */
				if (diff > 1 && diff < 0x8000) {
					rx.lost += diff - 1;
				}
			}
			rx_last_seq = ev->seq;
			rx_have_last = true;
		}
		break;
	default:
		break;
	}
}

static void runner(void)
{
	for (;;) {
		struct rf_ev ev;
		int64_t now = k_uptime_get();
		int64_t wait = 200;

		k_mutex_lock(&lock, K_FOREVER);
		if (op_due) {
			wait = MIN(wait, MAX(0, op_due_at - now));
		}
		if (state == TEST_MODE_RX && cfg.rx_report_s) {
			wait = MIN(wait, MAX(0, next_report_at - now));
		}
		k_mutex_unlock(&lock);

		bool got = (k_msgq_get(&ev_q, &ev, K_MSEC(wait)) == 0);

		k_mutex_lock(&lock, K_FOREVER);
		if (got) {
			if (ev.type == EV_DONE) {
				handle_done(&ev);
			} else {
				handle_rx_ev(&ev);
			}
		}
		now = k_uptime_get();
		if (op_due && !in_flight && now >= op_due_at) {
			op_due = false;
			if (state == TEST_MODE_TX) {
				tx_schedule();
			} else if (state == TEST_MODE_RX) {
				rx_schedule();
			}
		}
		bool report = state == TEST_MODE_RX && cfg.rx_report_s && now >= next_report_at;

		if (report) {
			next_report_at = now + cfg.rx_report_s * 1000;
		}
		k_mutex_unlock(&lock);

		if (report) {
			rf_test_print_rx_stats("+RXSTATS");
		}
	}
}

K_THREAD_DEFINE(rf_runner, 2048, runner, NULL, NULL, NULL, 5, 0, 0);

/* ---- public API ---- */

int rf_test_init(void)
{
	int err = nrf_modem_lib_init();

	if (err) {
		LOG_ERR("nrf_modem_lib_init failed: %d (is DECT NR+ PHY modem firmware flashed?)",
			err);
		return err;
	}
	err = nrf_modem_dect_phy_event_handler_set(phy_event_handler);
	if (err) {
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	err = phy_up(dect_band_group_index(settings_get()->band));
	if (!err && CTRL(NRF_MODEM_DECT_PHY_EVT_BANDS, nrf_modem_dect_phy_band_get())) {
		LOG_WRN("Band list not available; using built-in carrier ranges");
	}
	k_mutex_unlock(&lock);
	fill_payload();
	return err;
}

int rf_test_start(void)
{
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (state != TEST_MODE_IDLE) {
		err = -EBUSY;
		goto out;
	}
	cfg = *settings_get();
	op_due = false;
	consec_fails = 0;
	uint8_t group = dect_band_group_index(cfg.band);

	switch (cfg.mode) {
	case TEST_MODE_TX:
	case TEST_MODE_RX:
		err = phy_up(group);
		if (err) {
			goto out;
		}
		state = cfg.mode;
		if (cfg.mode == TEST_MODE_TX) {
			memset(&tx, 0, sizeof(tx));
			tx_seq = 0;
			tx_schedule();
		} else {
			memset(&rx, 0, sizeof(rx));
			rx_have_last = false;
			next_report_at = k_uptime_get() + cfg.rx_report_s * 1000;
			rx_schedule();
		}
		break;
	case TEST_MODE_CW:
		err = phy_down();
		if (err) {
			goto out;
		}
		err = CTRL(NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG,
			   nrf_modem_dect_phy_test_rf_tx_cw_control(
				   NRF_MODEM_DECT_PHY_TEST_RF_TX_CW_OPER_ON, cfg.carrier,
				   cfg.tx_power_dbm));
		if (err) {
			(void)phy_up(group);
			goto out;
		}
		state = TEST_MODE_CW;
		break;
	default:
		err = -EINVAL;
		goto out;
	}
	status_led_set(state != TEST_MODE_IDLE, false);
out:
	k_mutex_unlock(&lock);
	return err;
}

int rf_test_stop(void)
{
	int err = 0;
	bool wait = false;

	k_mutex_lock(&lock, K_FOREVER);
	switch (state) {
	case TEST_MODE_CW:
		err = CTRL(NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG,
			   nrf_modem_dect_phy_test_rf_tx_cw_control(
				   NRF_MODEM_DECT_PHY_TEST_RF_TX_CW_OPER_OFF, cfg.carrier,
				   cfg.tx_power_dbm));
		if (err) {
			break;
		}
		state = TEST_MODE_IDLE;
		err = phy_up(dect_band_group_index(cfg.band));
		break;
	case TEST_MODE_TX:
	case TEST_MODE_RX:
		state = TEST_MODE_IDLE;
		op_due = false;
		if (in_flight) {
			wait = true;
			k_sem_reset(&sem_stopped);
			(void)nrf_modem_dect_phy_cancel(in_flight_handle);
		}
		break;
	default:
		break;
	}
	k_mutex_unlock(&lock);

	if (wait && k_sem_take(&sem_stopped, STOP_TIMEOUT)) {
		LOG_WRN("Timed out waiting for the PHY operation to stop");
	}
	status_led_set(false, err != 0);
	return err;
}

bool rf_test_running(void)
{
	return state != TEST_MODE_IDLE;
}

uint8_t rf_test_active_mode(void)
{
	return state;
}

void rf_test_tx_stats(struct rf_tx_stats *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = tx;
	k_mutex_unlock(&lock);
}

void rf_test_rx_stats(struct rf_rx_stats *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = rx;
	k_mutex_unlock(&lock);
}

void rf_test_rx_stats_clear(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	memset(&rx, 0, sizeof(rx));
	rx_have_last = false;
	k_mutex_unlock(&lock);
}

/* Formats a value in tenths as "-12.3". */
static void fmt_tenths(char *buf, size_t len, int32_t v)
{
	snprintf(buf, len, "%s%d.%d", v < 0 ? "-" : "", abs(v) / 10, abs(v) % 10);
}

void rf_test_print_rx_stats(const char *prefix)
{
	struct rf_rx_stats s;
	char rssi[16] = "n/a", rmin[16] = "n/a", rmax[16] = "n/a", snr[16] = "n/a";
	char per[16] = "n/a";

	rf_test_rx_stats(&s);
	if (s.rssi_count) {
		fmt_tenths(rssi, sizeof(rssi), (s.rssi_sum_x2 * 5) / (int32_t)s.rssi_count);
		fmt_tenths(rmin, sizeof(rmin), s.rssi_min_x2 * 5);
		fmt_tenths(rmax, sizeof(rmax), s.rssi_max_x2 * 5);
	}
	if (s.snr_count) {
		fmt_tenths(snr, sizeof(snr), (s.snr_sum_x4 * 10 / 4) / (int32_t)s.snr_count);
	}
	if (s.valid + s.lost) {
		uint64_t per_x1000 = (uint64_t)s.lost * 1000 / (s.valid + s.lost);

		snprintf(per, sizeof(per), "%u.%u%%", (unsigned)(per_x1000 / 10),
			 (unsigned)(per_x1000 % 10));
	}
	at_printf("%s: rx=%u lost=%u per=%s pcc_ok=%u pcc_err=%u pdc_ok=%u pdc_err=%u "
		  "rssi=%s(min %s max %s)dBm snr=%sdB\r\n",
		  prefix, s.valid, s.lost, per, s.pcc_ok, s.pcc_crc_err, s.pdc_ok,
		  s.pdc_crc_err, rssi, rmin, rmax, snr);
	if (ev_dropped) {
		at_printf("+WARN: %u PHY events dropped (queue full)\r\n", ev_dropped);
	}
}

void rf_test_modem_health(int16_t *temp, uint16_t *mv)
{
	*temp = modem_temp;
	*mv = modem_mv;
}
