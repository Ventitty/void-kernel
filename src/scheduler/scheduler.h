#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "src/types.h"
#include "src/interrupts/interrupts.h"
#include "src/memory_manager/memory.h"
#include "src/scheduler/spinlock.h"
#include "src/utils/utils.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "src/time/time.h"
#include "src/memory_manager/task_memory.h"

#define STACK_FILL        0xA5A5A5A5u
#define STACK_GUARD_BYTES 32u
#define TASK_MAX_MEM_RANGES 4

#define WAIT_QUEUE_INIT { NULL, NULL }

struct task_block;

typedef struct {
    uint32_t start;
    uint32_t end;
    uint8_t  writable;
} task_mem_range_t;

typedef enum {
    TASK_READY = 0,   /* prête, en attente d'un cœur                        */
    TASK_RUNNING,     /* en cours d'exécution sur un cœur                   */
    TASK_BLOCKED,     /* en attente d'une primitive (mutex, sémaphore...)   */
    TASK_ZOMBIE,      /* terminée, mémoire libérée au prochain changement   */
} task_state_t;

typedef struct task {
    uint32_t     *sp;            /* contexte sauvegardé (cpu_context_t)     */
    void         *stack;         /* bas de la pile (bloc nmap)              */
    uint32_t      stack_size;
    volatile task_state_t state;
    uint8_t       pinned_core;   /* 0, 1 ou TASK_ANY_CORE                   */
    uint8_t       running_on;    /* cœur qui l'exécute, ou TASK_ANY_CORE    */
    int           id;            /* identifiant unique, jamais réutilisé    */

    struct task  *next;          /* liste globale des tâches ordonnançables */

    struct task  *wait_next;
    uint32_t      wait_mask;     /* event group : bits attendus             */
    uint8_t       wait_all;      /* event group : tous les bits ou un seul  */

    struct task  *sleep_next;
    uint32_t      wake_tick;
    uint8_t       sleeping;

    task_mem_range_t mem[TASK_MAX_MEM_RANGES];
    uint8_t          mem_count;

    struct task_block *blocks;
} task_t;

typedef struct {
    task_t *head;
    task_t *tail;
} wait_queue_t;

static inline void wait_queue_init(wait_queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
}

static inline void wait_queue_push(wait_queue_t *q, task_t *t) {
    t->wait_next = NULL;
    if (q->tail) q->tail->wait_next = t;
    else         q->head = t;
    q->tail = t;
}

static inline task_t *wait_queue_pop(wait_queue_t *q) {
    task_t *t = q->head;
    if (t) {
        q->head = t->wait_next;
        if (q->head == NULL) q->tail = NULL;
        t->wait_next = NULL;
    }
    return t;
}

static inline task_t *wait_queue_take_all(wait_queue_t *q) {
    task_t *t = q->head;
    q->head = NULL;
    q->tail = NULL;
    return t;
}

void scheduler_init(void);
int task_create(void (*entry)(void));
int task_create_pinned(void (*entry)(void), uint8_t core);
int task_create_user(void (*entry)(void));
int task_create_user_pinned(void (*entry)(void), uint8_t core);
int task_create_user_mem(void (*entry)(void), const task_mem_range_t *ranges, uint32_t count);

void scheduler_start(void);
uint32_t *schedule_next_task(uint32_t *current_sp);
uint32_t *scheduler_terminate_current(uint32_t *sp);

uint32_t get_core_id(void);
task_t  *scheduler_current_task(void);      /* NULL hors tâche (boot)       */
int      scheduler_current_task_id(void);   /* -1 hors tâche                */

task_t *scheduler_mark_blocked_self(void);
void    scheduler_yield_blocked(task_t *me);
void    scheduler_block_current(void);
void    scheduler_unblock(task_t *t);
task_t *scheduler_sleep_prepare(uint32_t wake_tick);
uint32_t *scheduler_sleep_from_syscall(uint32_t *sp, uint32_t wake_tick);
void scheduler_wake_sleepers(uint32_t now);
uint32_t scheduler_stack_unused(int task_id);
uint32_t scheduler_task_count(void);

#endif /* SCHEDULER_H */
