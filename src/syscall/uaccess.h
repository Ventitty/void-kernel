#ifndef UACCESS_H
#define UACCESS_H

#include "src/types.h"
#include "src/scheduler/scheduler.h"

/* ---------------------------------------------------------------------------
 * Vérification des pointeurs passés aux appels système.
 *
 * Une tâche utilisateur ne peut désigner que ses propres zones : sa pile,
 * celles données à task_create_user_mem (task_t.mem) et les blocs obtenus
 * par VOID_SYS_MEM_ALLOC. Sans ce contrôle, un
 * programme ferait lire ou écrire le noyau où il veut en lui passant une
 * adresse choisie.
 *
 * Seuls les appels venant du mode utilisateur sont vérifiés : une tâche du
 * noyau a de toute façon accès à toute la mémoire.
 * ------------------------------------------------------------------------- */

/* Vrai si [addr, addr + len) tient entièrement dans UNE zone de la tâche,
 * inscriptible si write != 0. len == 0 : toujours accepté (rien ne sera lu
 * ni écrit). */
int uaccess_range_ok(const task_t *t, const void *addr, uint32_t len, int write);

/* Longueur de la chaîne s si son '\0' se trouve dans la zone qui la
 * contient et dans les max premiers octets ; -1 sinon. */
int uaccess_strlen(const task_t *t, const char *s, uint32_t max);

#endif /* UACCESS_H */
