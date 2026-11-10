/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    at_cmd.c
*  @brief   AT command interface on UART1 (J1 TXD/RXD)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include "at_cmd.h"
#include "settings.h"
#include "rf_test.h"
#include "dect_bands.h"

LOG_MODULE_REGISTER(at_cmd, CONFIG_FCC_LOG_LEVEL);

#define LINE_MAX 96

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(fcc_at_uart));
K_MSGQ_DEFINE(line_q, LINE_MAX, 4, 1);
static K_MUTEX_DEFINE(out_lock);
static bool uart_ready;

static char rx_line[LINE_MAX];
static size_t rx_len;
static bool rx_overflow;

enum at_type { AT_EXEC, AT_QUERY, AT_SET, AT_TEST };

/* ---- UART ---- */

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t c;

	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &c, 1) == 1) {
		if (c == '\r' || c == '\n') {
			if (rx_len && !rx_overflow) {
				rx_line[rx_len] = '\0';
				(void)k_msgq_put(&line_q, rx_line, K_NO_WAIT);
			}
			rx_len = 0;
			rx_overflow = false;
		} else if (c == 0x08 || c == 0x7f) {
			if (rx_len) {
				rx_len--;
			}
		} else if (rx_len < LINE_MAX - 1) {
			rx_line[rx_len++] = c;
		} else {
			rx_overflow = true;
		}
	}
}

int at_init(void)
{
	if (!device_is_ready(uart)) {
		LOG_ERR("AT UART not ready");
		return -ENODEV;
	}
	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);
	uart_ready = true;
	return 0;
}

void at_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (!uart_ready) {
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	n = MIN(n, (int)sizeof(buf) - 1);

	k_mutex_lock(&out_lock, K_FOREVER);
	for (int i = 0; i < n; i++) {
		uart_poll_out(uart, buf[i]);
	}
	k_mutex_unlock(&out_lock);
}

/* ---- helpers ---- */

static bool parse_long(const char *s, long min, long max, long *out)
{
	char *end;

	if (!s || !*s) {
		return false;
	}
	errno = 0;
	long v = strtol(s, &end, 0);

	while (*end == ' ') {
		end++;
	}
	if (errno || *end != '\0' || v < min || v > max) {
		return false;
	}
	*out = v;
	return true;
}

static int apply(struct fcc_settings *s)
{
	const char *why = NULL;

	if (rf_test_running()) {
		at_printf("+ERROR: test running, send AT#STOP first\r\n");
		return -EBUSY;
	}
	int err = settings_apply(s, &why);

	if (err == -EINVAL) {
		at_printf("+ERROR: %s\r\n", why);
	} else if (err) {
		at_printf("+ERROR: EEPROM save failed (%d); value is active until reboot\r\n", err);
	}
	return err;
}

static void fmt_mhz(char *buf, size_t len, uint16_t carrier)
{
	uint32_t khz = dect_carrier_khz(carrier);

	snprintf(buf, len, "%u.%03u MHz", khz / 1000, khz % 1000);
}

static int bad_arg(const char *usage)
{
	at_printf("+ERROR: usage %s\r\n", usage);
	return -EINVAL;
}

/* ---- command handlers ---- */

static int cmd_at(enum at_type t, char *arg)
{
	return 0;
}

static int cmd_ati(enum at_type t, char *arg)
{
	at_printf("IRISS DECT NR+ FCC test firmware v%s\r\n", CONFIG_FCC_FIRMWARE_VER);
	return 0;
}

static int cmd_mode(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();

	if (t == AT_QUERY) {
		at_printf("+MODE: %s\r\n", settings_mode_name(s.mode));
		return 0;
	}
	if (t != AT_SET) {
		return bad_arg("AT#MODE=<IDLE|TX|RX|CW>");
	}
	long v;

	for (v = 0; v < TEST_MODE_COUNT; v++) {
		if (strcasecmp(arg, settings_mode_name(v)) == 0) {
			break;
		}
	}
	if (v == TEST_MODE_COUNT && !parse_long(arg, 0, TEST_MODE_COUNT - 1, &v)) {
		return bad_arg("AT#MODE=<IDLE|TX|RX|CW>");
	}
	s.mode = v;
	return apply(&s);
}

