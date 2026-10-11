#ifndef TASK_MEMORY_H
#define TASK_MEMORY_H

#include "src/types.h"
#include "src/scheduler/scheduler.h"

/* ---------------------------------------------------------------------------
 * Mémoire allouée à une tâche par l'appel système VOID_SYS_MEM_ALLOC
 * (typiquement pour le malloc de la libc).
 *
 * Chaque bloc est pris sur le tas du noyau et inscrit dans la liste de la
 * tâche (task_t.blocks). Les maillons de cette liste sont alloués à part,
 * HORS de la mémoire donnée au programme : il ne peut pas les corrompre
 * en débordant de son bloc. Tous les blocs encore alloués sont rendus à la
 * terminaison de la tâche.
 *
 * Une liste n'est modifiée que par sa tâche (depuis ses appels système)
 * puis par sa libération : aucun verrou n'est nécessaire.
 * ------------------------------------------------------------------------- */

typedef struct task_block {
    struct task_block *next;
    uint32_t           start;
    uint32_t           size;
} task_block_t;

/* Alloue size octets remis à zéro pour la tâche t. NULL si size vaut 0 ou
 * si la mémoire manque. Le tas arrondit chaque allocation à 128 octets :
 * la libc a intérêt à demander de grandes tranches et à les découper. */
void *task_mem_alloc(task_t *t, uint32_t size);

/* Rend un bloc obtenu par task_mem_alloc. 0, ou -1 si addr n'est pas le
 * début d'un bloc de cette tâche. */
int task_mem_free(task_t *t, void *addr);

/* Rend tous les blocs de la tâche (appelée à sa libération). */
void task_mem_release_all(task_t *t);

/* Si addr est dans un bloc de la tâche, écrit la fin du bloc (exclue) dans
 * *end et retourne 1 ; sinon 0. Utilisée par la vérification des
 * pointeurs. */
int task_mem_lookup(const task_t *t, uint32_t addr, uint32_t *end);

#endif /* TASK_MEMORY_H */
