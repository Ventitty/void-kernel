#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "src/types.h"
#include "src/interrupts/interrupts.h"
#include "src/memory_manager/memory.h"
#include "src/scheduler/spinlock.h"

#define STACK_FILL        0xA5A5A5A5u
#define STACK_GUARD_BYTES 32u

/* Zones mémoire qu'une tâche utilisateur peut désigner dans ses appels
 * système : sa pile (toujours la première) + celles données à sa création. */
#define TASK_MAX_MEM_RANGES 4

struct task_block;   /* src/memory_manager/task_memory.h */

typedef struct {
    uint32_t start;      /* incluse */
    uint32_t end;        /* exclue  */
    uint8_t  writable;
} task_mem_range_t;

typedef enum {
    TASK_READY = 0,   /* prête, en attente d'un cœur                        */
    TASK_RUNNING,     /* en cours d'exécution sur un cœur                   */
    TASK_BLOCKED,     /* en attente d'une primitive (mutex, sémaphore...)   */
    TASK_ZOMBIE,      /* terminée, mémoire libérée au prochain changement   */
} task_state_t;

/* Descripteur de tâche, alloué sur le tas à la création (nmap) et libéré à
 * sa terminaison : le nombre de tâches n'est limité que par la mémoire. */
typedef struct task {
    uint32_t     *sp;            /* contexte sauvegardé (cpu_context_t)     */
    void         *stack;         /* bas de la pile (bloc nmap)              */
    uint32_t      stack_size;
    volatile task_state_t state;
    uint8_t       pinned_core;   /* 0, 1 ou TASK_ANY_CORE                   */
    uint8_t       running_on;    /* cœur qui l'exécute, ou TASK_ANY_CORE    */
    int           id;            /* identifiant unique, jamais réutilisé    */

    struct task  *next;          /* liste globale des tâches ordonnançables */

    /* File d'attente d'une primitive de synchronisation. Une tâche bloquée
     * n'attend qu'une seule chose à la fois : un seul maillon suffit. Ces
     * champs appartiennent à la primitive et sont protégés par SON verrou. */
    struct task  *wait_next;
    uint32_t      wait_mask;     /* event group : bits attendus             */
    uint8_t       wait_all;      /* event group : tous les bits ou un seul  */

    /* Sommeil : la tâche est dans la liste des dormeurs (triée par
     * wake_tick) tant que sleeping vaut 1. Protégé par le verrou de
     * l'ordonnanceur. */
    struct task  *sleep_next;
    uint32_t      wake_tick;
    uint8_t       sleeping;

    /* Zones autorisées pour les pointeurs des appels système (tâches
     * utilisateur). Fixées à la création, jamais modifiées ensuite. */
    task_mem_range_t mem[TASK_MAX_MEM_RANGES];
    uint8_t          mem_count;

    /* Blocs alloués par VOID_SYS_MEM_ALLOC, rendus à la terminaison. */
    struct task_block *blocks;
} task_t;

/* ---- Files d'attente (FIFO) des primitives de synchronisation ----------
 * À manipuler uniquement sous le verrou de la primitive propriétaire. */

typedef struct {
    task_t *head;
    task_t *tail;
} wait_queue_t;

#define WAIT_QUEUE_INIT { NULL, NULL }

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

/* Détache toute la file et retourne sa tête (chaînée par wait_next). Lire
 * t->wait_next AVANT de réveiller t : une fois réveillée, la tâche peut se
 * remettre en attente ailleurs et réécrire ce champ. */
static inline task_t *wait_queue_take_all(wait_queue_t *q) {
    task_t *t = q->head;
    q->head = NULL;
    q->tail = NULL;
    return t;
}

/* ---- Création ----------------------------------------------------------
 * Retournent l'identifiant de la tâche, ou -1 si la mémoire manque. */

void scheduler_init(void);
int task_create(void (*entry)(void));
int task_create_pinned(void (*entry)(void), uint8_t core);
int task_create_user(void (*entry)(void));
int task_create_user_pinned(void (*entry)(void), uint8_t core);

/* Tâche utilisateur avec des zones mémoire supplémentaires (au plus
 * TASK_MAX_MEM_RANGES - 1, la pile occupant la première place), par
 * exemple les données d'un programme chargé (elf_image_t.data_*), pour
 * que ses chaînes et tampons soient acceptés par les appels système. */
int task_create_user_mem(void (*entry)(void), const task_mem_range_t *ranges, uint32_t count);

void scheduler_start(void);
uint32_t *schedule_next_task(uint32_t *current_sp);
uint32_t *scheduler_terminate_current(uint32_t *sp);

/* ---- Tâche courante et blocage ----------------------------------------- */

uint32_t get_core_id(void);
task_t  *scheduler_current_task(void);      /* NULL hors tâche (boot)       */
int      scheduler_current_task_id(void);   /* -1 hors tâche                */

/* Protocole de blocage d'une primitive :
 *   1. sous le verrou de la primitive : wait_queue_push() puis
 *      scheduler_mark_blocked_self() ;
 *   2. relâcher le verrou ;
 *   3. scheduler_yield_blocked(me) : attend que quelqu'un appelle
 *      scheduler_unblock(me). */
task_t *scheduler_mark_blocked_self(void);
void    scheduler_yield_blocked(task_t *me);
void    scheduler_block_current(void);
void    scheduler_unblock(task_t *t);

/* ---- Sommeil (utilisé par src/time) -------------------------------------
 * Les instants sont des valeurs du compteur de ticks, comparées par
 * différence signée : deux échéances doivent être à moins de 2^31 ticks
 * l'une de l'autre (src/time borne les durées en conséquence). */

/* Met la tâche courante en sommeil jusqu'au tick wake_tick : elle est
 * bloquée et inscrite dans la liste des dormeurs, mais continue de
 * s'exécuter jusqu'à ce qu'elle cède le processeur. Retourne la tâche, ou
 * NULL hors tâche. */
task_t *scheduler_sleep_prepare(uint32_t wake_tick);

/* Même chose depuis le gestionnaire d'un appel système : endort la tâche
 * interrompue et bascule immédiatement vers une autre. sp est le contexte
 * sauvegardé ; retourne celui à restaurer. */
uint32_t *scheduler_sleep_from_syscall(uint32_t *sp, uint32_t wake_tick);

/* Réveille toutes les tâches dont l'échéance est atteinte à l'instant now.
 * Appelée par l'interruption du tick. */
void scheduler_wake_sleepers(uint32_t now);

/* Octets de pile jamais utilisés par la tâche (0 si id inconnu). */
uint32_t scheduler_stack_unused(int task_id);
/* Nombre de tâches existantes (hors tâches idle). */
uint32_t scheduler_task_count(void);

#endif /* SCHEDULER_H */
