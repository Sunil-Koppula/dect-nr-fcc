/*
+------------------------------------------------------------------------------+
| IRISS Inc.
| -----------------------------------------------------------------------------
| Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
+------------------------------------------------------------------------------+
*/
/**
*  @file    status_led.h
*  @brief   Test state on LD1 (green), LD2 (red) and LED2 (J1-73)
*  @author  S.Koppula
*  @version 1.0.0
*  @date    11/10/2026
*---------------------------------------------------------------------------
Version     | Date      | Author        | Comments
 1.0.0      11/10/2026   S.Koppula        Initial Development
*/

#ifndef STATUS_LED_H_
#define STATUS_LED_H_

#include <stdbool.h>

int status_led_init(void);
/* running: green on. idle: red on. fault: red blinks. */
void status_led_set(bool running, bool fault);

#endif /* STATUS_LED_H_ */
