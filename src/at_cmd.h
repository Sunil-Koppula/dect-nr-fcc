/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    at_cmd.h
*  @brief   AT command interface on UART1 (J1 TXD/RXD)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#ifndef AT_CMD_H_
#define AT_CMD_H_

int at_init(void);
/* Thread-safe output to the AT port; caller supplies line endings. */
void at_printf(const char *fmt, ...);
void at_print_config(void);
/* Processes received command lines; never returns. */
void at_run(void);

#endif /* AT_CMD_H_ */