static int cmd_band(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	uint16_t min, max;
	long band, carrier = -1;

	if (t == AT_QUERY) {
		dect_band_carrier_range(s.band, &min, &max);
		at_printf("+BAND: %u (carriers %u-%u)\r\n", s.band, min, max);
		return 0;
	}
	char *comma = (t == AT_SET) ? strchr(arg, ',') : NULL;

	if (comma) {
		*comma = '\0';
		if (!parse_long(comma + 1, 0, 0xffff, &carrier)) {
			return bad_arg("AT#BAND=<1|2|4|9|22>[,<carrier>]");
		}
	}
	if (t != AT_SET || !parse_long(arg, 0, 255, &band) || !dect_band_is_known(band)) {
		return bad_arg("AT#BAND=<1|2|4|9|22>[,<carrier>]");
	}
	s.band = band;
	if (carrier >= 0) {
		s.carrier = carrier;
	} else if (!dect_carrier_valid(s.band, s.carrier)) {
		dect_band_carrier_range(s.band, &min, &max);
		s.carrier = min;
		at_printf("+CARRIER: %u (moved to band minimum)\r\n", s.carrier);
	}
	return apply(&s);
}

static int cmd_carrier(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	char mhz[20];
	long v;

	if (t == AT_QUERY) {
		fmt_mhz(mhz, sizeof(mhz), s.carrier);
		at_printf("+CARRIER: %u (%s, band %u)\r\n", s.carrier, mhz, s.band);
		return 0;
	}
	if (t == AT_TEST) {
		uint16_t min, max;

		dect_band_carrier_range(s.band, &min, &max);
		at_printf("+CARRIER: (%u-%u) for band %u\r\n", min, max, s.band);
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 0, 0xffff, &v)) {
		return bad_arg("AT#CARRIER=<n>");
	}
	s.carrier = v;
	return apply(&s);
}

static int cmd_mcs(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		at_printf("+MCS: %u\r\n", s.mcs);
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 0, DECT_MCS_MAX, &v)) {
		return bad_arg("AT#MCS=<0-4>");
	}
	s.mcs = v;
	return apply(&s);
}

static int cmd_txpwr(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		at_printf("+TXPWR: %d dBm (PHY code %d)\r\n", s.tx_power_dbm,
			  dect_power_code(s.tx_power_dbm));
		return 0;
	}
	if (t == AT_TEST) {
		at_printf("+TXPWR: (%s) dBm\r\n", dect_power_table_str());
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, -40, 23, &v) || dect_power_code(v) < 0) {
		at_printf("+ERROR: allowed dBm values: %s\r\n", dect_power_table_str());
		return -EINVAL;
	}
	s.tx_power_dbm = v;
	return apply(&s);
}

static int cmd_subslots(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		uint32_t us_x100 = s.subslots * DECT_SUBSLOT_US_X100;

		at_printf("+SUBSLOTS: %u (payload %u bytes, %u.%02u us)\r\n", s.subslots,
			  dect_tb_bytes(s.mcs, s.subslots), us_x100 / 100, us_x100 % 100);
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 1, DECT_SUBSLOTS_MAX, &v)) {
		return bad_arg("AT#SUBSLOTS=<1-16>");
	}
	s.subslots = v;
	return apply(&s);
}

static int cmd_gap(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		at_printf("+GAP: %u ms\r\n", s.tx_gap_ms);
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 0, 60000, &v)) {
		return bad_arg("AT#GAP=<0-60000 ms>");
	}
	s.tx_gap_ms = v;
	return apply(&s);
}

static int cmd_count(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		at_printf("+COUNT: %u%s\r\n", s.tx_count, s.tx_count ? "" : " (continuous)");
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 0, 100000000, &v)) {
		return bad_arg("AT#COUNT=<0=continuous|packets>");
	}
	s.tx_count = v;
	return apply(&s);
}

