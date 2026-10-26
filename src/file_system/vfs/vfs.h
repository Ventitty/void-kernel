#ifndef VFS_H
#define VFS_H

#include "src/types.h"
#include "src/file_system/ramfs/ramfs.h"
#include "src/file_system/fat32/fat32.h"
#include "arch/xtensa_lx6/drivers/esp32_sdcard/sdcard.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"

/* Longueur maximale d'un chemin pour vfs_mkdir_p sur la carte SD. */
#define VFS_PATH_MAX 256

/* Codes retour de vfs_mount_sd(). */
#define VFS_SD_OK      0
#define VFS_SD_NO_CARD (-1)   /* la carte ne répond pas */
#define VFS_SD_BAD_FS  (-2)   /* carte présente mais pas en FAT32 */

#define VFS_DEV_NAME_MAX 32

/* Fonctions d'un périphérique : mêmes signatures que uart0_read et
 * uart0_write, pour pouvoir les brancher sans adaptateur. Retournent le
 * nombre d'octets traités, ou -1 en cas d'erreur. */
typedef int (*vfs_dev_read_t)(void *buf, uint32_t len);
typedef int (*vfs_dev_write_t)(const void *buf, uint32_t len);

typedef struct vfs_dev {
    char             name[VFS_DEV_NAME_MAX];
    vfs_dev_read_t   read;
    vfs_dev_write_t  write;
    struct vfs_dev  *next;
} vfs_dev_t;

/* Initialise ramfs et crée /dev et /tmp. À appeler une fois au boot. */
int  vfs_init(void);

/* Initialise la carte SD puis monte son volume FAT32 comme racine. */
int  vfs_mount_sd(uint8_t sclk, uint8_t mosi, uint8_t miso, uint8_t cs);
int  vfs_sd_mounted(void);

/* ---- Fichiers : accès par chemin, sans ouverture ------------------------
 *
 * Pas de position : un fichier se lit et s'écrit en entier.
 *
 * vfs_read : lit TOUT le fichier dans buf. Échoue (-1) si len est plus
 *   petit que le fichier : pas de lecture partielle. vfs_stat donne la
 *   taille à prévoir. Retourne la taille du fichier (0 s'il est vide).
 * vfs_write : le contenu du fichier devient exactement les len octets de
 *   buf ; l'ancien contenu est remplacé et la fin en trop coupée (len = 0
 *   vide le fichier). Le fichier est créé s'il
 *   n'existe pas (son dossier doit exister). Retourne len, ou -1 (carte
 *   pleine : le fichier ne contient alors que le début de buf).
 * vfs_append : ajoute len octets à la fin du fichier (créé s'il n'existe
 *   pas). Octets écrits, ou -1.
 * vfs_truncate : fixe la taille du fichier (agrandi avec des zéros). 0/-1.
 *
 * Sur un périphérique de /dev, vfs_read, vfs_write et vfs_append appellent
 * directement ses fonctions : vfs_read rend ce qui est disponible
 * (éventuellement 0), vfs_write et vfs_append envoient len octets. */
int vfs_read(const char *path, void *buf, uint32_t len);
int vfs_write(const char *path, const void *buf, uint32_t len);
int vfs_append(const char *path, const void *buf, uint32_t len);
int vfs_truncate(const char *path, uint32_t size);

/* Arborescence. */
int vfs_stat(const char *path, uint32_t *out_size, int *out_is_dir);
int vfs_exists(const char *path);
int vfs_is_dir(const char *path);
int vfs_mkdir(const char *path);
int vfs_mkdir_p(const char *path);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
/* Chemins complets de tout ce que contient le dossier path (fichiers,
 * dossiers, périphériques ; pour "/" aussi les points de montage /dev et
 * /tmp), dans *out : tableau de chaînes terminé par NULL, en un seul bloc,
 * à libérer avec path_list_free. Retourne leur nombre, ou -1. */
int vfs_ls(const char *path, char ***out);

/* Crée sous /dev un fichier dont vfs_read et vfs_write appellent read et
 * write. L'un des deux peut être NULL (périphérique en lecture ou en
 * écriture seule). Échoue si le chemin n'est pas dans /dev ou existe déjà.
 * Le VFS ne fait que relayer : il ne connaît pas le matériel, c'est
 * l'appelant (typiquement kernel_main) qui choisit les fonctions.
 *
 * Les périphériques sont tenus par le VFS lui-même (ramfs n'est pas
 * concerné) et sont permanents : vfs_unlink les refuse. vfs_truncate y
 * échoue, et vfs_stat leur donne une taille nulle. */
int vfs_mknod(const char *path, vfs_dev_read_t read, vfs_dev_write_t write);

extern void uart_print(const char *str);

#endif /* VFS_H */
