#include "src/time/time.h"
#include "src/scheduler/scheduler.h"
#include "src/scheduler/spinlock.h"
#include "src/syscall/syscall.h"
#include "src/utils/utils.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "arch/xtensa_lx6/drivers/esp32_clock/clock.h"

extern void uart_print(const char *str);

/* Pas de division 64 bits dans ce fichier : celle de libgcc est compilée
 * pour l'ABI windowed, incompatible avec le noyau (call0). D'où la
 * contrainte sur TICK_HZ, vérifiée ici à la compilation. */
typedef char tick_hz_must_divide_1000[(1000u % TICK_HZ) == 0 ? 1 : -1];

/* Marge minimale, en cycles, entre l'instant présent et la prochaine
 * échéance du timer. Une échéance programmée dans le passé ne se
 * déclencherait qu'après un tour complet du compteur (plusieurs secondes). */
#define TICK_MIN_MARGIN_CYCLES 1000u

static uint32_t cpu_hz        = CLOCK_DEFAULT_HZ;
static uint32_t tick_cycles   = CLOCK_DEFAULT_HZ / TICK_HZ;
/* Cycles par microseconde : exact, la fréquence étant un multiple du MHz. */
static uint32_t cycles_per_us = CLOCK_DEFAULT_HZ / 1000000u;

/* Compteur de ticks, avancé par le seul cœur 0. La partie basse se lit
 * sans verrou (lecture 32 bits atomique) ; le verrou garantit une lecture
 * 64 bits cohérente au moment où la partie basse reboucle. */
static volatile uint32_t ticks_lo = 0;
static volatile uint32_t ticks_hi = 0;
static spinlock_t ticks_lock = SPINLOCK_INIT;

/* ---- Initialisation ----------------------------------------------------- */

static void print_dec(uint32_t v) {
    char buf[12];
    uart_print(utoa_base(v, buf, 10));
}

void time_init(void) {
    uint32_t hz = clock_measure_cpu_hz("[temps] mesure de l'horloge CPU ");

    if (hz == 0) {
        hz = CLOCK_DEFAULT_HZ;
        uart_print(" echec, valeur par defaut : ");
    } else {
        uart_print(" ");
    }

    cpu_hz = hz;
    tick_cycles = hz / TICK_HZ;
    cycles_per_us = hz / 1000000u;
    if (cycles_per_us == 0) cycles_per_us = 1;

    print_dec(hz / 1000000u);
    uart_print(" MHz, tick a ");
    print_dec(TICK_HZ);
    uart_print(" Hz\n");
}

void time_start_this_core(void) {
    cpu_write_ccompare0(cpu_read_ccount() + tick_cycles);
    interrupts_enable_line(TIMER0_IRQ_LINE);
}

/* ---- Interruption du tick ----------------------------------------------- */

uint32_t *time_tick_interrupt(uint32_t *sp) {
    /* Échéance suivante = précédente + une période, et non "maintenant +
     * une période" : la latence de traitement ne s'accumule pas, le tick
     * ne dérive pas. Si des échéances ont été manquées (interruptions
     * masquées longtemps), on les saute et on les compte quand même, pour
     * que le compteur reste fidèle au temps réel. */
    uint32_t next = cpu_read_ccompare0();
    uint32_t elapsed = 0;
    do {
        next += tick_cycles;
        elapsed++;
    } while ((int32_t)(next - cpu_read_ccount()) < (int32_t)TICK_MIN_MARGIN_CYCLES);

    cpu_write_ccompare0(next);   /* acquitte aussi l'interruption */

    if (cpu_core_id() == 0) {
        spinlock_acquire(&ticks_lock);
        uint32_t lo = ticks_lo + elapsed;
        if (lo < ticks_lo) ticks_hi = ticks_hi + 1u;
        ticks_lo = lo;
        spinlock_release(&ticks_lock);

        scheduler_wake_sleepers(lo);
    }

    return schedule_next_task(sp);
}

/* ---- Lecture du temps --------------------------------------------------- */

uint32_t tick_count(void) {
    return ticks_lo;
}

