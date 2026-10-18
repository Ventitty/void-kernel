#include "src/file_system/vfs/vfs.h"

/* vfs_open passe ses options telles quelles à fat32_open. */
typedef char vfs_fat32_flags_match[(VFS_O_READ == FAT32_O_READ && VFS_O_WRITE == FAT32_O_WRITE && VFS_O_CREATE == FAT32_O_CREATE && VFS_O_TRUNC == FAT32_O_TRUNC && VFS_O_APPEND == FAT32_O_APPEND) ? 1 : -1];

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

vfs_file_t *vfs_open(const char *path, int flags) {
    if (!is_valid(path) || is_root(path)) return NULL;

    void *handle;
    vfs_backend_t backend;

    if (is_ram_path(path)) {
        vfs_dev_t *dev = dev_lookup(path, 1);
        if (dev != NULL) {
            handle = dev;
            backend = VFS_BACKEND_DEV;
        } else {
            if (dev_shadows(path)) return NULL;
            handle = ram_fs_open(path, flags);
            backend = VFS_BACKEND_RAM;
        }
    } else {
        /* Les options VFS_O_* ont les mêmes valeurs que FAT32_O_*. */
        handle = fat32_open(path, flags);
        backend = VFS_BACKEND_FAT32;
    }
    if (handle == NULL) return NULL;

    vfs_file_t *f = (vfs_file_t *)nmap(sizeof(vfs_file_t));
    if (f == NULL) {
        if (backend == VFS_BACKEND_RAM)        ram_fs_close((ram_fs_file_t *)handle);
        else if (backend == VFS_BACKEND_FAT32) fat32_close((fat32_file_t *)handle);
        return NULL;
    }

    f->backend = backend;
    f->handle = handle;
    return f;
}

void vfs_close(vfs_file_t *file) {
    if (file == NULL) return;
    if (file->backend == VFS_BACKEND_RAM)        ram_fs_close((ram_fs_file_t *)file->handle);
    else if (file->backend == VFS_BACKEND_FAT32) fat32_close((fat32_file_t *)file->handle);
    unmap(file);
}

int vfs_read(vfs_file_t *file, void *buf, uint32_t len) {
    if (file == NULL || buf == NULL) return -1;
    if (file->backend == VFS_BACKEND_DEV) {
        vfs_dev_t *d = (vfs_dev_t *)file->handle;
        return d->read ? d->read(buf, len) : -1;
    }
    if (file->backend == VFS_BACKEND_RAM) return ram_fs_read((ram_fs_file_t *)file->handle, buf, len);
    return fat32_read((fat32_file_t *)file->handle, buf, len);
}

int vfs_write(vfs_file_t *file, const void *buf, uint32_t len) {
    if (file == NULL || buf == NULL) return -1;
    if (file->backend == VFS_BACKEND_DEV) {
        vfs_dev_t *d = (vfs_dev_t *)file->handle;
        return d->write ? d->write(buf, len) : -1;
    }
    if (file->backend == VFS_BACKEND_RAM) return ram_fs_write((ram_fs_file_t *)file->handle, buf, len);
    int n = fat32_write((fat32_file_t *)file->handle, buf, len);
    return n < 0 ? -1 : n;
}

int vfs_seek(vfs_file_t *file, int32_t offset, int whence) {
    if (file == NULL || file->backend == VFS_BACKEND_DEV) return -1;   /* flux : pas de position */
        if (file->backend == VFS_BACKEND_RAM) return ram_fs_seek((ram_fs_file_t *)file->handle, offset, whence);
        return fat32_seek((fat32_file_t *)file->handle, offset, whence);
}

uint32_t vfs_size(vfs_file_t *file) {
    if (file == NULL || file->backend == VFS_BACKEND_DEV) return 0;
    if (file->backend == VFS_BACKEND_RAM) return ram_fs_size((ram_fs_file_t *)file->handle);
    return fat32_size((fat32_file_t *)file->handle);
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

    if (!ram_fs_exists(path)) return -1;
    int dir = ram_fs_is_dir(path);
    uint32_t size = 0;
    if (!dir) {
        ram_fs_file_t *f = ram_fs_open(path, RAM_FS_O_READ);
        if (f) { size = ram_fs_size(f); ram_fs_close(f); }
    }
    if (out_size) *out_size = size;
    if (out_is_dir) *out_is_dir = dir;
    return 0;
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

int vfs_ls(const char *path) {
    if (!is_valid(path)) return -1;

    if (is_root(path)) {
        uart_print("D  dev/\n");
        uart_print("D  tmp/\n");
        if (!fat32_is_mounted()) {
            uart_print("   (carte SD non montee)\n");
            return 0;
        }
        return fat32_ls("/");
    }

    if (strcmp(path, "/dev") == 0) {
        spinlock_acquire(&dev_lock);
        vfs_dev_t *d = dev_list;
        spinlock_release(&dev_lock);
        for (; d; d = d->next) {
            uart_print("C  ");
            uart_print(d->name);
            uart_print("\n");
        }
        return ram_fs_ls(path);
    }

    if (is_ram_path(path)) {
        if (dev_shadows(path)) return -1;
        return ram_fs_ls(path);
    }
    return fat32_ls(path);
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
