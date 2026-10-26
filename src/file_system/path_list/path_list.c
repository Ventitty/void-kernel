#include "src/file_system/path_list/path_list.h"

void path_list_init(path_list_t *pl) {
    pl->buf = NULL;
    pl->used = 0;
    pl->cap = 0;
    pl->count = 0;
    pl->failed = 0;
}

/* Réserve extra octets dans le tampon temporaire. 0, ou -1. */
static int reserve(path_list_t *pl, uint32_t extra) {
    if (pl->failed) return -1;
    if (extra > 0xFFFFFFFFu / 2u - pl->used) { pl->failed = 1; return -1; }
    uint32_t needed = pl->used + extra;
    if (needed <= pl->cap) return 0;

    /* Tampon temporaire : doublement, pour ne pas tout recopier à chaque
     * entrée. La liste finale, elle, est allouée à la taille exacte. */
    uint32_t cap = pl->cap ? pl->cap : 256u;
    while (cap < needed) cap *= 2u;

    char *nb = (char *)nmap(cap);
    if (nb == NULL) { pl->failed = 1; return -1; }
    if (pl->buf) {
        memcpy(nb, pl->buf, pl->used);
        unmap(pl->buf);
    }
    pl->buf = nb;
    pl->cap = cap;
    return 0;
}

void path_list_add(path_list_t *pl, const char *dir, const char *name, uint32_t name_len) {
    uint32_t dlen = (uint32_t)strlen(dir);
    int slash = !(dlen > 0 && dir[dlen - 1] == '/');
    if (dlen > 0xFFFFu || name_len > 0xFFFFu) { pl->failed = 1; return; }

    if (reserve(pl, dlen + (uint32_t)slash + name_len + 1u) != 0) return;
    char *p = pl->buf + pl->used;
    memcpy(p, dir, dlen);
    p += dlen;
    if (slash) *p++ = '/';
    memcpy(p, name, name_len);
    p[name_len] = '\0';
    pl->used += dlen + (uint32_t)slash + name_len + 1u;
    pl->count++;
}

void path_list_add_path(path_list_t *pl, const char *path) {
    uint32_t len = (uint32_t)strlen(path);
    if (reserve(pl, len + 1u) != 0) return;
    memcpy(pl->buf + pl->used, path, len + 1u);
    pl->used += len + 1u;
    pl->count++;
}

void path_list_discard(path_list_t *pl) {
    if (pl->buf) unmap(pl->buf);
    path_list_init(pl);
}

int path_list_finish(path_list_t *pl, char ***out) {
    *out = NULL;
    if (pl->failed) { path_list_discard(pl); return -1; }

    uint32_t table = (pl->count + 1u) * (uint32_t)sizeof(char *);
    char **list = (char **)nmap(table + pl->used);
    if (list == NULL) { path_list_discard(pl); return -1; }

    char *strings = (char *)list + table;
    if (pl->used > 0) memcpy(strings, pl->buf, pl->used);

    char *p = strings;
    for (uint32_t i = 0; i < pl->count; i++) {
        list[i] = p;
        p += strlen(p) + 1u;
    }
    list[pl->count] = NULL;

    int count = (int)pl->count;
    path_list_discard(pl);
    *out = list;
    return count;
}

void path_list_free(char **list) {
    if (list) unmap(list);
}