uint64_t tick_count64(void) {
    spinlock_acquire(&ticks_lock);
    uint64_t t = ((uint64_t)ticks_hi << 32) | ticks_lo;
    spinlock_release(&ticks_lock);
    return t;
}

uint64_t time_uptime_ms(void) {
    return tick_count64() * TICK_MS;
}

uint32_t time_cpu_hz(void) {
    return cpu_hz;
}

uint32_t time_ms_to_ticks(uint32_t ms) {
    uint32_t t = ms / TICK_MS + (ms % TICK_MS != 0 ? 1u : 0u);
    return t > TIME_MAX_SLEEP_TICKS ? TIME_MAX_SLEEP_TICKS : t;
}

/* ---- Sommeil ------------------------------------------------------------ */

/* Bloquer n'est permis que depuis une tâche, hors interruption (EXCM) et
 * sans spinlock tenu (niveau d'interruption relevé). */
static int can_block(void) {
    uint32_t ps = cpu_read_ps();
    return (ps & PS_EXCM_MASK) == 0 && (ps & PS_INTLEVEL_MASK) == 0;
}

static int block_until(uint32_t wake_tick) {
    if (!can_block()) return -1;

    task_t *me = scheduler_sleep_prepare(wake_tick);
    if (me == NULL) return -1;

    /* Céder le processeur tout de suite : la tâche étant bloquée, elle ne
     * sera réélue qu'après son réveil par le tick. La boucle couvre un
     * réveil survenu avant même que l'appel système ait eu lieu. */
    while (me->state == TASK_BLOCKED) {
        sys_yield();
    }
    return 0;
}

int sleep_ticks(uint32_t n) {
    if (scheduler_current_task() == NULL) {
        while (n-- > 0) delay_ms(TICK_MS);
        return 0;
    }
    if (!can_block()) return -1;

    if (n == 0) {
        sys_yield();
        return 0;
    }
    if (n > TIME_MAX_SLEEP_TICKS) n = TIME_MAX_SLEEP_TICKS;

    return block_until(tick_count() + n);
}

int sleep_ms(uint32_t ms) {
    if (ms == 0) return sleep_ticks(0);

    /* Un tick de plus que la durée arrondie : l'appel tombe n'importe où
     * dans la période en cours, ce tick supplémentaire garantit au moins
     * ms millisecondes. */
    uint32_t n = time_ms_to_ticks(ms);
    if (n < TIME_MAX_SLEEP_TICKS) n++;

    if (scheduler_current_task() == NULL) {
        delay_ms(ms);
        return 0;
    }
    return sleep_ticks(n);
}

int sleep_until(uint32_t *last_wake, uint32_t period) {
    if (last_wake == NULL || scheduler_current_task() == NULL) return -1;
    if (period > TIME_MAX_SLEEP_TICKS) period = TIME_MAX_SLEEP_TICKS;

    uint32_t wake = *last_wake + period;
    *last_wake = wake;

    /* Échéance déjà passée : pas de sommeil, on rattrape le retard. */
    if ((int32_t)(wake - tick_count()) <= 0) return can_block() ? 0 : -1;

    return block_until(wake);
}

/* ---- Attente active ----------------------------------------------------- */

void delay_us(uint32_t us) {
    /* Par tranches d'au plus 2^30 cycles : une différence de CCOUNT reste
     * juste tant que l'attente tient sur moins d'un tour du compteur, et
     * chunk * cycles_per_us tient sur 32 bits. */
    const uint32_t max_chunk_us = 0x40000000u / cycles_per_us;

    while (us > 0) {
        uint32_t chunk_us = us > max_chunk_us ? max_chunk_us : us;
        uint32_t cycles = chunk_us * cycles_per_us;
        uint32_t start = cpu_read_ccount();
        while ((uint32_t)(cpu_read_ccount() - start) < cycles) { }
        us -= chunk_us;
    }
}

void delay_ms(uint32_t ms) {
    while (ms > 0) {
        uint32_t chunk = ms > 1000000u ? 1000000u : ms;   /* reste sous 2^32 µs */
        delay_us(chunk * 1000u);
        ms -= chunk;
    }
}
