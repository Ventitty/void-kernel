#include "src/scheduler/scheduler.h"
#include "src/utils/utils.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "src/time/time.h"
#include "src/memory_manager/task_memory.h"

static spinlock_t tasks_lock = SPINLOCK_INIT;

static task_t  *task_list  = NULL;
static task_t  *cursor     = NULL;
static uint32_t task_count = 0;
static int      next_id    = 0;

static task_t *volatile current_task[2] = { NULL, NULL };
static task_t *idle_task[2] = { NULL, NULL };
static task_t *retired[2]   = { NULL, NULL };

/* Tâches endormies, triées par échéance croissante (sous tasks_lock). */
static task_t *sleep_list = NULL;

static void stack_overflow_halt(const task_t *t, uint32_t sp) {
    uart_print("\n[noyau] DEBORDEMENT DE PILE : tache ");
    uart_print_hex((uint32_t)t->id);
    uart_print(", sp=");
    uart_print_hex(sp);
    uart_print(", pile=");
    uart_print_hex((uint32_t)t->stack);
    uart_print(" (pile trop petite, ou recursion)\n");
    uart_print("[noyau] coeur arrete : la memoire voisine a pu etre corrompue.\n");

    (void)cpu_irq_mask_all();
    while (1) { }
}

static void stack_check_outgoing(const task_t *t, uint32_t *current_sp) {
    if (t == NULL) return;

    uint32_t base = (uint32_t)t->stack;
    if ((uint32_t)current_sp < base + STACK_GUARD_BYTES) {
        stack_overflow_halt(t, (uint32_t)current_sp);
    }

    const volatile uint32_t *guard = (const volatile uint32_t *)base;
    for (uint32_t i = 0; i < STACK_GUARD_BYTES / 4u; i++) {
        if (guard[i] != STACK_FILL) {
            stack_overflow_halt(t, (uint32_t)current_sp);
        }
    }
}

static void list_append_locked(task_t *t) {
    t->next = NULL;
    if (task_list == NULL) {
        task_list = t;
    } else {
        task_t *it = task_list;
        while (it->next) it = it->next;
        it->next = t;
    }
    task_count++;
}

static void list_remove_locked(task_t *t) {
    task_t *prev = NULL;
    for (task_t *it = task_list; it; prev = it, it = it->next) {
        if (it != t) continue;

        if (prev) prev->next = t->next;
        else      task_list = t->next;

        if (cursor == t) cursor = prev;

        t->next = NULL;
        task_count--;
        return;
    }
}

static task_t *find_locked(int id) {
    for (task_t *it = task_list; it; it = it->next) {
        if (it->id == id) return it;
    }
    return NULL;
}

static void mark_zombie_locked(task_t *t) {
    t->state = TASK_ZOMBIE;
    list_remove_locked(t);
}

static int eligible(const task_t *t, uint32_t core) {
    return t->state == TASK_READY
    && t->running_on == TASK_ANY_CORE
    && (t->pinned_core == TASK_ANY_CORE || t->pinned_core == core);
}

static task_t *pick_next_locked(uint32_t core) {
    if (task_list == NULL) return NULL;

    task_t *start = (cursor && cursor->next) ? cursor->next : task_list;
    task_t *t = start;
    do {
        if (eligible(t, core)) {
            cursor = t;
            return t;
        }
        t = t->next ? t->next : task_list;
    } while (t != start);

    return NULL;
}

static void task_exit(void) {
    spinlock_acquire(&tasks_lock);
    task_t *me = current_task[cpu_core_id()];
    if (me != NULL) mark_zombie_locked(me);
    spinlock_release(&tasks_lock);

    while (1) cpu_wait_irq();
}

static void idle_loop(void) {
    while (1) cpu_wait_irq();
}

