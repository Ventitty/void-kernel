#ifndef FAT32_H
#define FAT32_H

#include "src/types.h"
#include "arch/xtensa_lx6/drivers/esp32_sdcard/sdcard.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"
#include "src/scheduler/spinlock.h"

/* ---------------------------------------------------------------------------
 * FAT32 sur carte SD : lecture et écriture.
 *
 * Écriture directe : chaque modification part immédiatement sur la carte,
 * y compris la taille du fichier dans son entrée de dossier à chaque
 * fat32_write. Une coupure de courant laisse au pire un fichier tronqué,
 * jamais une structure incohérente. Le compteur d'espace libre (secteur
 * FSInfo) est tenu à jour s'il était valide au montage.
 *
 * Noms : format court 8.3 uniquement (8 caractères, un point, 3 caractères,
 * insensible à la casse). Un nom entièrement en minuscules est conservé en
 * minuscules (indicateurs de casse FAT, respectés par Linux et Windows).
 * Les fichiers créés sur PC avec un nom long restent accessibles par leur
 * nom court (ex. "LONGFI~1.TXT") ; les supprimer efface aussi leur nom long.
 *
 * Concurrence : toutes les opérations sont sérialisées par un verrou. Deux
 * fichiers ouverts sur le même fichier ne voient pas les changements de
 * taille de l'autre.
 * ------------------------------------------------------------------------- */

#define FAT32_EOC_MIN      0x0FFFFFF8u
#define FAT32_EOC          0x0FFFFFFFu
#define FAT32_BAD_CLUSTER  0x0FFFFFF7u
#define FAT32_CLUSTER_MASK 0x0FFFFFFFu

#define DIRENT_SIZE     32u
#define DIRENT_FREE     0x00u
#define DIRENT_DELETED  0xE5u
#define ATTR_READ_ONLY  0x01u
#define ATTR_HIDDEN     0x02u
#define ATTR_SYSTEM     0x04u
#define ATTR_VOLUME_ID  0x08u
#define ATTR_DIRECTORY  0x10u
#define ATTR_ARCHIVE    0x20u
#define ATTR_LFN        0x0Fu

#define FAT32_SEEK_SET 0
#define FAT32_SEEK_CUR 1
#define FAT32_SEEK_END 2

/* Options d'ouverture (mêmes valeurs que VFS_O_*). */
#define FAT32_O_READ   0x01
#define FAT32_O_WRITE  0x02
#define FAT32_O_CREATE 0x04
#define FAT32_O_TRUNC  0x08
#define FAT32_O_APPEND 0x10

/* Codes de retour (négatifs) des opérations. */
#define FAT32_OK              0
#define FAT32_ERR_IO         -1   /* erreur de lecture/écriture de la carte */
#define FAT32_ERR_NOENT      -2   /* n'existe pas                           */
#define FAT32_ERR_EXIST      -3   /* existe déjà                            */
#define FAT32_ERR_NOSPC      -4   /* carte pleine                           */
#define FAT32_ERR_INVAL      -5   /* nom ou argument invalide               */
#define FAT32_ERR_NOTEMPTY   -6   /* dossier non vide                       */
#define FAT32_ERR_ISDIR      -7   /* est un dossier                         */
#define FAT32_ERR_NOTDIR     -8   /* n'est pas un dossier                   */
#define FAT32_ERR_BADF       -9   /* fichier non ouvert pour cette opération */
#define FAT32_ERR_NOTMOUNTED -10  /* aucun volume monté                     */

struct volume {
    int mounted;
    uint32_t volume_start_lba;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t total_clusters;

    /* Écriture */
    uint32_t active_fat;        /* FAT lue (miroir désactivé), sinon 0   */
    int      fat_mirrored;      /* 1 : écrire toutes les copies de la FAT */
    uint32_t fsinfo_lba;        /* 0 si pas de secteur FSInfo valide      */
    uint32_t free_count;        /* 0xFFFFFFFF : inconnu, non tenu à jour  */
    uint32_t next_free;         /* indice de départ de la recherche       */
    int      fsinfo_dirty;
};

struct fat32_file {
    uint32_t first_cluster;
    uint32_t size;
    uint32_t pos;
    int      flags;              /* FAT32_O_*                              */

    uint32_t cached_cluster_index;
    uint32_t cached_cluster;

    /* Emplacement de l'entrée de dossier, mise à jour à chaque écriture. */
    uint32_t dirent_lba;
    uint32_t dirent_off;
};

typedef struct fat32_file fat32_file_t;

extern void uart_print(const char *str);
extern void uart_print_hex(uint32_t val);

int fat32_mount(void);
int fat32_is_mounted(void);
void fat32_print_info(void);

/* Taille et nature d'une entrée. 0, ou FAT32_ERR_*. */
int fat32_stat(const char *path, uint32_t *out_size, int *out_is_dir);

/* Ouvre un fichier (jamais un dossier). flags : FAT32_O_*. Avec
 * FAT32_O_CREATE, le fichier est créé s'il n'existe pas ; FAT32_O_TRUNC
 * le vide (s'il est ouvert en écriture) ; FAT32_O_APPEND place chaque
 * écriture en fin de fichier. NULL en cas d'échec. */
fat32_file_t *fat32_open(const char *path, int flags);
void fat32_close(fat32_file_t *file);

/* Octets lus (0 en fin de fichier), ou FAT32_ERR_*. */
int  fat32_read(fat32_file_t *file, void *buf, uint32_t len);

/* Octets écrits, ou FAT32_ERR_*. Écrire après la fin du fichier (après un
 * seek) remplit l'intervalle de zéros. Une écriture partielle (carte
 * pleine en cours de route) retourne le nombre d'octets effectivement
 * écrits. */
int  fat32_write(fat32_file_t *file, const void *buf, uint32_t len);

int  fat32_seek(fat32_file_t *file, int32_t offset, int whence);
uint32_t fat32_size(fat32_file_t *file);

/* Supprime un fichier. 0, ou FAT32_ERR_* (ISDIR pour un dossier). */
int fat32_unlink(const char *path);

/* Crée un dossier (le parent doit exister). 0, ou FAT32_ERR_*. */
int fat32_mkdir(const char *path);

/* Supprime un dossier vide. 0, ou FAT32_ERR_* (NOTEMPTY s'il ne l'est pas). */
int fat32_rmdir(const char *path);

/* Affiche le contenu d'un dossier sur la console. 0, ou FAT32_ERR_*. */
int fat32_ls(const char *path);

#endif /* FAT32_H */
