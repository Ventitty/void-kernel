#include "src/file_system/vfs/vfs.h"

static vfs_dev_t  *dev_list = NULL;
static spinlock_t  dev_lock = SPINLOCK_INIT;

static int path_in(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static int is_valid(const char *path) {
    return path != NULL && path[0] == '/';
}

static int is_root(const char *path) {
    return path[0] == '/' && path[1] == '\0';
}

static int is_ram_path(const char *path) {
    return path_in(path, "/dev") || path_in(path, "/tmp");
}

static int is_mount_point(const char *path) {
    return strcmp(path, "/dev") == 0 || strcmp(path, "/tmp") == 0;
}

static uint32_t dev_component_len(const char *path) {
    if (strncmp(path, "/dev/", 5) != 0) return 0;
    uint32_t n = 0;
    while (path[5 + n] != '\0' && path[5 + n] != '/') n++;
    return n;
}

static vfs_dev_t *dev_lookup(const char *path, int exact) {
    uint32_t n = dev_component_len(path);
    if (n == 0 || n >= VFS_DEV_NAME_MAX) return NULL;
    if (exact && path[5 + n] != '\0') return NULL;

    spinlock_acquire(&dev_lock);
    vfs_dev_t *found = NULL;
    for (vfs_dev_t *d = dev_list; d; d = d->next) {
        if (strncmp(d->name, path + 5, n) == 0 && d->name[n] == '\0') {
            found = d;
            break;
        }
    }
    spinlock_release(&dev_lock);
    return found;
}

static int dev_shadows(const char *path) {
    return dev_lookup(path, 0) != NULL;
}

int vfs_init(void) {
    ram_fs_init();
    if (ram_fs_mkdir("/dev") != 0) return -1;
    if (ram_fs_mkdir("/tmp") != 0) return -1;
    return 0;
}

int vfs_mount_sd(uint8_t sclk, uint8_t mosi, uint8_t miso, uint8_t cs) {
    if (sd_init(sclk, mosi, miso, cs) != 0) return VFS_SD_NO_CARD;
    if (fat32_mount() != 0) return VFS_SD_BAD_FS;
    return VFS_SD_OK;
}

int vfs_sd_mounted(void) {
    return fat32_is_mounted();
}

/* Périphérique de /dev désigné par path, ou NULL. */
static vfs_dev_t *dev_of(const char *path) {
    return is_ram_path(path) ? dev_lookup(path, 1) : NULL;
}

int vfs_read(const char *path, void *buf, uint32_t len) {
    if (!is_valid(path) || is_root(path)) return -1;
    if (buf == NULL && len > 0) return -1;

    vfs_dev_t *dev = dev_of(path);
    if (dev != NULL) return dev->read ? dev->read(buf, len) : -1;

    if (is_ram_path(path)) return dev_shadows(path) ? -1 : ram_fs_read(path, buf, len);
    int n = fat32_read(path, buf, len);
    return n < 0 ? -1 : n;
}

int vfs_write(const char *path, const void *buf, uint32_t len) {
    if (!is_valid(path) || is_root(path)) return -1;
    if (buf == NULL && len > 0) return -1;

    vfs_dev_t *dev = dev_of(path);
    if (dev != NULL) return dev->write ? dev->write(buf, len) : -1;

    if (is_ram_path(path)) {
        if (dev_shadows(path) || is_mount_point(path)) return -1;
        return ram_fs_write(path, buf, len);
    }
    int n = fat32_write(path, buf, len);
    return n < 0 ? -1 : n;
}

int vfs_append(const char *path, const void *buf, uint32_t len) {
    if (!is_valid(path) || is_root(path)) return -1;
    if (buf == NULL && len > 0) return -1;

    /* Sur un périphérique, ajouter et écrire reviennent au même. */
    vfs_dev_t *dev = dev_of(path);
    if (dev != NULL) return dev->write ? dev->write(buf, len) : -1;

    if (is_ram_path(path)) {
        if (dev_shadows(path) || is_mount_point(path)) return -1;
        return ram_fs_append(path, buf, len);
    }
    int n = fat32_append(path, buf, len);
    return n < 0 ? -1 : n;
}

int vfs_truncate(const char *path, uint32_t size) {
    if (!is_valid(path) || is_root(path) || is_mount_point(path) || dev_shadows(path)) return -1;
    if (is_ram_path(path)) return ram_fs_truncate(path, size);
    return fat32_truncate(path, size) == FAT32_OK ? 0 : -1;
}

int vfs_stat(const char *path, uint32_t *out_size, int *out_is_dir) {
    if (!is_valid(path)) return -1;

    if (is_root(path)) {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = 1;
        return 0;
    }

    if (!is_ram_path(path)) return fat32_stat(path, out_size, out_is_dir);

    if (dev_lookup(path, 1) != NULL) {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = 0;
        return 0;
    }
    if (dev_shadows(path)) return -1;

    return ram_fs_stat(path, out_size, out_is_dir);
}

int vfs_exists(const char *path) {
    return vfs_stat(path, NULL, NULL) == 0;
}

int vfs_is_dir(const char *path) {
    int dir = 0;
    return vfs_stat(path, NULL, &dir) == 0 && dir;
}

int vfs_mkdir(const char *path) {
    if (!is_valid(path) || is_root(path) || dev_shadows(path)) return -1;
    if (!is_ram_path(path)) return fat32_mkdir(path) == FAT32_OK ? 0 : -1;
    return ram_fs_mkdir(path);
}

/* Crée chaque dossier du chemin manquant sur la carte, comme mkdir -p. */
static int fat32_mkdir_p(const char *path) {
    char partial[VFS_PATH_MAX];
    uint32_t len = (uint32_t)strlen(path);
    if (len >= sizeof(partial)) return -1;

    for (uint32_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0') continue;
        if (path[i - 1] == '/') continue;              /* séparateurs multiples */

            memcpy(partial, path, i);
        partial[i] = '\0';

        int r = fat32_mkdir(partial);
        if (r == FAT32_ERR_EXIST) {
            int dir = 0;
            if (fat32_stat(partial, NULL, &dir) != FAT32_OK || !dir) return -1;
        } else if (r != FAT32_OK) {
            return -1;
        }
    }
    return 0;
}

int vfs_mkdir_p(const char *path) {
    if (!is_valid(path) || is_root(path) || dev_shadows(path)) return -1;
    if (!is_ram_path(path)) return fat32_mkdir_p(path);
    return ram_fs_mkdir_p(path);
}

int vfs_rmdir(const char *path) {
    if (!is_valid(path) || is_root(path) || is_mount_point(path) || dev_shadows(path)) return -1;
    if (!is_ram_path(path)) return fat32_rmdir(path) == FAT32_OK ? 0 : -1;
    return ram_fs_rmdir(path);
}

int vfs_unlink(const char *path) {
    /* Les périphériques sont permanents (voir le registre plus haut). */
    if (!is_valid(path) || is_root(path) || is_mount_point(path) || dev_shadows(path)) return -1;
    if (!is_ram_path(path)) return fat32_unlink(path) == FAT32_OK ? 0 : -1;
    return ram_fs_unlink(path);
}

/* Ajoute à pl les chemins d'une liste renvoyée par un ls, puis la libère. */
static void merge(path_list_t *pl, char **list) {
    for (uint32_t i = 0; list && list[i]; i++) path_list_add_path(pl, list[i]);
    path_list_free(list);
}

int vfs_ls(const char *path, char ***out) {
    *out = NULL;
    if (!is_valid(path)) return -1;

    path_list_t pl;
    path_list_init(&pl);
    char **sub = NULL;

    if (is_root(path)) {
        /* Points de montage, puis la racine de la carte si elle est montée. */
        path_list_add_path(&pl, "/dev");
        path_list_add_path(&pl, "/tmp");
        if (fat32_is_mounted()) {
            if (fat32_ls("/", &sub) < 0) { path_list_discard(&pl); return -1; }
            merge(&pl, sub);
        }
        return path_list_finish(&pl, out);
    }

    if (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0) {
        /* Périphériques, puis ce que ramfs contient dans /dev. */
        spinlock_acquire(&dev_lock);
        for (vfs_dev_t *d = dev_list; d; d = d->next) {
            path_list_add(&pl, path, d->name, (uint32_t)strlen(d->name));
        }
        spinlock_release(&dev_lock);
        if (ram_fs_ls(path, &sub) < 0) { path_list_discard(&pl); return -1; }
        merge(&pl, sub);
        return path_list_finish(&pl, out);
    }

    if (is_ram_path(path)) {
        if (dev_shadows(path)) return -1;
        return ram_fs_ls(path, out);
    }
    int n = fat32_ls(path, out);
    return n < 0 ? -1 : n;
}

int vfs_mknod(const char *path, vfs_dev_read_t read, vfs_dev_write_t write) {
    if (!is_valid(path) || (read == NULL && write == NULL)) return -1;

    uint32_t n = dev_component_len(path);
    if (n == 0 || n >= VFS_DEV_NAME_MAX || path[5 + n] != '\0') return -1;

    if (ram_fs_exists(path)) return -1;

    vfs_dev_t *d = (vfs_dev_t *)nmap(sizeof(vfs_dev_t));
    if (d == NULL) return -1;
    memset(d, 0, sizeof(vfs_dev_t));
    strncpy(d->name, path + 5, n);
    d->name[n] = '\0';
    d->read  = read;
    d->write = write;

    spinlock_acquire(&dev_lock);
    for (vfs_dev_t *it = dev_list; it; it = it->next) {
        if (strcmp(it->name, d->name) == 0) {
            spinlock_release(&dev_lock);
            unmap(d);
            return -1;
        }
    }
    if (dev_list == NULL) {
        dev_list = d;
    } else {
        vfs_dev_t *last = dev_list;
        while (last->next) last = last->next;
        last->next = d;
    }
    spinlock_release(&dev_lock);
    return 0;
}