static task_t *task_alloc(void (*entry)(void), uint8_t pinned_core, int user_mode, uint32_t stack_size) {
    task_t *t = (task_t *)nmap(sizeof(task_t));
    if (t == NULL) return NULL;

    void *stack = nmap(stack_size);
    if (stack == NULL) {
        unmap(t);
        return NULL;
    }

    memset(t, 0, sizeof(task_t));

    uint8_t *stack_top = (uint8_t *)(((uint32_t)stack + stack_size) & ~0xFu);
    cpu_context_t *ctx = (cpu_context_t *)(stack_top - sizeof(cpu_context_t));

    for (uint32_t *p = (uint32_t *)stack; p < (uint32_t *)ctx; p++) *p = STACK_FILL;
    for (uint32_t *p = (uint32_t *)ctx; p < (uint32_t *)stack_top; p++) *p = 0;

    ctx->sp_orig  = (uint32_t)stack_top;
    ctx->epc      = (uint32_t)entry;
    ctx->a0       = (uint32_t)task_exit;
    ctx->exccause = EXCCAUSE_LEVEL1_INTERRUPT;

    /* PS de départ : celui du créateur, sans son niveau d'interruption ni
     * EXCM. Une tâche créée alors que le créateur tient un spinlock (niveau
     * 3) ou traite une interruption ne doit pas hériter de ce masquage :
     * elle tournerait sans jamais être préemptée par le tick. */
    uint32_t ps = cpu_read_ps() & ~(PS_INTLEVEL_MASK | PS_EXCM_MASK);
    if (user_mode) ps |= PS_UM_MASK;
    ctx->ps = ps;

    t->sp          = (uint32_t *)ctx;
    t->stack       = stack;
    t->stack_size  = stack_size;
    t->state       = TASK_READY;
    t->pinned_core = pinned_core;
    t->running_on  = TASK_ANY_CORE;
    t->id          = -1;

    t->mem[0].start    = (uint32_t)stack;
    t->mem[0].end      = (uint32_t)stack + stack_size;
    t->mem[0].writable = 1;
    t->mem_count       = 1;
    return t;
}

static void task_free(task_t *t) {
    /* Mémoire obtenue par VOID_SYS_MEM_ALLOC et jamais rendue : récupérée
     * ici, un programme qui oublie ses free() ne fait pas fuir le tas. */
    task_mem_release_all(t);
    unmap(t->stack);
    unmap(t);
}

void scheduler_init(void) {
    spinlock_acquire(&tasks_lock);
    task_list  = NULL;
    cursor     = NULL;
    task_count = 0;
    next_id    = 0;
    current_task[0] = current_task[1] = NULL;
    retired[0] = retired[1] = NULL;
    spinlock_release(&tasks_lock);

    for (uint8_t core = 0; core < 2; core++) {
        idle_task[core] = task_alloc(idle_loop, core, 0, IDLE_STACK_SIZE);
        if (idle_task[core] == NULL) {
            uart_print("[noyau] impossible d'allouer la tache idle\n");
            (void)cpu_irq_mask_all();
            while (1) { }
        }
    }
}

