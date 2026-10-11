/* Frontend services used by the RV32 JIT.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gpsp_rv32_platform.h"

static const struct gpsp_rv32_platform_ops *platform_ops;

void gpsp_rv32_set_platform(const struct gpsp_rv32_platform_ops *ops)
{
  /* An executable allocation must always have matching cleanup and publish. */
  platform_ops = ops && ops->exec_alloc && ops->exec_free &&
      ops->exec_publish ? ops : NULL;
}

void *gpsp_rv32_exec_alloc(size_t alignment, size_t size)
{
  if (!platform_ops || !platform_ops->exec_alloc)
    return NULL;
  return platform_ops->exec_alloc(alignment, size);
}

void gpsp_rv32_exec_free(void *code)
{
  if (platform_ops && platform_ops->exec_free)
    platform_ops->exec_free(code);
}

bool gpsp_rv32_exec_publish(void *code, size_t size)
{
  if (!platform_ops || !platform_ops->exec_publish)
    return false;
  return platform_ops->exec_publish(code, size);
}

void gpsp_rv32_log(gpsp_rv32_log_level level, const char *tag,
    const char *format, ...)
{
  if (!platform_ops || !platform_ops->vlog)
    return;

  va_list args;
  va_start(args, format);
  platform_ops->vlog(level, tag, format, args);
  va_end(args);
}
