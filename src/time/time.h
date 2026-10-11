#ifndef TIME_H
#define TIME_H

#include "src/types.h"

/* ---------------------------------------------------------------------------
 * Temps du noyau : compteur de ticks, sommeil et attente active.
 *
 * Le tick est une interruption périodique (timer 0 de chaque cœur, TICK_HZ
 * fois par seconde). Elle fait avancer le compteur de ticks, réveille les
 * tâches dont le sommeil est écoulé et déclenche l'ordonnancement.
 *
 * Deux familles d'attente :
 *   - sleep_* : la tâche DORT, le processeur exécute les autres tâches.
 *     Résolution : un tick. À utiliser dans les tâches.
 *   - delay_* : attente ACTIVE sur le compteur de cycles, précise à la
 *     microseconde, mais qui monopolise le cœur. Pour les drivers et les
 *     délais très courts, ou avant le démarrage de l'ordonnanceur.
 * ------------------------------------------------------------------------- */

/* Fréquence du tick. Chaque tick est aussi un point de préemption : plus
 * elle est haute, plus le sommeil est précis, mais plus l'ordonnanceur
 * coûte. 1000 Hz donne une résolution d'une milliseconde. Peut être
 * redéfinie à la compilation (-DTICK_HZ=100u). */
#ifndef TICK_HZ
#define TICK_HZ 1000u
#endif

/* TICK_HZ doit diviser 1000 (1000, 500, 250, 200, 100...) : les
 * conversions se font alors sans division 64 bits. */
#define TICK_MS (1000u / TICK_HZ)   /* millisecondes par tick */

/* Durée maximale d'un sommeil, en ticks (~12 jours à 1000 Hz). Au-delà,
 * la durée est ramenée à cette valeur. */
#define TIME_MAX_SLEEP_TICKS 0x3FFFFFFFu

/* ---- Initialisation (noyau) --------------------------------------------- */

/* Mesure la fréquence CPU et calcule la période du tick. À appeler une
 * fois au démarrage, sur le cœur 0, AVANT de démarrer le second cœur et
 * l'ordonnanceur. Affiche la fréquence retenue. */
void time_init(void);

/* Arme le tick sur le cœur appelant (appelée par scheduler_start). */
void time_start_this_core(void);

/* Gestionnaire du tick, appelé par l'interruption timer 0 avec le contexte
 * sauvegardé ; retourne le contexte à restaurer. */
uint32_t *time_tick_interrupt(uint32_t *sp);

/* ---- Lecture du temps ---------------------------------------------------- */

/* Ticks écoulés depuis le démarrage de l'ordonnanceur. Reboucle tous les
 * 2^32 ticks (~49 jours à 1000 Hz) : comparer par différence. */
uint32_t tick_count(void);

/* Même compteur sur 64 bits, sans rebouclage. */
uint64_t tick_count64(void);

/* Millisecondes écoulées depuis le démarrage de l'ordonnanceur. */
uint64_t time_uptime_ms(void);

/* Fréquence CPU retenue par time_init, en Hz. */
uint32_t time_cpu_hz(void);

/* Conversion milliseconde -> ticks, arrondie au tick SUPÉRIEUR et bornée
 * à TIME_MAX_SLEEP_TICKS. */
uint32_t time_ms_to_ticks(uint32_t ms);

/* ---- Sommeil (tâches uniquement) -----------------------------------------
 * Retournent 0, ou -1 si l'appel est fait depuis une interruption ou avec
 * un spinlock tenu (la tâche ne dort alors pas). Une durée nulle cède
 * simplement le processeur. */

/* Dort au moins ms millisecondes. Avant le démarrage de l'ordonnanceur
 * (aucune tâche), se rabat sur une attente active. */
int sleep_ms(uint32_t ms);

/* Dort jusqu'au n-ième tick à venir : entre n-1 et n périodes de tick
 * selon l'instant de l'appel dans la période en cours. */
int sleep_ticks(uint32_t n);

/* Sommeil périodique sans dérive : dort jusqu'à *last_wake + period, puis
 * met à jour *last_wake. Si l'échéance est déjà passée (la tâche a pris du
 * retard), retourne aussitôt. Initialiser *last_wake avec tick_count().
 * Retourne -1 hors tâche. */
int sleep_until(uint32_t *last_wake, uint32_t period);

/* ---- Attente active ------------------------------------------------------ */

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

#endif /* TIME_H */