static int task_create_common(void (*entry)(void), uint8_t pinned_core, int user_mode,
                              const task_mem_range_t *ranges, uint32_t count) {
    if (count > TASK_MAX_MEM_RANGES - 1u) return -1;
    if (count > 0 && ranges == NULL) return -1;

    task_t *t = task_alloc(entry, pinned_core, user_mode, TASK_STACK_SIZE);
    if (t == NULL) return -1;

    for (uint32_t i = 0; i < count; i++) {
        t->mem[t->mem_count++] = ranges[i];
    }

    spinlock_acquire(&tasks_lock);
    t->id = next_id;
    next_id = (next_id + 1) & 0x7FFFFFFF;
    int id = t->id;
    list_append_locked(t);
    spinlock_release(&tasks_lock);

    return id;
                              }

                              int task_create(void (*entry)(void)) {
                                  return task_create_common(entry, TASK_ANY_CORE, 0, NULL, 0);
                              }

                              int task_create_pinned(void (*entry)(void), uint8_t core) {
                                  if (core > 1) return -1;
                                  return task_create_common(entry, core, 0, NULL, 0);
                              }

                              int task_create_user(void (*entry)(void)) {
                                  return task_create_common(entry, TASK_ANY_CORE, 1, NULL, 0);
                              }

                              int task_create_user_pinned(void (*entry)(void), uint8_t core) {
                                  if (core > 1) return -1;
                                  return task_create_common(entry, core, 1, NULL, 0);
                              }

                              int task_create_user_mem(void (*entry)(void), const task_mem_range_t *ranges, uint32_t count) {
                                  return task_create_common(entry, TASK_ANY_CORE, 1, ranges, count);
                              }

                              uint32_t *schedule_next_task(uint32_t *current_sp) {
                                  uint32_t core = cpu_core_id();
                                  task_t *cur = current_task[core];

                                  stack_check_outgoing(cur, current_sp);

                                  task_t *to_free = retired[core];
                                  retired[core] = NULL;

                                  spinlock_acquire(&tasks_lock);

                                  if (cur != NULL) {
                                      cur->sp = current_sp;
                                      cur->running_on = TASK_ANY_CORE;
                                      if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
                                  }

                                  task_t *next = pick_next_locked(core);
                                  if (next == NULL) next = idle_task[core];

                                  next->state = TASK_RUNNING;
                                  next->running_on = (uint8_t)core;
                                  current_task[core] = next;

                                  if (cur != NULL && cur->state == TASK_ZOMBIE) {
                                      retired[core] = cur;
                                  }

                                  uint32_t *new_sp = next->sp;
                                  spinlock_release(&tasks_lock);

                                  if (to_free != NULL) task_free(to_free);

                                  return new_sp;
                              }

                              uint32_t *scheduler_terminate_current(uint32_t *sp) {
                                  uint32_t core = cpu_core_id();

                                  spinlock_acquire(&tasks_lock);
                                  task_t *cur = current_task[core];
                                  if (cur != NULL && cur != idle_task[core]) {
                                      mark_zombie_locked(cur);
                                  }
                                  spinlock_release(&tasks_lock);

                                  return schedule_next_task(sp);
                              }

                              void scheduler_start(void) {
                                  uint32_t core = cpu_core_id();

                                  interrupts_init_this_core();

                                  current_task[core] = NULL;

                                  time_start_this_core();
                                  interrupts_enable_global();

                                  while (1) cpu_wait_irq();
                              }

                              uint32_t get_core_id(void) {
                                  return cpu_core_id();
                              }

                              task_t *scheduler_current_task(void) {
                                  uint32_t ps = cpu_irq_mask_all();
                                  uint32_t core = cpu_core_id();
                                  task_t *t = current_task[core];
                                  cpu_write_ps(ps);

                                  if (t == idle_task[0] || t == idle_task[1]) return NULL;
                                  return t;
                              }

                              int scheduler_current_task_id(void) {
                                  task_t *t = scheduler_current_task();
                                  return t ? t->id : -1;
                              }

                              task_t *scheduler_mark_blocked_self(void) {
                                  task_t *me = scheduler_current_task();
                                  if (me == NULL) return NULL;

                                  spinlock_acquire(&tasks_lock);
                                  me->state = TASK_BLOCKED;
                                  spinlock_release(&tasks_lock);

                                  return me;
                              }

                              void scheduler_yield_blocked(task_t *me) {
                                  if (me == NULL) return;

                                  while (me->state == TASK_BLOCKED) {
                                      cpu_wait_irq();
                                  }
                              }

                              void scheduler_block_current(void) {
                                  scheduler_yield_blocked(scheduler_mark_blocked_self());
                              }

                              /* ---- Sommeil ------------------------------------------------------------ */

                              static int tick_before(uint32_t a, uint32_t b) {
                                  return (int32_t)(a - b) < 0;
                              }

                              static void sleep_insert_locked(task_t *t, uint32_t wake_tick) {
                                  t->wake_tick = wake_tick;
                                  t->sleeping  = 1;

                                  /* Après les dormeurs de même échéance : réveil dans l'ordre d'arrivée. */
                                  task_t **link = &sleep_list;
                                  while (*link != NULL && !tick_before(wake_tick, (*link)->wake_tick)) {
                                      link = &(*link)->sleep_next;
                                  }
                                  t->sleep_next = *link;
                                  *link = t;
                              }

                              static void sleep_remove_locked(task_t *t) {
                                  if (!t->sleeping) return;

                                  for (task_t **link = &sleep_list; *link != NULL; link = &(*link)->sleep_next) {
                                      if (*link == t) {
                                          *link = t->sleep_next;
                                          break;
                                      }
                                  }
                                  t->sleep_next = NULL;
                                  t->sleeping   = 0;
                              }

                              task_t *scheduler_sleep_prepare(uint32_t wake_tick) {
                                  task_t *me = scheduler_current_task();
                                  if (me == NULL) return NULL;

                                  spinlock_acquire(&tasks_lock);
                                  sleep_insert_locked(me, wake_tick);
                                  me->state = TASK_BLOCKED;
                                  spinlock_release(&tasks_lock);

                                  return me;
                              }

                              uint32_t *scheduler_sleep_from_syscall(uint32_t *sp, uint32_t wake_tick) {
                                  uint32_t core = cpu_core_id();

                                  spinlock_acquire(&tasks_lock);
                                  task_t *cur = current_task[core];
                                  if (cur != NULL && cur != idle_task[core]) {
                                      sleep_insert_locked(cur, wake_tick);
                                      cur->state = TASK_BLOCKED;
                                  }
                                  spinlock_release(&tasks_lock);

                                  /* La tâche étant bloquée, schedule_next_task en élit une autre. */
                                  return schedule_next_task(sp);
                              }

                              void scheduler_wake_sleepers(uint32_t now) {
                                  spinlock_acquire(&tasks_lock);
                                  /* Liste triée : on s'arrête au premier dormeur pas encore échu. */
                                  while (sleep_list != NULL && !tick_before(now, sleep_list->wake_tick)) {
                                      task_t *t = sleep_list;
                                      sleep_list = t->sleep_next;
                                      t->sleep_next = NULL;
                                      t->sleeping   = 0;
                                      if (t->state == TASK_BLOCKED) t->state = TASK_READY;
                                  }
                                  spinlock_release(&tasks_lock);
                              }

                              void scheduler_unblock(task_t *t) {
                                  if (t == NULL) return;

                                  spinlock_acquire(&tasks_lock);
                                  /* Une tâche réveillée par une autre voie quitte la liste des dormeurs :
                                   * utile dès que les attentes auront un délai d'expiration. */
                                  sleep_remove_locked(t);
                                  if (t->state == TASK_BLOCKED) {
                                      t->state = TASK_READY;
                                  }
                                  spinlock_release(&tasks_lock);
                              }

                              uint32_t scheduler_stack_unused(int task_id) {
                                  uint32_t n = 0;

                                  spinlock_acquire(&tasks_lock);
                                  task_t *t = find_locked(task_id);
                                  if (t != NULL) {
                                      const volatile uint32_t *p = (const volatile uint32_t *)t->stack;
                                      uint32_t words = t->stack_size / 4u;
                                      while (n < words && p[n] == STACK_FILL) n++;
                                  }
                                  spinlock_release(&tasks_lock);

                                  return n * 4u;
                              }

                              uint32_t scheduler_task_count(void) {
                                  spinlock_acquire(&tasks_lock);
                                  uint32_t n = task_count;
                                  spinlock_release(&tasks_lock);
                                  return n;
                              }
