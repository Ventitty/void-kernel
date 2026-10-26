#ifndef PATH_LIST_H
#define PATH_LIST_H

#include "src/types.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"

/* ---------------------------------------------------------------------------
 * Liste de chemins renvoyée par les fonctions ls (ram_fs_ls, fat32_ls,
 * vfs_ls).
 *
 * Forme : un tableau de char* terminé par NULL, suivi des chaînes
 * elles-mêmes, le tout dans UN SEUL bloc alloué à la taille exacte :
 *
 *     char **liste;
 *     int n = vfs_ls("/VOID", &liste);
 *     for (int i = 0; i < n; i++) uart_print(liste[i]);   // "/VOID/LOGS", ...
 *     path_list_free(liste);
 *
 * Construction : path_list_init, puis path_list_add / path_list_add_path
 * pour chaque entrée, puis path_list_finish qui produit la liste. Les
 * chaînes sont accumulées dans un tampon temporaire, libéré par
 * path_list_finish dans tous les cas. Un manque de mémoire en cours de
 * route est retenu et signalé par path_list_finish.
 * ------------------------------------------------------------------------- */

typedef struct {
    char    *buf;      /* chaînes accumulées, chacune terminée par '\0' */
    uint32_t used;
    uint32_t cap;
    uint32_t count;
    int      failed;   /* mémoire insuffisante pendant la construction  */
} path_list_t;

void path_list_init(path_list_t *pl);

/* Ajoute dir + "/" + name (name_len caractères). Pas de "/" doublé si dir
 * se termine déjà par "/" (cas de la racine). */
void path_list_add(path_list_t *pl, const char *dir, const char *name, uint32_t name_len);

/* Ajoute un chemin complet tel quel. */
void path_list_add_path(path_list_t *pl, const char *path);

/* Produit la liste dans *out. Retourne le nombre de chemins, ou -1 si la
 * mémoire a manqué (*out vaut alors NULL). Libère le tampon temporaire. */
int path_list_finish(path_list_t *pl, char ***out);

/* Abandonne une construction (erreur en cours de route). */
void path_list_discard(path_list_t *pl);

/* Libère une liste renvoyée par un ls. NULL accepté. */
void path_list_free(char **list);

#endif /* PATH_LIST_H */
