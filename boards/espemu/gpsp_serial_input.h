/* Button input from the host viewer over the console UART.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SERIAL_INPUT_H
#define GPSP_SERIAL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the reader once; later calls do nothing. */
void gpsp_serial_input_start(void);
/* RetroPad bits, plus bit 16 for a home press. */
uint32_t gpsp_serial_input_read(void);
uint16_t gpsp_serial_input_read_command(void);
void gpsp_serial_input_report_command(uint16_t command, bool success);
void gpsp_serial_input_stop(void);

#endif /* GPSP_SERIAL_INPUT_H */
