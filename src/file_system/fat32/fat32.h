#ifndef FAT32_H
#define FAT32_H

#include "src/types.h"
#include "arch/xtensa_lx6/drivers/esp32_sdcard/sdcard.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"
#include "src/scheduler/spinlock.h"
#include "src/file_system/path_list/path_list.h"

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
 * Accès par chemin, sans ouverture ni position : un fichier se lit et
 * s'écrit en entier (fat32_read, fat32_write) ; fat32_append ajoute à la
 * fin. Le dernier fichier accédé est gardé en cache.
 *
 * Concurrence : toutes les opérations sont sérialisées par un verrou.
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
#define FAT32_ERR_NOTMOUNTED -10  /* aucun volume monté                     */
#define FAT32_ERR_TOOBIG     -11  /* fichier plus grand que le tampon       */

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


extern void uart_print(const char *str);
extern void uart_print_hex(uint32_t val);

int fat32_mount(void);
int fat32_is_mounted(void);
void fat32_print_info(void);

/* Taille et nature d'une entrée. 0, ou FAT32_ERR_*. */
int fat32_stat(const char *path, uint32_t *out_size, int *out_is_dir);

/* Lit tout le fichier dans buf. Retourne sa taille, ou FAT32_ERR_* :
 * TOOBIG si len est plus petit que le fichier (pas de lecture partielle),
 * ISDIR pour un dossier. */
int fat32_read(const char *path, void *buf, uint32_t len);

/* Remplace tout le contenu du fichier par les len octets de buf : l'ancien
 * contenu est réécrit puis la fin en trop coupée. Le fichier est créé s'il
 * n'existe pas (son dossier doit exister) ; len = 0 le vide. Retourne len,
 * ou FAT32_ERR_* (NOSPC : carte pleine, le fichier ne contient alors que
 * le début de buf). */
int fat32_write(const char *path, const void *buf, uint32_t len);

/* Ajoute len octets à la fin du fichier (créé s'il n'existe pas).
 * Retourne len, ou FAT32_ERR_* (NOSPC : seul le début a été ajouté). */
int fat32_append(const char *path, const void *buf, uint32_t len);

/* Fixe la taille d'un fichier : les clusters au-delà sont libérés, ou le
 * fichier est agrandi avec des zéros. 0, ou FAT32_ERR_*. */
int fat32_truncate(const char *path, uint32_t size);

/* Supprime un fichier. 0, ou FAT32_ERR_* (ISDIR pour un dossier). */
int fat32_unlink(const char *path);

/* Crée un dossier (le parent doit exister). 0, ou FAT32_ERR_*. */
int fat32_mkdir(const char *path);

/* Supprime un dossier vide. 0, ou FAT32_ERR_* (NOTEMPTY s'il ne l'est pas). */
int fat32_rmdir(const char *path);

/* Chemins complets des entrées (fichiers et dossiers, sans "." ni "..")
 * du dossier path, dans *out (voir path_list.h, à libérer avec
 * path_list_free). Les noms sont les noms courts 8.3. Retourne leur
 * nombre, ou FAT32_ERR_* (NOTDIR, NOENT... ; NOSPC si la mémoire manque). */
int fat32_ls(const char *path, char ***out);

#endif /* FAT32_H */