static int cmd_lbt(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long en, thr = s.lbt_threshold_dbm, per = s.lbt_period_symbols;

	if (t == AT_QUERY) {
		at_printf("+LBT: %u,%d,%u\r\n", s.lbt_enable, s.lbt_threshold_dbm,
			  s.lbt_period_symbols);
		return 0;
	}
	const char *usage = "AT#LBT=<0|1>[,<threshold dBm>[,<period symbols 2-110>]]";

	if (t != AT_SET) {
		return bad_arg(usage);
	}
	char *p2 = strchr(arg, ',');
	char *p3 = NULL;

	if (p2) {
		*p2++ = '\0';
		p3 = strchr(p2, ',');
		if (p3) {
			*p3++ = '\0';
		}
	}
	if (!parse_long(arg, 0, 1, &en) || (p2 && !parse_long(p2, -128, 0, &thr)) ||
	    (p3 && !parse_long(p3, 0, 255, &per))) {
		return bad_arg(usage);
	}
	s.lbt_enable = en;
	s.lbt_threshold_dbm = thr;
	s.lbt_period_symbols = per;
	return apply(&s);
}

static int cmd_netid(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	char *end;

	if (t == AT_QUERY) {
		at_printf("+NETID: 0x%08X\r\n", s.network_id);
		return 0;
	}
	if (t != AT_SET || !*arg) {
		return bad_arg("AT#NETID=<32-bit value, e.g. 0x12345678>");
	}
	errno = 0;
	unsigned long v = strtoul(arg, &end, 0);

	if (errno || *end || v > UINT32_MAX) {
		return bad_arg("AT#NETID=<32-bit value, e.g. 0x12345678>");
	}
	s.network_id = v;
	return apply(&s);
}

static int cmd_autostart(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		at_printf("+AUTOSTART: %u\r\n", s.autostart);
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 0, 1, &v)) {
		return bad_arg("AT#AUTOSTART=<0|1>");
	}
	s.autostart = v;
	return apply(&s);
}

static int cmd_rxreport(enum at_type t, char *arg)
{
	struct fcc_settings s = *settings_get();
	long v;

	if (t == AT_QUERY) {
		at_printf("+RXREPORT: %u s\r\n", s.rx_report_s);
		return 0;
	}
	if (t != AT_SET || !parse_long(arg, 0, 3600, &v)) {
		return bad_arg("AT#RXREPORT=<0=off|seconds>");
	}
	s.rx_report_s = v;
	return apply(&s);
}

void at_print_config(void)
{
	const struct fcc_settings *s = settings_get();
	char mhz[20];
	uint32_t us_x100 = s->subslots * DECT_SUBSLOT_US_X100;

	fmt_mhz(mhz, sizeof(mhz), s->carrier);
	at_printf("+CFG: MODE=%s\r\n", settings_mode_name(s->mode));
	at_printf("+CFG: BAND=%u\r\n", s->band);
	at_printf("+CFG: CARRIER=%u (%s)\r\n", s->carrier, mhz);
	at_printf("+CFG: MCS=%u\r\n", s->mcs);
	at_printf("+CFG: TXPWR=%d dBm\r\n", s->tx_power_dbm);
	at_printf("+CFG: SUBSLOTS=%u (payload %u bytes, %u.%02u us)\r\n", s->subslots,
		  dect_tb_bytes(s->mcs, s->subslots), us_x100 / 100, us_x100 % 100);
	at_printf("+CFG: GAP=%u ms\r\n", s->tx_gap_ms);
	at_printf("+CFG: COUNT=%u\r\n", s->tx_count);
	at_printf("+CFG: LBT=%u,%d,%u\r\n", s->lbt_enable, s->lbt_threshold_dbm,
		  s->lbt_period_symbols);
	at_printf("+CFG: NETID=0x%08X\r\n", s->network_id);
	at_printf("+CFG: AUTOSTART=%u\r\n", s->autostart);
	at_printf("+CFG: RXREPORT=%u s\r\n", s->rx_report_s);
}

static int cmd_cfg(enum at_type t, char *arg)
{
	if (t != AT_QUERY) {
		return bad_arg("AT#CFG?");
	}
	at_print_config();
	return 0;
}

