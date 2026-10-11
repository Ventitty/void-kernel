#include "src/memory_manager/task_memory.h"

void *task_mem_alloc(task_t *t, uint32_t size) {
    if (t == NULL || size == 0) return NULL;

    task_block_t *node = (task_block_t *)nmap(sizeof(task_block_t));
    if (node == NULL) return NULL;

    void *block = nmap(size);
    if (block == NULL) {
        unmap(node);
        return NULL;
    }

    memset(block, 0, size);

    node->start = (uint32_t)block;
    node->size  = size;
    node->next  = t->blocks;
    t->blocks   = node;
    return block;
}

int task_mem_free(task_t *t, void *addr) {
    if (t == NULL || addr == NULL) return -1;

    for (task_block_t **link = &t->blocks; *link != NULL; link = &(*link)->next) {
        task_block_t *node = *link;
        if (node->start == (uint32_t)addr) {
            *link = node->next;
            unmap(addr);
            unmap(node);
            return 0;
        }
    }
    return -1;
}

void task_mem_release_all(task_t *t) {
    if (t == NULL) return;

    task_block_t *node = t->blocks;
    t->blocks = NULL;
    while (node != NULL) {
        task_block_t *next = node->next;
        unmap((void *)node->start);
        unmap(node);
        node = next;
    }
}

int task_mem_lookup(const task_t *t, uint32_t addr, uint32_t *end) {
    if (t == NULL) return 0;

    for (const task_block_t *node = t->blocks; node != NULL; node = node->next) {
        if (addr >= node->start && addr < node->start + node->size) {
            if (end) *end = node->start + node->size;
            return 1;
        }
    }
    return 0;
}
