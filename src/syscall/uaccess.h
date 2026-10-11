#ifndef UACCESS_H
#define UACCESS_H

#include "src/types.h"
#include "src/scheduler/scheduler.h"
#include "src/memory_manager/task_memory.h"

struct task;

int uaccess_range_ok(const struct task *t, const void *addr, uint32_t len, int write);
int uaccess_strlen(const struct task *t, const char *s, uint32_t max);

#endif /* UACCESS_H */
