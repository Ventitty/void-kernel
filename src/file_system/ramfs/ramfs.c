#include "src/file_system/ramfs/ramfs.h"

static ram_fs_node_t root;
static spinlock_t ram_fs_lock = SPINLOCK_INIT;

static ram_fs_node_t *find_child(ram_fs_node_t *dir, const char *name, uint32_t len) {
    for (ram_fs_node_t *c = dir->first_child; c; c = c->next_sibling) {
        if (strlen(c->name) == len && strncmp(c->name, name, len) == 0) return c;
    }
    return NULL;
}

static ram_fs_node_t *alloc_node(const char *name, uint32_t len, ram_fs_type_t type, ram_fs_node_t *parent) {
    ram_fs_node_t *n = (ram_fs_node_t *)nmap(sizeof(ram_fs_node_t));
    if (!n) return NULL;

    memset(n, 0, sizeof(ram_fs_node_t));
    strncpy(n->name, name, len < RAM_FS_MAX_NAME - 1 ? len : RAM_FS_MAX_NAME - 1);
    n->name[len < RAM_FS_MAX_NAME - 1 ? len : RAM_FS_MAX_NAME - 1] = '\0';
    n->type = type;
    n->parent = parent;

    n->next_sibling = parent->first_child;
    parent->first_child = n;

    return n;
}

static void detach_node(ram_fs_node_t *node) {
    ram_fs_node_t *parent = node->parent;
    if (!parent) return;

    if (parent->first_child == node) {
        parent->first_child = node->next_sibling;
        return;
    }
    for (ram_fs_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (c->next_sibling == node) {
            c->next_sibling = node->next_sibling;
            return;
        }
    }
}

static ram_fs_node_t *resolve_parent(const char *path, const char **out_name, uint32_t *out_len) {
    if (!path || path[0] != '/') return NULL;

    ram_fs_node_t *cur = &root;
    const char *p = path + 1;

    while (1) {
        const char *start = p;
        while (*p != '\0' && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);

        if (*p == '\0') {
            *out_name = start;
            *out_len = len;
            return cur;
        }

        if (len == 0) { p++; continue; }

        ram_fs_node_t *child = find_child(cur, start, len);
        if (!child || child->type != RAM_FS_TYPE_DIR) return NULL;
        cur = child;
        p++;
    }
}

static ram_fs_node_t *find_node(const char *path) {
    if (path && path[0] == '/' && path[1] == '\0') return &root;

    const char *name;
    uint32_t len;
    ram_fs_node_t *parent = resolve_parent(path, &name, &len);
    if (!parent || len == 0) return NULL;

    return find_child(parent, name, len);
}

/* Agrandit le tampon d'un fichier pour contenir needed octets. La capacité
 * double à chaque fois (au moins 64 octets) : une suite d'ajouts en fin de
 * fichier ne recopie pas tout le contenu à chaque écriture. */
static int ensure_capacity(ram_fs_node_t *n, uint32_t needed) {
    if (needed <= n->capacity) return 0;

    uint32_t cap = n->capacity ? n->capacity : 64u;
    while (cap < needed) {
        if (cap > 0x7FFFFFFFu) { cap = needed; break; }
        cap *= 2u;
    }

    uint8_t *new_buf = (uint8_t *)nmap(cap);
    if (!new_buf) return -1;

    if (n->data) {
        memcpy(new_buf, n->data, n->size);
        unmap(n->data);
    }
    n->data = new_buf;
    n->capacity = cap;
    return 0;
}

void ram_fs_init(void) {
    spinlock_acquire(&ram_fs_lock);
    memset(&root, 0, sizeof(root));
    root.type = RAM_FS_TYPE_DIR;
    root.name[0] = '\0';
    spinlock_release(&ram_fs_lock);
}

int ram_fs_read(const char *path, void *buf, uint32_t len) {
    if (buf == NULL && len > 0) return -1;

    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = find_node(path);
    int ret = -1;
    /* Tout le fichier ou rien : pas de lecture partielle. */
    if (n != NULL && n->type == RAM_FS_TYPE_FILE && n->size <= len) {
        if (n->size > 0) memcpy(buf, n->data, n->size);
        ret = (int)n->size;
    }

    spinlock_release(&ram_fs_lock);
    return ret;
}

/* Fichier path, créé (vide) s'il n'existe pas. NULL si le dossier parent
 * manque, si le nom est invalide ou si path est un dossier. Verrou tenu
 * par l'appelant. */
static ram_fs_node_t *file_get_or_create(const char *path) {
    const char *name;
    uint32_t name_len;
    ram_fs_node_t *parent = resolve_parent(path, &name, &name_len);
    if (!parent || name_len == 0 || name_len >= RAM_FS_MAX_NAME) return NULL;

    ram_fs_node_t *n = find_child(parent, name, name_len);
    if (n == NULL) return alloc_node(name, name_len, RAM_FS_TYPE_FILE, parent);
    return (n->type == RAM_FS_TYPE_FILE) ? n : NULL;
}

int ram_fs_write(const char *path, const void *buf, uint32_t len) {
    if (buf == NULL && len > 0) return -1;

    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = file_get_or_create(path);
    int ret = -1;
    if (n != NULL && ensure_capacity(n, len) == 0) {
        if (len > 0) memcpy(n->data, buf, len);
        n->size = len;                       /* l'ancien contenu est remplacé */
        ret = (int)len;
    }

    spinlock_release(&ram_fs_lock);
    return ret;
}

int ram_fs_append(const char *path, const void *buf, uint32_t len) {
    if (buf == NULL && len > 0) return -1;

    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = file_get_or_create(path);
    int ret = -1;
    if (n != NULL && len <= 0xFFFFFFFFu - n->size && ensure_capacity(n, n->size + len) == 0) {
        if (len > 0) memcpy(n->data + n->size, buf, len);
        n->size += len;
        ret = (int)len;
    }

    spinlock_release(&ram_fs_lock);
    return ret;
}

