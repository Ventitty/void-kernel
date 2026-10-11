#ifndef TASK_MEMORY_H
#define TASK_MEMORY_H

#include "src/types.h"
#include "src/scheduler/scheduler.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"

struct task;

typedef struct task_block {
    struct task_block *next;
    uint32_t           start;
    uint32_t           size;
} task_block_t;

void *task_mem_alloc(struct task *t, uint32_t size);
int task_mem_free(struct task *t, void *addr);
void task_mem_release_all(struct task *t);
int task_mem_lookup(const struct task *t, uint32_t addr, uint32_t *end);

#endif /* TASK_MEMORY_H */