static int cmd_start(enum at_type t, char *arg)
{
	const struct fcc_settings *s = settings_get();

	if (t != AT_EXEC) {
		return bad_arg("AT#START");
	}
	if (s->mode == TEST_MODE_IDLE) {
		at_printf("+ERROR: mode is IDLE, set AT#MODE first\r\n");
		return -EINVAL;
	}
	int err = rf_test_start();

	if (err == -EBUSY) {
		at_printf("+ERROR: already running\r\n");
	} else if (err) {
		at_printf("+ERROR: start failed (%d / 0x%x)\r\n", err, err);
	} else {
		char mhz[20];

		fmt_mhz(mhz, sizeof(mhz), s->carrier);
		at_printf("+TEST: STARTED %s carrier %u (%s) mcs %u %d dBm\r\n",
			  settings_mode_name(s->mode), s->carrier, mhz, s->mcs, s->tx_power_dbm);
	}
	return err;
}

static int cmd_stop(enum at_type t, char *arg)
{
	if (t != AT_EXEC) {
		return bad_arg("AT#STOP");
	}
	uint8_t was = rf_test_active_mode();
	int err = rf_test_stop();

	if (err) {
		at_printf("+ERROR: stop failed (%d / 0x%x)\r\n", err, err);
		return err;
	}
	if (was == TEST_MODE_TX) {
		struct rf_tx_stats tx;

		rf_test_tx_stats(&tx);
		at_printf("+TXSTATS: sent=%u ok=%u lbt_busy=%u failed=%u\r\n", tx.sent, tx.ok,
			  tx.lbt_busy, tx.failed);
	} else if (was == TEST_MODE_RX) {
		rf_test_print_rx_stats("+RXSTATS");
	}
	at_printf("+TEST: STOPPED\r\n");
	return 0;
}

static int cmd_status(enum at_type t, char *arg)
{
	struct rf_tx_stats tx;
	int16_t temp;
	uint16_t mv;

	if (t != AT_QUERY) {
		return bad_arg("AT#STATUS?");
	}
	rf_test_tx_stats(&tx);
	rf_test_modem_health(&temp, &mv);
	at_printf("+STATUS: %s,%s\r\n", rf_test_running() ? "RUNNING" : "STOPPED",
		  settings_mode_name(rf_test_running() ? rf_test_active_mode() :
							  settings_get()->mode));
	at_printf("+TXSTATS: sent=%u ok=%u lbt_busy=%u failed=%u last_err=0x%x\r\n", tx.sent,
		  tx.ok, tx.lbt_busy, tx.failed, tx.last_err);
	rf_test_print_rx_stats("+RXSTATS");
	at_printf("+MODEM: temp=%d C voltage=%u mV\r\n", temp, mv);
	return 0;
}

static int cmd_rxstats(enum at_type t, char *arg)
{
	if (t == AT_QUERY) {
		rf_test_print_rx_stats("+RXSTATS");
		return 0;
	}
	if (t == AT_SET && strcmp(arg, "0") == 0) {
		rf_test_rx_stats_clear();
		return 0;
	}
	return bad_arg("AT#RXSTATS? | AT#RXSTATS=0 (clear)");
}

static int cmd_bands(enum at_type t, char *arg)
{
	const struct dect_band_info *list;
	int n = dect_bands_get_modem_list(&list);

	if (t != AT_QUERY) {
		return bad_arg("AT#BANDS?");
	}
	if (n == 0) {
		at_printf("+BANDS: modem band list not available\r\n");
	}
	for (int i = 0; i < n; i++) {
		char lo[20], hi[20];

		fmt_mhz(lo, sizeof(lo), list[i].min_carrier);
		fmt_mhz(hi, sizeof(hi), list[i].max_carrier);
		at_printf("+BANDS: band=%u group=%u carriers=%u-%u (%s - %s) power_class=%u "
			  "rx_gain=%d\r\n",
			  list[i].band, list[i].group, list[i].min_carrier, list[i].max_carrier, lo,
			  hi, list[i].power_class, list[i].rx_gain);
	}
	return 0;
}

static int cmd_factory(enum at_type t, char *arg)
{
	struct fcc_settings s;

	if (t != AT_EXEC) {
		return bad_arg("AT#FACTORY");
	}
	settings_defaults(&s);
	return apply(&s);
}