int ram_fs_truncate(const char *path, uint32_t size) {
    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = find_node(path);
    int ret = -1;
    if (n != NULL && n->type == RAM_FS_TYPE_FILE) {
        if (size <= n->size) {
            n->size = size;
            ret = 0;
        } else if (ensure_capacity(n, size) == 0) {
            memset(n->data + n->size, 0, size - n->size);
            n->size = size;
            ret = 0;
        }
    }

    spinlock_release(&ram_fs_lock);
    return ret;
}

int ram_fs_stat(const char *path, uint32_t *out_size, int *out_is_dir) {
    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = find_node(path);
    if (n != NULL) {
        if (out_size) *out_size = (n->type == RAM_FS_TYPE_FILE) ? n->size : 0;
        if (out_is_dir) *out_is_dir = (n->type == RAM_FS_TYPE_DIR);
    }

    spinlock_release(&ram_fs_lock);
    return n != NULL ? 0 : -1;
}

int ram_fs_unlink(const char *path) {
    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = find_node(path);
    if (!n || n == &root || n->type == RAM_FS_TYPE_DIR) {
        spinlock_release(&ram_fs_lock);
        return -1;
    }

    detach_node(n);
    if (n->data) unmap(n->data);
    unmap(n);

    spinlock_release(&ram_fs_lock);
    return 0;
}

int ram_fs_mkdir(const char *path) {
    spinlock_acquire(&ram_fs_lock);

    const char *name;
    uint32_t len;
    ram_fs_node_t *parent = resolve_parent(path, &name, &len);
    if (!parent || len == 0 || len >= RAM_FS_MAX_NAME) {
        spinlock_release(&ram_fs_lock);
        return -1;
    }

    ram_fs_node_t *existing = find_child(parent, name, len);
    if (existing) {
        int ok = (existing->type == RAM_FS_TYPE_DIR);
        spinlock_release(&ram_fs_lock);
        return ok ? 0 : -1;
    }

    ram_fs_node_t *n = alloc_node(name, len, RAM_FS_TYPE_DIR, parent);
    spinlock_release(&ram_fs_lock);
    return n ? 0 : -1;
}

int ram_fs_mkdir_p(const char *path) {
    if (!path || path[0] != '/') return -1;

    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *cur = &root;
    const char *p = path + 1;

    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);

        if (len == 0) { if (*p == '/') p++; continue; }
        if (len >= RAM_FS_MAX_NAME) { spinlock_release(&ram_fs_lock); return -1; }

        ram_fs_node_t *child = find_child(cur, start, len);
        if (!child) {
            child = alloc_node(start, len, RAM_FS_TYPE_DIR, cur);
            if (!child) { spinlock_release(&ram_fs_lock); return -1; }
        } else if (child->type != RAM_FS_TYPE_DIR) {
            spinlock_release(&ram_fs_lock);
            return -1;
        }

        cur = child;
        if (*p == '/') p++;
    }

    spinlock_release(&ram_fs_lock);
    return 0;
}

int ram_fs_rmdir(const char *path) {
    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *n = find_node(path);
    if (!n || n == &root || n->type != RAM_FS_TYPE_DIR) {
        spinlock_release(&ram_fs_lock);
        return -1;
    }
    if (n->first_child != NULL) {
        spinlock_release(&ram_fs_lock);
        return -1;
    }

    detach_node(n);
    unmap(n);

    spinlock_release(&ram_fs_lock);
    return 0;
}

int ram_fs_is_dir(const char *path) {
    spinlock_acquire(&ram_fs_lock);
    ram_fs_node_t *n = find_node(path);
    int ok = n && n->type == RAM_FS_TYPE_DIR;
    spinlock_release(&ram_fs_lock);
    return ok;
}

int ram_fs_exists(const char *path) {
    spinlock_acquire(&ram_fs_lock);
    int ok = find_node(path) != NULL;
    spinlock_release(&ram_fs_lock);
    return ok;
}

int ram_fs_ls(const char *path) {
    spinlock_acquire(&ram_fs_lock);

    ram_fs_node_t *dir = find_node(path);
    if (!dir || dir->type != RAM_FS_TYPE_DIR) {
        spinlock_release(&ram_fs_lock);
        return -1;
    }

    for (ram_fs_node_t *c = dir->first_child; c; c = c->next_sibling) {
        if (c->type == RAM_FS_TYPE_DIR) {
            uart_print("D  ");
            uart_print(c->name);
            uart_print("/\n");
        } else {
            uart_print("F  ");
            uart_print(c->name);
            uart_print("  (");
            uart_print_hex(c->size);
            uart_print(" octets)\n");
        }
    }

    spinlock_release(&ram_fs_lock);
    return 0;
}

static void print_node_recursive(ram_fs_node_t *node, int depth) {
    for (int i = 0; i < depth; i++) uart_print("  ");

    if (node == &root) {
        uart_print("/\n");
    } else if (node->type == RAM_FS_TYPE_DIR) {
        uart_print(node->name);
        uart_print("/\n");
    } else {
        uart_print(node->name);
        uart_print("  (");
        uart_print_hex(node->size);
        uart_print(" octets)\n");
    }

    if (node->type == RAM_FS_TYPE_DIR) {
        for (ram_fs_node_t *c = node->first_child; c; c = c->next_sibling) {
            print_node_recursive(c, depth + 1);
        }
    }
}

void ram_fs_print_tree(void) {
    spinlock_acquire(&ram_fs_lock);
    print_node_recursive(&root, 0);
    spinlock_release(&ram_fs_lock);
}
