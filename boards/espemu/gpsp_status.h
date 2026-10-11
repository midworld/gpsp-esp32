/* CPU use, free memory and chip temperature reports.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_STATUS_H
#define GPSP_STATUS_H

void gpsp_status_start(void);
/** Log one line per task: core, CPU share over one second, free stack. */
void gpsp_status_print_tasks(void);
void gpsp_status_stop(void);

#endif /* GPSP_STATUS_H */
