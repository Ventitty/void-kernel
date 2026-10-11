#ifndef CPU_H
#define CPU_H

#include "src/types.h"

/* ---------------------------------------------------------------------------
 * Primitives CPU Xtensa LX6.
 *
 * Tout l'assembleur inline du noyau vit ici : le code de src/ n'appelle que
 * ces fonctions et ne contient aucune instruction Xtensa. Un portage vers une
 * autre architecture consiste à fournir le même jeu de fonctions.
 *
 * always_inline : le noyau est compilé sans -O, et GCC n'intègre alors jamais
 * un simple "static inline". L'attribut garantit qu'un appel à cpu_wait_irq()
 * coûte exactement une instruction waiti, pas un appel de fonction.
 *
 * Les fonctions qui modifient l'état visible par la mémoire (PS, barrières)
 * portent le clobber "memory" : le compilateur ne déplace aucun accès
 * mémoire de part et d'autre.
 * ------------------------------------------------------------------------- */

#define CPU_INLINE static inline __attribute__((always_inline))

/* ---- Identification ----------------------------------------------------- */

/* Numéro du cœur courant : 0 (PRO_CPU) ou 1 (APP_CPU). */
CPU_INLINE uint32_t cpu_core_id(void) {
    uint32_t prid;
    __asm__ volatile ("rsr.prid %0" : "=r"(prid));
    return (prid >> 13) & 1u;
}

/* ---- Registre PS et masquage des interruptions -------------------------- */

CPU_INLINE uint32_t cpu_read_ps(void) {
    uint32_t ps;
    __asm__ volatile ("rsr %0, ps" : "=r"(ps));
    return ps;
}

/* Restaure un PS sauvegardé (typiquement la valeur rendue par cpu_irq_*). */
CPU_INLINE void cpu_write_ps(uint32_t ps) {
    __asm__ volatile ("wsr %0, ps\n\trsync" :: "r"(ps) : "memory");
}

/* rsil n'accepte qu'un niveau immédiat : une fonction par niveau utilisé.
 * Chacune retourne l'ancien PS, à repasser à cpu_write_ps(). */

/* Masque toutes les interruptions (niveau 15). */
CPU_INLINE uint32_t cpu_irq_mask_all(void) {
    uint32_t ps;
    __asm__ volatile ("rsil %0, 15" : "=r"(ps) :: "memory");
    return ps;
}

/* Masque les interruptions de niveau <= 3 (niveau des spinlocks). */
CPU_INLINE uint32_t cpu_irq_mask_level3(void) {
    uint32_t ps;
    __asm__ volatile ("rsil %0, 3" : "=r"(ps) :: "memory");
    return ps;
}

/* Démasque toutes les interruptions (niveau 0). */
CPU_INLINE uint32_t cpu_irq_unmask_all(void) {
    uint32_t ps;
    __asm__ volatile ("rsil %0, 0" : "=r"(ps) :: "memory");
    return ps;
}

/* Met le cœur en veille jusqu'à la prochaine interruption. */
CPU_INLINE void cpu_wait_irq(void) {
    __asm__ volatile ("waiti 0" ::: "memory");
}

/* ---- Contrôleur d'interruptions du cœur --------------------------------- */

CPU_INLINE void cpu_set_vecbase(uint32_t addr) {
    __asm__ volatile ("wsr %0, vecbase\n\trsync" :: "r"(addr));
}

CPU_INLINE uint32_t cpu_read_intenable(void) {
    uint32_t v;
    __asm__ volatile ("rsr %0, intenable" : "=r"(v));
    return v;
}

CPU_INLINE void cpu_write_intenable(uint32_t mask) {
    __asm__ volatile ("wsr %0, intenable\n\trsync" :: "r"(mask));
}

/* Interruptions en attente (registre INTERRUPT). */
CPU_INLINE uint32_t cpu_read_pending_irq(void) {
    uint32_t v;
    __asm__ volatile ("rsr %0, interrupt" : "=r"(v));
    return v;
}

/* Acquitte les interruptions (front) dont le bit est à 1 dans mask. */
CPU_INLINE void cpu_clear_irq(uint32_t mask) {
    __asm__ volatile ("wsr %0, intclear\n\trsync" :: "r"(mask));
}

/* ---- Compteur de cycles et timer 0 du cœur ------------------------------
 * CCOUNT s'incrémente à chaque cycle CPU et reboucle tous les 2^32 cycles.
 * Quand CCOUNT atteint CCOMPARE0, l'interruption timer 0 (ligne 6) est
 * levée ; écrire CCOMPARE0 l'acquitte. Chaque cœur a ses propres
 * registres. Comparer des valeurs de CCOUNT par différence signée :
 * (int32_t)(a - b) < 0 signifie "a avant b", rebouclage compris. */

CPU_INLINE uint32_t cpu_read_ccount(void) {
    uint32_t v;
    __asm__ volatile ("rsr %0, ccount" : "=r"(v));
    return v;
}

CPU_INLINE uint32_t cpu_read_ccompare0(void) {
    uint32_t v;
    __asm__ volatile ("rsr %0, ccompare0" : "=r"(v));
    return v;
}

CPU_INLINE void cpu_write_ccompare0(uint32_t v) {
    __asm__ volatile ("wsr %0, ccompare0\n\trsync" :: "r"(v));
}

/* ---- Barrières ---------------------------------------------------------- */

/* Barrière mémoire : toutes les écritures précédentes sont terminées. */
CPU_INLINE void cpu_mem_barrier(void) {
    __asm__ volatile ("memw" ::: "memory");
}

/* À appeler après avoir écrit du code en mémoire (chargeur ELF) : vide les
 * écritures puis resynchronise le flux d'instructions. */
CPU_INLINE void cpu_sync_code(void) {
    __asm__ volatile ("memw\n\tisync" ::: "memory");
}

/* Barrière pour le compilateur seul (aucune instruction émise) : force la
 * mémoire pointée par p à être considérée comme lue. Empêche par exemple
 * l'élimination d'un memset() juste avant la fin de vie d'un buffer. */
CPU_INLINE void cpu_compiler_barrier_ptr(const void *p) {
    __asm__ volatile ("" :: "r"(p) : "memory");
}

/* ---- Atomiques (arch/xtensa_lx6/src/atomic.S) --------------------------- */

/* Compare-and-swap atomique (s32c1i) : si *ptr == expected, écrit desired.
 * Retourne toujours l'ancienne valeur de *ptr ; l'échange a eu lieu si et
 * seulement si cette valeur vaut expected. */
uint32_t cpu_atomic_cas(volatile uint32_t *ptr, uint32_t expected, uint32_t desired);

#endif /* CPU_H */