static int cmd_reboot(enum at_type t, char *arg)
{
	if (t != AT_EXEC) {
		return bad_arg("AT#REBOOT");
	}
	(void)rf_test_stop();
	at_printf("OK\r\n");
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int cmd_help(enum at_type t, char *arg);

static const struct {
	const char *name;
	int (*fn)(enum at_type t, char *arg);
	const char *help;
} cmds[] = {
	{ "AT", cmd_at, "AT                       attention" },
	{ "ATI", cmd_ati, "ATI                      firmware version" },
	{ "AT#HELP", cmd_help, "AT#HELP                  this list" },
	{ "AT#MODE", cmd_mode, "AT#MODE=<IDLE|TX|RX|CW>  test mode" },
	{ "AT#BAND", cmd_band, "AT#BAND=<1|2|4|9|22>[,c] DECT NR+ band" },
	{ "AT#CARRIER", cmd_carrier, "AT#CARRIER=<n>           absolute carrier number (=? range)" },
	{ "AT#MCS", cmd_mcs, "AT#MCS=<0-4>             modulation and coding scheme" },
	{ "AT#TXPWR", cmd_txpwr, "AT#TXPWR=<dBm>           TX power (=? allowed values)" },
	{ "AT#SUBSLOTS", cmd_subslots, "AT#SUBSLOTS=<1-16>       packet length" },
	{ "AT#GAP", cmd_gap, "AT#GAP=<ms>              idle time between TX packets" },
	{ "AT#COUNT", cmd_count, "AT#COUNT=<n>             TX packets per test, 0=continuous" },
	{ "AT#LBT", cmd_lbt, "AT#LBT=<0|1>[,dBm[,sym]] listen-before-talk" },
	{ "AT#NETID", cmd_netid, "AT#NETID=<0x...>         network ID (TX and RX must match)" },
	{ "AT#AUTOSTART", cmd_autostart, "AT#AUTOSTART=<0|1>       start stored mode at power-up" },
	{ "AT#RXREPORT", cmd_rxreport, "AT#RXREPORT=<s>          RX stats period, 0=off" },
	{ "AT#CFG", cmd_cfg, "AT#CFG?                  all settings" },
	{ "AT#START", cmd_start, "AT#START                 start the stored mode" },
	{ "AT#STOP", cmd_stop, "AT#STOP                  stop and print stats" },
	{ "AT#STATUS", cmd_status, "AT#STATUS?               state, stats, temperature" },
	{ "AT#RXSTATS", cmd_rxstats, "AT#RXSTATS? / =0         RX statistics / clear" },
	{ "AT#BANDS", cmd_bands, "AT#BANDS?                bands reported by the modem" },
	{ "AT#FACTORY", cmd_factory, "AT#FACTORY               restore and save defaults" },
	{ "AT#REBOOT", cmd_reboot, "AT#REBOOT                reboot" },
};

static int cmd_help(enum at_type t, char *arg)
{
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		at_printf("%s\r\n", cmds[i].help);
	}
	return 0;
}

static void process_line(char *line)
{
	while (*line == ' ') {
		line++;
	}
	size_t len = strlen(line);

	while (len && line[len - 1] == ' ') {
		line[--len] = '\0';
	}
	if (len == 0) {
		return;
	}

	enum at_type t = AT_EXEC;
	char *arg = "";
	char *eq = strchr(line, '=');

	if (eq) {
		*eq = '\0';
		arg = eq + 1;
		t = (strcmp(arg, "?") == 0) ? AT_TEST : AT_SET;
	} else if (line[len - 1] == '?') {
		line[len - 1] = '\0';
		t = AT_QUERY;
	}
	for (char *p = line; *p; p++) {
		*p = toupper((unsigned char)*p);
	}
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		if (strcmp(line, cmds[i].name) == 0) {
			int err = cmds[i].fn(t, arg);

			at_printf(err ? "ERROR\r\n" : "OK\r\n");
			return;
		}
	}
	at_printf("+ERROR: unknown command, try AT#HELP\r\nERROR\r\n");
}

void at_run(void)
{
	char line[LINE_MAX];

	for (;;) {
		k_msgq_get(&line_q, line, K_FOREVER);
		process_line(line);
	}
}
