# DECT NR+ FCC / RF Test Firmware

Lab firmware for the IRISS DECT NR+ Plug-In board (150067, nRF9151). It puts the radio into a
fixed TX, RX or CW test mode, controlled by AT commands. Settings are saved in EEPROM.

> Test use only. The radio runs without the product's encryption, MAC or channel-access rules.
> Never ship this image.

## Build and flash

```powershell
./build.ps1                       # -> build/merged.hex  (NCS v3.2.4, nrf9151dk/nrf9151/ns)
./program.ps1 -Serial <JLinkSN>   # chip erase + flash + reset
./program.ps1 -Serial <JLinkSN> -Modem   # also flash mfw-nr+-phy_nrf91x1_2.0.0 first
```

A chip erase removes the product firmware and its radio key (TF-M ITS). The EEPROM is not erased.
To go back to the product image, use `dect_nr_mesh/program.ps1`. It re-stages the radio key.

## Connections

| Signal | Board | Settings |
|---|---|---|
| AT port | UART1: TXD P0.29 = **J1-74**, RXD P0.28 = **J1-72**, GND J1-11/20/22 | 115200 8N1, no flow control, 3.3 V |
| Debug log | UART0 TX P0.27 = **J1-71** (TX_DBG), TX only | 115200 8N1 |
| RF | **AT2** (U.FL) = nRF9151 ANT. AT1 is GNSS and has 3.3 V DC bias on it | |

Commands end with CR or LF. There is no echo, so turn on local echo in your terminal.

LEDs: green LD1 (and J1-73) is on while a test runs. Red LD2 is on when idle, and blinks after a
fault such as a PHY init failure or repeated TX/RX errors.

## AT commands

Every `=` setting is checked, saved to EEPROM straight away, and read back to verify. Settings
can only be changed while the test is stopped. A command returns `OK`, or a `+ERROR: <reason>`
line followed by `ERROR`.

| Command | Description | Default |
|---|---|---|
| `AT#MODE=<IDLE\|TX\|RX\|CW>` | Test mode. TX = modulated DECT NR+ packets. RX = receive + PER. CW = unmodulated carrier | IDLE |
| `AT#BAND=<1\|2\|4\|9\|22>[,<carrier>]` | Band. If no carrier is given and the current one is outside the band, the band minimum is used | 4 |
| `AT#CARRIER=<n>` / `AT#CARRIER=?` | Absolute carrier number / allowed range | 525 |
| `AT#MCS=<0-4>` | Modulation and coding scheme | 2 |
| `AT#TXPWR=<dBm>` / `AT#TXPWR=?` | One of -40,-30,-20,-16,-12,-8,-4,0,4,7,10,13,16,19,21,23 | 19 |
| `AT#SUBSLOTS=<1-16>` | Packet length in subslots (208.33 µs each); must be supported by the MCS | 4 |
| `AT#GAP=<ms>` | Idle time between TX packets (0 = as fast as the modem allows) | 0 |
| `AT#COUNT=<n>` | TX packets per test, 0 = continuous | 0 |
| `AT#LBT=<0\|1>[,<dBm>[,<symbols>]]` | Listen-before-talk on/off, threshold, period (2-110 symbols) | 0,-85,2 |
| `AT#NETID=<0x...>` | Network ID. TX and RX boards must match | 0x12345678 |
| `AT#AUTOSTART=<0\|1>` | Start the stored mode at power-up, without a host | 0 |
| `AT#RXREPORT=<s>` | Period of `+RXSTATS` reports in RX mode, 0 = off | 1 |
| `AT#CFG?` | Print all settings | |
| `AT#START` / `AT#STOP` | Start the stored mode / stop it and print stats | |
| `AT#STATUS?` | Running state, TX/RX counters, modem temperature and voltage | |
| `AT#RXSTATS?` / `AT#RXSTATS=0` | RX statistics / clear | |
| `AT#BANDS?` | Bands, carrier ranges and power class reported by the modem | |
| `AT#FACTORY` | Restore and save defaults | |
| `ATI`, `AT#HELP`, `AT#REBOOT` | | |

Carrier frequency is shown as 450.144 MHz + n × 0.864 MHz (ETSI TS 103 636-2 §5.4.2).
Examples: carrier 525 = 903.744 MHz, 538 = 914.976 MHz, 551 = 926.208 MHz (band 4);
1703 = 1921.536 MHz (band 9). Check the frequency on a spectrum analyzer before formal testing.

## Typical use

TX board (for example band 4, middle channel, MCS 2, 19 dBm):

```
AT#MODE=TX
AT#BAND=4,538
AT#MCS=2
AT#TXPWR=19
AT#START
```

RX board (same band, carrier and network ID):

```
AT#MODE=RX
AT#BAND=4,538
AT#START
+RXSTATS: rx=4210 lost=3 per=0.0% pcc_ok=4213 pcc_err=0 pdc_ok=4213 pdc_err=0 rssi=-52.5(min -54.0 max -51.0)dBm snr=31.2dB
```

PER is counted from sequence numbers in the TX payload, so start the RX board first.
To let a board transmit at power-up with nothing connected (radiated tests), set `AT#AUTOSTART=1`
and power-cycle it.

## Notes

- TX packets go out back-to-back with immediate start. The modem adds a short turnaround gap
  between packets, so the duty cycle is below 100 %. Use `AT#MODE=CW` for an unmodulated carrier.
- CW uses the modem's certification API. That API needs the PHY de-initialized, so the firmware
  does this on `AT#START` and brings the PHY back up on `AT#STOP`.
- Band 4 (902-928 MHz) uses PHY band group 1 and only carriers 525-551. Nordic lists band 4 as
  R&D-only on nRF9151 LACA A0, and as qualified on LACA A0AB. Confirm which SiP revision the
  boards have.
- Settings are stored in M24M02 block `0x52`, offset 0, with a CRC32. The mesh firmware leaves
  this block unallocated, so product data in `0x50`/`0x51` is not touched.
- On schematic rev X1, U5 pin 7 (WC̅) is tied to +3V3, which write-protects the EEPROM.
  On a board wired like that, every `=` command reports `+ERROR: EEPROM save failed`. The value
  still applies until reboot.
