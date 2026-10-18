#ifndef VFS_H
#define VFS_H

#include "src/types.h"
#include "src/file_system/ramfs/ramfs.h"
#include "src/file_system/fat32/fat32.h"
#include "arch/xtensa_lx6/drivers/esp32_sdcard/sdcard.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"

#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_CREATE 0x04
#define VFS_O_TRUNC  0x08
#define VFS_O_APPEND 0x10

/* Longueur maximale d'un chemin pour vfs_mkdir_p sur la carte SD. */
#define VFS_PATH_MAX 256

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

/* Codes retour de vfs_mount_sd(). */
#define VFS_SD_OK      0
#define VFS_SD_NO_CARD (-1)   /* la carte ne répond pas */
#define VFS_SD_BAD_FS  (-2)   /* carte présente mais pas en FAT32 */

#define VFS_DEV_NAME_MAX 32

typedef struct vfs_file vfs_file_t;

/* Fonctions d'un périphérique : mêmes signatures que uart0_read et
 * uart0_write, pour pouvoir les brancher sans adaptateur. Retournent le
 * nombre d'octets traités, ou -1 en cas d'erreur. */
typedef int (*vfs_dev_read_t)(void *buf, uint32_t len);
typedef int (*vfs_dev_write_t)(const void *buf, uint32_t len);

typedef enum {
    VFS_BACKEND_RAM   = 0,
    VFS_BACKEND_FAT32 = 1,
    VFS_BACKEND_DEV   = 2,
} vfs_backend_t;

struct vfs_file {
    vfs_backend_t backend;
    void         *handle;   /* ram_fs_file_t*, fat32_file_t* ou vfs_dev_t* */
};

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

/* Fichiers. */
vfs_file_t *vfs_open(const char *path, int flags);
void     vfs_close(vfs_file_t *file);
int      vfs_read(vfs_file_t *file, void *buf, uint32_t len);
int      vfs_write(vfs_file_t *file, const void *buf, uint32_t len);
int      vfs_seek(vfs_file_t *file, int32_t offset, int whence);
uint32_t vfs_size(vfs_file_t *file);

/* Arborescence. */
int vfs_stat(const char *path, uint32_t *out_size, int *out_is_dir);
int vfs_exists(const char *path);
int vfs_is_dir(const char *path);
int vfs_mkdir(const char *path);
int vfs_mkdir_p(const char *path);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_ls(const char *path);

/* Crée sous /dev un fichier dont vfs_read et vfs_write appellent read et
 * write. L'un des deux peut être NULL (périphérique en lecture ou en
 * écriture seule). Échoue si le chemin n'est pas dans /dev ou existe déjà.
 * Le VFS ne fait que relayer : il ne connaît pas le matériel, c'est
 * l'appelant (typiquement kernel_main) qui choisit les fonctions.
 *
 * Les périphériques sont tenus par le VFS lui-même (ramfs n'est pas
 * concerné) et sont permanents : vfs_unlink les refuse. Sur un
 * périphérique, vfs_seek échoue et vfs_size vaut 0. */
int vfs_mknod(const char *path, vfs_dev_read_t read, vfs_dev_write_t write);

extern void uart_print(const char *str);

#endif /* VFS_H */
