#ifndef RAM_FS_H
#define RAM_FS_H

#include "src/types.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"
#include "src/scheduler/spinlock.h"

#define RAM_FS_MAX_NAME 32

typedef enum {
    RAM_FS_TYPE_FILE   = 0,
    RAM_FS_TYPE_DIR    = 1,
} ram_fs_type_t;

typedef struct ram_fs_node {
    char name[RAM_FS_MAX_NAME];
    ram_fs_type_t type;
    struct ram_fs_node *parent;
    struct ram_fs_node *next_sibling;

    struct ram_fs_node *first_child;

    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
} ram_fs_node_t;

extern void uart_print(const char *str);
extern void uart_print_hex(uint32_t val);

void ram_fs_init(void);

/* Accès aux fichiers par chemin, sans ouverture ni position : un fichier
 * se lit et s'écrit en entier.
 *
 * ram_fs_read : copie tout le fichier dans buf. Retourne sa taille, ou -1
 *   si le chemin n'est pas un fichier ou si len est plus petit que le
 *   fichier (pas de lecture partielle).
 * ram_fs_write : remplace tout le contenu du fichier par les len octets
 *   de buf. Le fichier est créé s'il n'existe pas (son dossier doit
 *   exister) ; len = 0 le vide. Retourne len, ou -1.
 * ram_fs_append : ajoute len octets à la fin (fichier créé s'il n'existe
 *   pas). Retourne len, ou -1.
 * ram_fs_truncate : fixe la taille du fichier (agrandi avec des zéros).
 *
 * Mémoire : le tampon d'un fichier est toujours le plus petit bloc de
 * l'allocateur pouvant le contenir (multiple de 128 octets, en-tête
 * compris). Il est réalloué à la bonne taille quand le fichier grandit ou
 * rétrécit, et libéré quand le fichier devient vide. Un ajout qui ne tient
 * plus dans le bloc recopie tout le fichier : pour de nombreux petits
 * ajouts sur un gros fichier, mieux vaut regrouper les données.
 * ram_fs_stat : taille et nature d'une entrée ; 0, ou -1 si absente. */
int ram_fs_read(const char *path, void *buf, uint32_t len);
int ram_fs_write(const char *path, const void *buf, uint32_t len);
int ram_fs_append(const char *path, const void *buf, uint32_t len);
int ram_fs_truncate(const char *path, uint32_t size);
int ram_fs_stat(const char *path, uint32_t *out_size, int *out_is_dir);

int ram_fs_unlink(const char *path);

int ram_fs_mkdir(const char *path);
int ram_fs_mkdir_p(const char *path);
int ram_fs_rmdir(const char *path);

int ram_fs_is_dir(const char *path);
int ram_fs_exists(const char *path);
int ram_fs_ls(const char *path);

void ram_fs_print_tree(void);

#endif /* RAM_FS_H */
