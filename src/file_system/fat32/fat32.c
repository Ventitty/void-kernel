#include "src/file_system/fat32/fat32.h"

/* ---------------------------------------------------------------------------
 * Toutes les fonctions publiques prennent fat32_lock pour toute leur durée.
 * Les tampons de secteur sont statiques (protégés par ce verrou) plutôt que
 * sur la pile : les piles des tâches ne font que 4 Ko.
 *
 *   sec_buf : secteur de travail (dossiers, données) ;
 *   fat_buf : cache d'un secteur de FAT, en écriture directe (toujours
 *             identique au disque).
 * ------------------------------------------------------------------------- */

static spinlock_t fat32_lock = SPINLOCK_INIT;
static struct volume vol;

static uint8_t  sec_buf[512];
static uint8_t  fat_buf[512];
static uint32_t fat_buf_lba = 0xFFFFFFFFu;

static void cache_invalidate(void);   /* cache du dernier fichier, plus bas */

/* Date écrite dans les entrées créées ou modifiées : le noyau n'a pas
 * d'horloge calendaire. 1er janvier 2026, minuit. */
#define FAT_DATE(y, m, d) ((uint16_t)((((y) - 1980u) << 9) | ((m) << 5) | (d)))
#define FAT32_DEFAULT_DATE FAT_DATE(2026u, 1u, 1u)
#define FAT32_DEFAULT_TIME 0u

/* Indicateurs de casse (octet 12 d'une entrée) : nom et extension stockés
 * en majuscules mais à afficher en minuscules. */
#define NTRES_LOWER_BASE 0x08u
#define NTRES_LOWER_EXT  0x10u

/* Un nom long occupe au plus 20 entrées (255 caractères / 13). */
#define MAX_LFN_ENTRIES 20u

#define FSINFO_LEAD_SIG   0x41615252u
#define FSINFO_STRUC_SIG  0x61417272u
#define FSINFO_TRAIL_SIG  0xAA550000u
#define FSINFO_UNKNOWN    0xFFFFFFFFu

/* ---- Utilitaires ---------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t max_cluster(void) {
    return vol.total_clusters + 1u;
}

static int valid_cluster(uint32_t c) {
    return c >= 2u && c <= max_cluster();
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return vol.data_start_lba + (cluster - 2u) * vol.sectors_per_cluster;
}

static uint32_t bytes_per_cluster(void) {
    return vol.bytes_per_sector * vol.sectors_per_cluster;
}

/* ---- Montage ------------------------------------------------------------- */

int fat32_mount(void) {
    uint8_t *sector = sec_buf;
    int ret = FAT32_ERR_IO;

    spinlock_acquire(&fat32_lock);
    cache_invalidate();
    vol.mounted = 0;
    fat_buf_lba = 0xFFFFFFFFu;

    if (sd_read_block(0, sector) != 0) goto out;
    if (sector[510] != 0x55 || sector[511] != 0xAA) goto out;

    uint32_t volume_start = 0;
    uint8_t part_type = sector[0x1BE + 4];
    if (part_type == 0x0B || part_type == 0x0C) {
        volume_start = rd32(&sector[0x1BE + 8]);
    }

    if (sd_read_block(volume_start, sector) != 0) goto out;

    uint32_t bytes_per_sector = rd16(&sector[0x0B]);
    uint32_t fat_size_32 = rd32(&sector[0x24]);
    if (bytes_per_sector != 512 || fat_size_32 == 0) goto out;

    vol.volume_start_lba    = volume_start;
    vol.bytes_per_sector    = bytes_per_sector;
    vol.sectors_per_cluster = sector[0x0D];
    vol.reserved_sectors    = rd16(&sector[0x0E]);
    vol.num_fats            = sector[0x10];
    vol.fat_size_sectors    = fat_size_32;
    vol.root_cluster        = rd32(&sector[0x2C]);

    if (vol.sectors_per_cluster == 0 || vol.num_fats == 0) goto out;

    vol.fat_start_lba  = vol.volume_start_lba + vol.reserved_sectors;
    vol.data_start_lba = vol.fat_start_lba + vol.num_fats * vol.fat_size_sectors;

    uint32_t total_sectors = rd32(&sector[0x20]);
    if (total_sectors == 0) total_sectors = rd16(&sector[0x13]);
    uint32_t meta = vol.reserved_sectors + vol.num_fats * vol.fat_size_sectors;
    if (total_sectors <= meta) goto out;
    vol.total_clusters = (total_sectors - meta) / vol.sectors_per_cluster;

    /* Les entrées de FAT disponibles bornent aussi le nombre de clusters. */
    uint32_t fat_entries = vol.fat_size_sectors * (512u / 4u);
    if (vol.total_clusters + 2u > fat_entries) vol.total_clusters = fat_entries - 2u;
    if (!valid_cluster(vol.root_cluster)) goto out;

    /* Miroir des FAT (BPB_ExtFlags, bit 7 à 1 : une seule FAT active). */
    uint16_t ext_flags = rd16(&sector[0x28]);
    vol.fat_mirrored = (ext_flags & 0x80u) == 0;
    vol.active_fat   = vol.fat_mirrored ? 0u : (ext_flags & 0x0Fu);
    if (vol.active_fat >= vol.num_fats) goto out;

    /* Secteur FSInfo : compteur d'espace libre et indice de recherche. */
    uint32_t fsinfo_sector = rd16(&sector[0x30]);
    vol.fsinfo_lba   = 0;
    vol.free_count   = FSINFO_UNKNOWN;
    vol.next_free    = 2;
    vol.fsinfo_dirty = 0;
    if (fsinfo_sector != 0 && fsinfo_sector != 0xFFFFu && fsinfo_sector < vol.reserved_sectors) {
        uint32_t lba = vol.volume_start_lba + fsinfo_sector;
        if (sd_read_block(lba, sector) == 0 &&
            rd32(&sector[0]) == FSINFO_LEAD_SIG && rd32(&sector[484]) == FSINFO_STRUC_SIG) {
            vol.fsinfo_lba = lba;
        uint32_t fc = rd32(&sector[488]);
        uint32_t nf = rd32(&sector[492]);
        if (fc <= vol.total_clusters) vol.free_count = fc;
        if (valid_cluster(nf)) vol.next_free = nf;
            }
    }

    vol.mounted = 1;
    ret = FAT32_OK;

    out:
    spinlock_release(&fat32_lock);
    return ret;
}

int fat32_is_mounted(void) {
    return vol.mounted;
}

void fat32_print_info(void) {
    if (!vol.mounted) { uart_print("[fat32] aucun volume monte\n"); return; }

    uart_print("[fat32] volume_start_lba="); uart_print_hex(vol.volume_start_lba); uart_print("\n");
    uart_print("[fat32] sectors_per_cluster="); uart_print_hex(vol.sectors_per_cluster); uart_print("\n");
    uart_print("[fat32] reserved_sectors="); uart_print_hex(vol.reserved_sectors); uart_print("\n");
    uart_print("[fat32] num_fats="); uart_print_hex(vol.num_fats); uart_print("\n");
    uart_print("[fat32] fat_size_sectors="); uart_print_hex(vol.fat_size_sectors); uart_print("\n");
    uart_print("[fat32] root_cluster="); uart_print_hex(vol.root_cluster); uart_print("\n");
    uart_print("[fat32] data_start_lba="); uart_print_hex(vol.data_start_lba); uart_print("\n");
    uart_print("[fat32] total_clusters="); uart_print_hex(vol.total_clusters); uart_print("\n");
    uart_print("[fat32] free_clusters="); uart_print_hex(vol.free_count); uart_print("\n");
}

/* ---- FAT ----------------------------------------------------------------- */

static uint32_t fat_sector_of(uint32_t cluster) {
    return (cluster * 4u) / 512u;
}

/* Charge dans fat_buf le secteur de la FAT active contenant cluster. */
static int fat_load(uint32_t cluster) {
    uint32_t lba = vol.fat_start_lba + vol.active_fat * vol.fat_size_sectors + fat_sector_of(cluster);
    if (fat_buf_lba == lba) return 0;
    if (sd_read_block(lba, fat_buf) != 0) {
        fat_buf_lba = 0xFFFFFFFFu;
        return -1;
    }
    fat_buf_lba = lba;
    return 0;
}

/* Valeur de l'entrée de FAT d'un cluster, ou FAT32_BAD_CLUSTER si illisible. */
static uint32_t fat_get(uint32_t cluster) {
    if (!valid_cluster(cluster) || fat_load(cluster) != 0) return FAT32_BAD_CLUSTER;
    return rd32(&fat_buf[(cluster * 4u) % 512u]) & FAT32_CLUSTER_MASK;
}

/* Écrit l'entrée de FAT d'un cluster dans toutes les copies (ou la seule
 * active si le miroir est désactivé). Les 4 bits de poids fort, réservés,
 * sont conservés. */
static int fat_set(uint32_t cluster, uint32_t value) {
    if (!valid_cluster(cluster) || fat_load(cluster) != 0) return -1;

    uint32_t off = (cluster * 4u) % 512u;
    uint32_t old = rd32(&fat_buf[off]);
    wr32(&fat_buf[off], (old & ~FAT32_CLUSTER_MASK) | (value & FAT32_CLUSTER_MASK));

    for (uint32_t i = 0; i < vol.num_fats; i++) {
        if (!vol.fat_mirrored && i != vol.active_fat) continue;
        uint32_t lba = vol.fat_start_lba + i * vol.fat_size_sectors + fat_sector_of(cluster);
        if (sd_write_block(lba, fat_buf) != 0) {
            fat_buf_lba = 0xFFFFFFFFu;   /* état disque incertain : recharger */
            return -1;
        }
    }
    return 0;
}

static void free_count_add(int32_t delta) {
    if (vol.free_count == FSINFO_UNKNOWN) return;
    vol.free_count = (uint32_t)((int32_t)vol.free_count + delta);
    vol.fsinfo_dirty = 1;
}

/* Écrit le compteur d'espace libre et l'indice de recherche dans FSInfo,
 * s'ils ont changé. Appelée à la fin de chaque opération modifiante. */
static void fsinfo_flush(void) {
    if (!vol.fsinfo_dirty || vol.fsinfo_lba == 0) return;
    if (sd_read_block(vol.fsinfo_lba, sec_buf) != 0) return;
    if (rd32(&sec_buf[0]) != FSINFO_LEAD_SIG || rd32(&sec_buf[484]) != FSINFO_STRUC_SIG) return;
    wr32(&sec_buf[488], vol.free_count);
    wr32(&sec_buf[492], vol.next_free);
    if (sd_write_block(vol.fsinfo_lba, sec_buf) == 0) vol.fsinfo_dirty = 0;
}

static int zero_cluster(uint32_t cluster) {
    memset(sec_buf, 0, sizeof(sec_buf));
    uint32_t lba = cluster_to_lba(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
        if (sd_write_block(lba + s, sec_buf) != 0) return -1;
    }
    return 0;
}

/* Alloue un cluster libre, le marque fin de chaîne et, si prev est un
 * cluster, l'accroche derrière lui. Retourne le cluster, ou 0 si la carte
 * est pleine ou en cas d'erreur. */
static uint32_t alloc_cluster(uint32_t prev) {
    uint32_t max = max_cluster();
    uint32_t start = valid_cluster(vol.next_free) ? vol.next_free : 2u;
    uint32_t c = start;

    for (uint32_t n = 0; n < vol.total_clusters; n++) {
        uint32_t v = fat_get(c);
        if (v == 0) {
            if (fat_set(c, FAT32_EOC) != 0) return 0;
            if (valid_cluster(prev) && fat_set(prev, c) != 0) {
                (void)fat_set(c, 0);
                return 0;
            }
            vol.next_free = (c + 1u <= max) ? c + 1u : 2u;
            vol.fsinfo_dirty = 1;
            free_count_add(-1);
            return c;
        }
        if (v == FAT32_BAD_CLUSTER && fat_buf_lba == 0xFFFFFFFFu) return 0;   /* erreur de lecture */
            c = (c + 1u <= max) ? c + 1u : 2u;
    }
    return 0;
}

/* Libère toute une chaîne de clusters. Bornée par le nombre de clusters du
 * volume : une FAT corrompue (boucle) ne bloque pas le noyau. */
static int free_chain(uint32_t first) {
    uint32_t c = first;
    for (uint32_t n = 0; n < vol.total_clusters && valid_cluster(c); n++) {
        uint32_t next = fat_get(c);
        if (fat_set(c, 0) != 0) return -1;
        free_count_add(1);
        if (c < vol.next_free) {
            vol.next_free = c;
            vol.fsinfo_dirty = 1;
        }
        if (next >= FAT32_EOC_MIN || !valid_cluster(next)) break;
        c = next;
    }
    return 0;
}

/* ---- Noms 8.3 ------------------------------------------------------------ */

static int is_83_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') return 1;
    if (ch >= 'a' && ch <= 'z') return 1;
    if (ch >= '0' && ch <= '9') return 1;
    switch (ch) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')':
        case '-': case '@': case '^': case '_': case '`': case '{': case '}': case '~':
            return 1;
        default:
            return 0;
    }
}

/* Convertit un composant de chemin en nom court FAT (11 octets, complétés
 * d'espaces, en majuscules). Accepte "." et "..". *ntres reçoit les
 * indicateurs de casse à stocker. Retourne 0, ou -1 si le nom n'est pas un
 * nom 8.3 valide. */
static int encode_83(const char *name, uint32_t len, uint8_t out[11], uint8_t *ntres) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (ntres) *ntres = 0;

    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
        for (uint32_t i = 0; i < len; i++) out[i] = '.';
        return 0;
    }
    if (len == 0 || len > 12) return -1;

    uint32_t dot = len;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '.') {
            if (dot != len) return -1;   /* un seul point */
                dot = i;
        } else if (!is_83_char(name[i])) {
            return -1;
        }
    }

    uint32_t base_len = dot;
    uint32_t ext_len  = (dot < len) ? len - dot - 1u : 0u;
    if (base_len == 0 || base_len > 8) return -1;
    if (dot < len && (ext_len == 0 || ext_len > 3)) return -1;

    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
    for (uint32_t i = 0; i < base_len; i++) {
        char ch = name[i];
        if (ch >= 'a' && ch <= 'z') base_lower = 1;
        if (ch >= 'A' && ch <= 'Z') base_upper = 1;
        out[i] = (uint8_t)to_upper((unsigned char)ch);
    }
    for (uint32_t i = 0; i < ext_len; i++) {
        char ch = name[dot + 1u + i];
        if (ch >= 'a' && ch <= 'z') ext_lower = 1;
        if (ch >= 'A' && ch <= 'Z') ext_upper = 1;
        out[8 + i] = (uint8_t)to_upper((unsigned char)ch);
    }

    /* Casse mixte : impossible à représenter sans nom long, on stocke en
     * majuscules. Tout en minuscules : on le signale pour l'affichage. */
    if (ntres) {
        if (base_lower && !base_upper) *ntres |= NTRES_LOWER_BASE;
        if (ext_lower && !ext_upper)   *ntres |= NTRES_LOWER_EXT;
    }
    return 0;
}

/* ---- Dossiers ------------------------------------------------------------ */

typedef struct {
    uint32_t lba;
    uint32_t off;
} dloc_t;

typedef struct {
    uint8_t  raw[DIRENT_SIZE];
    dloc_t   loc;
    dloc_t   lfn[MAX_LFN_ENTRIES];   /* entrées de nom long qui précèdent */
    uint32_t lfn_count;
} dentry_t;

static uint32_t dirent_cluster(const uint8_t *e) {
    return ((uint32_t)rd16(&e[20]) << 16) | rd16(&e[26]);
}

static void dirent_set_cluster(uint8_t *e, uint32_t c) {
    wr16(&e[20], (uint16_t)(c >> 16));
    wr16(&e[26], (uint16_t)c);
}

static int dirent_is_dir(const uint8_t *e) {
    return (e[11] & ATTR_DIRECTORY) != 0;
}

/* Cherche le nom court key dans le dossier dont le premier cluster est
 * dir_cluster. 1 trouvé (out rempli), 0 absent, -1 erreur. */
static int dir_lookup(uint32_t dir_cluster, const uint8_t key[11], dentry_t *out) {
    uint32_t c = dir_cluster;
    uint32_t lfn_count = 0;

    for (uint32_t n = 0; n < vol.total_clusters && valid_cluster(c); n++) {
        uint32_t base = cluster_to_lba(c);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (sd_read_block(base + s, sec_buf) != 0) return -1;

            for (uint32_t off = 0; off < 512; off += DIRENT_SIZE) {
                const uint8_t *e = &sec_buf[off];
                if (e[0] == DIRENT_FREE) return 0;
                if (e[0] == DIRENT_DELETED) { lfn_count = 0; continue; }

                if (e[11] == ATTR_LFN) {
                    if (lfn_count < MAX_LFN_ENTRIES) {
                        out->lfn[lfn_count].lba = base + s;
                        out->lfn[lfn_count].off = off;
                        lfn_count++;
                    }
                    continue;
                }
                if (e[11] & ATTR_VOLUME_ID) { lfn_count = 0; continue; }

                if (memcmp(e, key, 11) == 0) {
                    memcpy(out->raw, e, DIRENT_SIZE);
                    out->loc.lba = base + s;
                    out->loc.off = off;
                    out->lfn_count = lfn_count;
                    return 1;
                }
                lfn_count = 0;
            }
        }
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC_MIN) break;
        c = next;
    }
    return 0;
}

/* Résout le dossier parent du dernier composant de path. Retourne 0 et
 * renseigne parent / name / name_len, ou FAT32_ERR_*. */
static int resolve_parent(const char *path, uint32_t *parent, const char **name, uint32_t *name_len) {
    if (path == NULL || path[0] != '/') return FAT32_ERR_INVAL;

    uint32_t cur = vol.root_cluster;
    const char *p = path + 1;

    while (1) {
        while (*p == '/') p++;   /* séparateurs multiples */
            const char *start = p;
        while (*p != '\0' && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);

        const char *rest = p;
        while (*rest == '/') rest++;
        if (*rest == '\0') {     /* dernier composant (éventuels '/' finaux) */
            *parent = cur;
            *name = start;
            *name_len = len;
            return FAT32_OK;
        }

        uint8_t key[11];
        if (encode_83(start, len, key, NULL) != 0) return FAT32_ERR_NOENT;

        dentry_t d;
        int r = dir_lookup(cur, key, &d);
        if (r < 0) return FAT32_ERR_IO;
        if (r == 0) return FAT32_ERR_NOENT;
        if (!dirent_is_dir(d.raw)) return FAT32_ERR_NOTDIR;

        uint32_t next = dirent_cluster(d.raw);
        cur = valid_cluster(next) ? next : vol.root_cluster;   /* ".." vers la racine vaut 0 */
        p = rest;
    }
}

/* Cherche l'entrée désignée par path. 1 trouvée, ou FAT32_ERR_*. Pour la
 * racine, retourne 2 (pas d'entrée de dossier). */
static int lookup_path(const char *path, dentry_t *out, uint32_t *parent_out) {
    uint32_t parent;
    const char *name;
    uint32_t len;
    int r = resolve_parent(path, &parent, &name, &len);
    if (r != FAT32_OK) return r;
    if (parent_out) *parent_out = parent;
    if (len == 0) return 2;   /* "/" */

        uint8_t key[11];
    if (encode_83(name, len, key, NULL) != 0) return FAT32_ERR_NOENT;

    r = dir_lookup(parent, key, out);
    if (r < 0) return FAT32_ERR_IO;
    return r == 1 ? 1 : FAT32_ERR_NOENT;
}

static int write_dirent(const dloc_t *loc, const uint8_t *raw) {
    if (sd_read_block(loc->lba, sec_buf) != 0) return -1;
    memcpy(&sec_buf[loc->off], raw, DIRENT_SIZE);
    return sd_write_block(loc->lba, sec_buf) == 0 ? 0 : -1;
}

/* Marque supprimées l'entrée et ses entrées de nom long. */
static int delete_dirent(const dentry_t *d) {
    for (uint32_t i = 0; i < d->lfn_count; i++) {
        if (sd_read_block(d->lfn[i].lba, sec_buf) != 0) return -1;
        sec_buf[d->lfn[i].off] = DIRENT_DELETED;
        if (sd_write_block(d->lfn[i].lba, sec_buf) != 0) return -1;
    }
    if (sd_read_block(d->loc.lba, sec_buf) != 0) return -1;
    sec_buf[d->loc.off] = DIRENT_DELETED;
    return sd_write_block(d->loc.lba, sec_buf) == 0 ? 0 : -1;
}

/* Trouve une entrée libre dans un dossier, en l'agrandissant d'un cluster
 * (remis à zéro) s'il est plein. 0, ou FAT32_ERR_*. */
static int dir_find_free(uint32_t dir_cluster, dloc_t *out) {
    uint32_t c = dir_cluster;
    uint32_t last = c;

    for (uint32_t n = 0; n < vol.total_clusters && valid_cluster(c); n++) {
        uint32_t base = cluster_to_lba(c);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (sd_read_block(base + s, sec_buf) != 0) return FAT32_ERR_IO;
            for (uint32_t off = 0; off < 512; off += DIRENT_SIZE) {
                if (sec_buf[off] == DIRENT_FREE || sec_buf[off] == DIRENT_DELETED) {
                    out->lba = base + s;
                    out->off = off;
                    return FAT32_OK;
                }
            }
        }
        last = c;
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC_MIN) break;
        if (!valid_cluster(next)) return FAT32_ERR_IO;
        c = next;
    }

    uint32_t fresh = alloc_cluster(last);
    if (fresh == 0) return FAT32_ERR_NOSPC;
    if (zero_cluster(fresh) != 0) return FAT32_ERR_IO;
    out->lba = cluster_to_lba(fresh);
    out->off = 0;
    return FAT32_OK;
}

static void make_dirent(uint8_t *e, const uint8_t key[11], uint8_t ntres, uint8_t attr, uint32_t cluster) {
    memset(e, 0, DIRENT_SIZE);
    memcpy(e, key, 11);
    e[11] = attr;
    e[12] = ntres;
    wr16(&e[14], FAT32_DEFAULT_TIME);
    wr16(&e[16], FAT32_DEFAULT_DATE);
    wr16(&e[18], FAT32_DEFAULT_DATE);
    wr16(&e[22], FAT32_DEFAULT_TIME);
    wr16(&e[24], FAT32_DEFAULT_DATE);
    dirent_set_cluster(e, cluster);
}

/* Crée une entrée dans le dossier parent. 0, ou FAT32_ERR_*. */
static int create_entry(uint32_t parent, const char *name, uint32_t len, uint8_t attr, uint32_t cluster, dloc_t *loc_out) {
    uint8_t key[11];
    uint8_t ntres;
    if (encode_83(name, len, key, &ntres) != 0) return FAT32_ERR_INVAL;
    if (key[0] == '.') return FAT32_ERR_INVAL;   /* "." et ".." sont réservés */

        dentry_t existing;
    int r = dir_lookup(parent, key, &existing);
    if (r < 0) return FAT32_ERR_IO;
    if (r == 1) return FAT32_ERR_EXIST;

    dloc_t loc;
    r = dir_find_free(parent, &loc);
    if (r != FAT32_OK) return r;

    uint8_t e[DIRENT_SIZE];
    make_dirent(e, key, ntres, attr, cluster);
    if (write_dirent(&loc, e) != 0) return FAT32_ERR_IO;

    if (loc_out) *loc_out = loc;

    return FAT32_OK;

}

/* ---- Informations -------------------------------------------------------- */

int fat32_stat(const char *path, uint32_t *out_size, int *out_is_dir) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;

    spinlock_acquire(&fat32_lock);
    dentry_t d;
    int r = lookup_path(path, &d, NULL);
    spinlock_release(&fat32_lock);

    if (r == 2) {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = 1;
        return FAT32_OK;
    }
    if (r != 1) return r;

    int dir = dirent_is_dir(d.raw);
    if (out_size) *out_size = dir ? 0 : rd32(&d.raw[28]);
    if (out_is_dir) *out_is_dir = dir;
    return FAT32_OK;
}

/* ---- Fichiers ------------------------------------------------------------ */

/* Fichier en cours de manipulation : entrée de dossier et position dans
 * la chaîne de clusters. Interne : l'interface publique travaille par
 * chemin, sans ouverture. */
typedef struct {
    uint32_t first_cluster;
    uint32_t size;
    uint32_t cached_cluster_index;   /* dernier cluster atteint dans la chaîne */
    uint32_t cached_cluster;
    uint32_t dirent_lba;             /* emplacement de l'entrée de dossier     */
    uint32_t dirent_off;
} fat32_node_t;

/* Cache du dernier fichier accédé : un vfs_stat suivi d'un read (chargeur
 * ELF), ou des ajouts successifs dans un journal, ne refont pas la
 * recherche dans les dossiers. Le cache garde l'entrée et le dernier
 * cluster atteint, ce qui évite aussi de reparcourir la chaîne depuis le
 * début pour chaque ajout. Il est mis à jour par les
 * écritures qui passent par lui, et invalidé par toute suppression ou
 * création de dossier. Un même fichier écrit sous une autre graphie du
 * chemin ("/a.txt" / "/A.TXT") remplace simplement l'entrée du cache. */
#define FAT32_CACHE_PATH_MAX 128u

static struct {
    int          valid;
    char         path[FAT32_CACHE_PATH_MAX];
    fat32_node_t node;
} cache;

static void cache_invalidate(void) {
    cache.valid = 0;
}

/* Fichier désigné par path, dans cache.node. Avec create, un fichier
 * absent est créé (vide). 0, ou FAT32_ERR_* (ISDIR pour un dossier). */
static int node_get(const char *path, int create, fat32_node_t **out) {
    if (cache.valid && strcmp(cache.path, path) == 0) {
        *out = &cache.node;
        return FAT32_OK;
    }
    cache.valid = 0;

    fat32_node_t *n = &cache.node;
    uint32_t parent;
    dentry_t d;
    int r = lookup_path(path, &d, &parent);
    if (r == 2) return FAT32_ERR_ISDIR;   /* la racine */

        if (r == 1) {
            if (dirent_is_dir(d.raw)) return FAT32_ERR_ISDIR;
            n->first_cluster = dirent_cluster(d.raw);
            n->size          = rd32(&d.raw[28]);
            n->dirent_lba    = d.loc.lba;
            n->dirent_off    = d.loc.off;
        } else if (r == FAT32_ERR_NOENT && create) {
            const char *name;
            uint32_t len;
            dloc_t loc;
            r = resolve_parent(path, &parent, &name, &len);
            if (r != FAT32_OK) return r;
            if (len == 0) return FAT32_ERR_ISDIR;
            r = create_entry(parent, name, len, ATTR_ARCHIVE, 0, &loc);
            if (r != FAT32_OK) return r;
            n->first_cluster = 0;
            n->size          = 0;
            n->dirent_lba    = loc.lba;
            n->dirent_off    = loc.off;
        } else {
            return r;
        }

        n->cached_cluster_index = 0;
        n->cached_cluster       = n->first_cluster;

        if (strlen(path) < FAT32_CACHE_PATH_MAX) {
            strcpy(cache.path, path);
            cache.valid = 1;
        }
        *out = n;
        return FAT32_OK;
}

/* Cluster d'indice index dans la chaîne du fichier (lecture). 0 si la
 * chaîne est plus courte. */
static uint32_t cluster_for_read(fat32_node_t *file, uint32_t index) {
    uint32_t c, i;
    if (file->cached_cluster_index <= index && valid_cluster(file->cached_cluster)) {
        c = file->cached_cluster;
        i = file->cached_cluster_index;
    } else {
        c = file->first_cluster;
        i = 0;
    }
    if (!valid_cluster(c)) return 0;

    while (i < index) {
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC_MIN || !valid_cluster(next)) return 0;
        c = next;
        i++;
    }
    file->cached_cluster_index = index;
    file->cached_cluster = c;
    return c;
}

/* Cluster d'indice index pour une écriture : la chaîne est prolongée (et
 * créée si le fichier est vide) autant que nécessaire. 0 si la carte est
 * pleine ou en cas d'erreur. */
static uint32_t cluster_for_write(fat32_node_t *file, uint32_t index) {
    if (!valid_cluster(file->first_cluster)) {
        uint32_t c = alloc_cluster(0);
        if (c == 0) return 0;
        file->first_cluster = c;
        file->cached_cluster_index = 0;
        file->cached_cluster = c;
    }

    uint32_t c, i;
    if (file->cached_cluster_index <= index && valid_cluster(file->cached_cluster)) {
        c = file->cached_cluster;
        i = file->cached_cluster_index;
    } else {
        c = file->first_cluster;
        i = 0;
    }

    while (i < index) {
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC_MIN || !valid_cluster(next)) {
            next = alloc_cluster(c);
            if (next == 0) return 0;
        }
        c = next;
        i++;
    }
    file->cached_cluster_index = index;
    file->cached_cluster = c;
    return c;
}

/* Écrit len octets à partir de offset (src NULL : des zéros). Retourne le
 * nombre d'octets écrits. */
static uint32_t write_at(fat32_node_t *file, uint32_t offset, const uint8_t *src, uint32_t len) {
    uint32_t bpc = bytes_per_cluster();
    uint32_t done = 0;

    while (done < len) {
        uint32_t abs_pos = offset + done;
        uint32_t cluster = cluster_for_write(file, abs_pos / bpc);
        if (cluster == 0) break;

        uint32_t in_cluster = abs_pos % bpc;
        uint32_t lba = cluster_to_lba(cluster) + in_cluster / 512u;
        uint32_t in_sector = in_cluster % 512u;

        uint32_t chunk = 512u - in_sector;
        if (chunk > len - done) chunk = len - done;

        /* Secteur partiel : lire, modifier, réécrire. Secteur complet :
         * inutile de le lire. */
        if (chunk != 512u && sd_read_block(lba, sec_buf) != 0) break;
        if (src) memcpy(sec_buf + in_sector, src + done, chunk);
        else     memset(sec_buf + in_sector, 0, chunk);
        if (sd_write_block(lba, sec_buf) != 0) break;

        done += chunk;
    }
    return done;
}

/* Reporte taille et premier cluster dans l'entrée de dossier du fichier. */
static int update_file_dirent(const fat32_node_t *file) {
    if (sd_read_block(file->dirent_lba, sec_buf) != 0) return -1;
    uint8_t *e = &sec_buf[file->dirent_off];
    dirent_set_cluster(e, valid_cluster(file->first_cluster) ? file->first_cluster : 0);
    wr32(&e[28], file->size);
    wr16(&e[22], FAT32_DEFAULT_TIME);
    wr16(&e[24], FAT32_DEFAULT_DATE);
    e[11] |= ATTR_ARCHIVE;
    return sd_write_block(file->dirent_lba, sec_buf) == 0 ? 0 : -1;
}

/* Copie len octets à partir de offset (dans les limites du fichier).
 * Retourne le nombre d'octets copiés. Verrou tenu par l'appelant. */
static uint32_t read_at(fat32_node_t *file, uint32_t offset, uint8_t *out, uint32_t len) {
    uint32_t bpc = bytes_per_cluster();
    uint32_t done = 0;

    while (done < len) {
        uint32_t abs_pos = offset + done;
        uint32_t cluster = cluster_for_read(file, abs_pos / bpc);
        if (cluster == 0) break;

        uint32_t in_cluster = abs_pos % bpc;
        uint32_t lba = cluster_to_lba(cluster) + in_cluster / 512u;
        uint32_t in_sector = in_cluster % 512u;

        uint32_t chunk = 512u - in_sector;
        if (chunk > len - done) chunk = len - done;

        /* Secteur complet : lu directement dans le tampon de l'appelant,
         * sans passer par sec_buf. */
        if (chunk == 512u) {
            if (sd_read_block(lba, out + done) != 0) break;
        } else {
            if (sd_read_block(lba, sec_buf) != 0) break;
            memcpy(out + done, sec_buf + in_sector, chunk);
        }
        done += chunk;
    }
    return done;
}

/* Fixe la taille du fichier : les clusters au-delà sont libérés, ou le
 * fichier est agrandi avec des zéros. Met à jour l'entrée de dossier.
 * 0, ou FAT32_ERR_*. Verrou tenu par l'appelant. */
static int resize(fat32_node_t *file, uint32_t size) {
    int r = FAT32_OK;
    if (size == file->size) return FAT32_OK;

    if (size > file->size) {
        uint32_t add = size - file->size;
        uint32_t z = write_at(file, file->size, NULL, add);
        file->size += z;
        if (z != add) r = FAT32_ERR_NOSPC;
    } else {
        /* Garder les clusters nécessaires, libérer la suite. */
        uint32_t bpc = bytes_per_cluster();
        uint32_t keep = size / bpc + (size % bpc != 0 ? 1u : 0u);

        if (keep == 0) {
            if (valid_cluster(file->first_cluster) && free_chain(file->first_cluster) != 0) r = FAT32_ERR_IO;
            file->first_cluster = 0;
        } else {
            uint32_t last = cluster_for_read(file, keep - 1u);
            if (last == 0) {
                r = FAT32_ERR_IO;
            } else {
                uint32_t next = fat_get(last);
                if (valid_cluster(next) && next < FAT32_EOC_MIN) {
                    if (fat_set(last, FAT32_EOC) != 0 || free_chain(next) != 0) r = FAT32_ERR_IO;
                }
            }
        }
        file->size = size;
        file->cached_cluster_index = 0;
        file->cached_cluster = file->first_cluster;
    }

    if (update_file_dirent(file) != 0) r = FAT32_ERR_IO;
    return r;
}

int fat32_read(const char *path, void *buf, uint32_t len) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;
    if (buf == NULL && len > 0) return FAT32_ERR_INVAL;

    spinlock_acquire(&fat32_lock);

    fat32_node_t *file;
    int r = node_get(path, 0, &file);
    if (r == FAT32_OK) {
        if (file->size > len) {
            r = FAT32_ERR_TOOBIG;            /* pas de lecture partielle */
        } else {
            uint32_t n = read_at(file, 0, (uint8_t *)buf, file->size);
            r = (n == file->size) ? (int)n : FAT32_ERR_IO;
        }
    }

    spinlock_release(&fat32_lock);
    return r;
}

int fat32_write(const char *path, const void *buf, uint32_t len) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;
    if (buf == NULL && len > 0) return FAT32_ERR_INVAL;

    spinlock_acquire(&fat32_lock);

    fat32_node_t *file;
    int ret = node_get(path, 1, &file);
    if (ret != FAT32_OK) goto out;

    /* Réécrire par-dessus l'ancien contenu réutilise ses clusters ; la
     * troncature libère ensuite ceux qui dépassent. Si la carte est pleine
     * en cours de route, le fichier est coupé après le dernier octet écrit. */
    uint32_t old_first = file->first_cluster;
    uint32_t n = write_at(file, 0, (const uint8_t *)buf, len);
    if (n < file->size) {
        ret = resize(file, n);                      /* coupe et met à jour l'entrée */
    } else {
        ret = FAT32_OK;
        if (n != file->size || file->first_cluster != old_first) {
            file->size = n;
            if (update_file_dirent(file) != 0) ret = FAT32_ERR_IO;
        }
    }
    if (ret == FAT32_OK && n != len) ret = FAT32_ERR_NOSPC;
    if (ret == FAT32_OK) ret = (int)n;
    else cache_invalidate();

    out:
    fsinfo_flush();
    spinlock_release(&fat32_lock);
    return ret;
}

int fat32_append(const char *path, const void *buf, uint32_t len) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;
    if (buf == NULL && len > 0) return FAT32_ERR_INVAL;

    spinlock_acquire(&fat32_lock);

    fat32_node_t *file;
    int ret = node_get(path, 1, &file);
    if (ret != FAT32_OK) goto out;

    /* Taille maximale d'un fichier FAT32 : 4 Go - 1. */
    if (len > 0xFFFFFFFFu - file->size) { ret = FAT32_ERR_NOSPC; goto out; }

    uint32_t n = write_at(file, file->size, (const uint8_t *)buf, len);
    if (n > 0) {
        file->size += n;
        if (update_file_dirent(file) != 0) {
            cache_invalidate();
            ret = FAT32_ERR_IO;
            goto out;
        }
    }
    ret = (n == len) ? (int)n : FAT32_ERR_NOSPC;

    out:
    fsinfo_flush();
    spinlock_release(&fat32_lock);
    return ret;
}

int fat32_truncate(const char *path, uint32_t size) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;

    spinlock_acquire(&fat32_lock);

    fat32_node_t *file;
    int r = node_get(path, 0, &file);
    if (r == FAT32_OK) {
        r = resize(file, size);
        if (r != FAT32_OK) cache_invalidate();
    }

    fsinfo_flush();
    spinlock_release(&fat32_lock);
    return r;
}

/* ---- Suppression et dossiers -------------------------------------------- */

int fat32_unlink(const char *path) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;

    spinlock_acquire(&fat32_lock);
    cache_invalidate();
    dentry_t d;
    int r = lookup_path(path, &d, NULL);
    if (r == 2) r = FAT32_ERR_ISDIR;
    if (r == 1) {
        if (dirent_is_dir(d.raw)) {
            r = FAT32_ERR_ISDIR;
        } else {
            /* L'entrée d'abord : une coupure entre les deux laisse des
             * clusters perdus (récupérables), jamais un fichier pointant
             * vers des clusters libres. */
            uint32_t first = dirent_cluster(d.raw);
            r = (delete_dirent(&d) == 0) ? FAT32_OK : FAT32_ERR_IO;
            if (r == FAT32_OK && valid_cluster(first) && free_chain(first) != 0) r = FAT32_ERR_IO;
        }
    }
    fsinfo_flush();
    spinlock_release(&fat32_lock);
    return r;
}

int fat32_mkdir(const char *path) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;

    spinlock_acquire(&fat32_lock);
    cache_invalidate();

    uint32_t parent;
    const char *name;
    uint32_t len;
    int r = resolve_parent(path, &parent, &name, &len);
    if (r != FAT32_OK) goto out;
    if (len == 0) { r = FAT32_ERR_EXIST; goto out; }   /* "/" */

        uint8_t key[11];
        if (encode_83(name, len, key, NULL) != 0 || key[0] == '.') { r = FAT32_ERR_INVAL; goto out; }
        dentry_t existing;
        r = dir_lookup(parent, key, &existing);
        if (r < 0) { r = FAT32_ERR_IO; goto out; }
        if (r == 1) { r = FAT32_ERR_EXIST; goto out; }

        /* 1. Le contenu du nouveau dossier ("." et ".."), avant de le rendre
         *    visible : une coupure laisse au pire un cluster perdu. */
        uint32_t c = alloc_cluster(0);
        if (c == 0) { r = FAT32_ERR_NOSPC; goto out; }
        if (zero_cluster(c) != 0) { (void)free_chain(c); r = FAT32_ERR_IO; goto out; }

        static const uint8_t dot[11]    = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        static const uint8_t dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        memset(sec_buf, 0, sizeof(sec_buf));
        make_dirent(&sec_buf[0], dot, 0, ATTR_DIRECTORY, c);
        /* ".." vers la racine s'écrit 0, par convention FAT. */
        make_dirent(&sec_buf[DIRENT_SIZE], dotdot, 0, ATTR_DIRECTORY,
                    parent == vol.root_cluster ? 0 : parent);
        if (sd_write_block(cluster_to_lba(c), sec_buf) != 0) { (void)free_chain(c); r = FAT32_ERR_IO; goto out; }

        /* 2. L'entrée dans le parent. */
        r = create_entry(parent, name, len, ATTR_DIRECTORY, c, NULL);
        if (r != FAT32_OK) (void)free_chain(c);

        out:
        fsinfo_flush();
        spinlock_release(&fat32_lock);
        return r;
}

/* 1 si le dossier ne contient que "." et "..", 0 sinon, -1 erreur. */
static int dir_is_empty(uint32_t dir_cluster) {
    uint32_t c = dir_cluster;
    for (uint32_t n = 0; n < vol.total_clusters && valid_cluster(c); n++) {
        uint32_t base = cluster_to_lba(c);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (sd_read_block(base + s, sec_buf) != 0) return -1;
            for (uint32_t off = 0; off < 512; off += DIRENT_SIZE) {
                const uint8_t *e = &sec_buf[off];
                if (e[0] == DIRENT_FREE) return 1;
                if (e[0] == DIRENT_DELETED) continue;
                if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) continue;
                return 0;
            }
        }
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC_MIN) break;
        c = next;
    }
    return 1;
}

int fat32_rmdir(const char *path) {
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;

    spinlock_acquire(&fat32_lock);
    cache_invalidate();
    dentry_t d;
    int r = lookup_path(path, &d, NULL);
    if (r == 2) r = FAT32_ERR_INVAL;   /* la racine */
        if (r == 1) {
            uint32_t c = dirent_cluster(d.raw);
            if (!dirent_is_dir(d.raw)) {
                r = FAT32_ERR_NOTDIR;
            } else if (d.raw[0] == '.') {
                r = FAT32_ERR_INVAL;       /* "." ou ".." */
            } else {
                int empty = valid_cluster(c) ? dir_is_empty(c) : 1;
                if (empty < 0)       r = FAT32_ERR_IO;
                else if (!empty)     r = FAT32_ERR_NOTEMPTY;
                else {
                    r = (delete_dirent(&d) == 0) ? FAT32_OK : FAT32_ERR_IO;
                    if (r == FAT32_OK && valid_cluster(c) && free_chain(c) != 0) r = FAT32_ERR_IO;
                }
            }
        }
        fsinfo_flush();
        spinlock_release(&fat32_lock);
        return r;
}

/* ---- Listing ------------------------------------------------------------- */

/* Nom affichable "NOM.EXT" (casse FAT respectée) à partir d'une entrée. */
static void format_name(const uint8_t *e, char out[13]) {
    uint32_t n = 0;
    int lower_base = (e[12] & NTRES_LOWER_BASE) != 0;
    int lower_ext  = (e[12] & NTRES_LOWER_EXT) != 0;

    for (uint32_t i = 0; i < 8 && e[i] != ' '; i++) {
        out[n++] = lower_base ? (char)to_lower(e[i]) : (char)e[i];
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (uint32_t i = 8; i < 11 && e[i] != ' '; i++) {
            out[n++] = lower_ext ? (char)to_lower(e[i]) : (char)e[i];
        }
    }
    out[n] = '\0';
}

int fat32_ls(const char *path, char ***out) {
    *out = NULL;
    if (!vol.mounted) return FAT32_ERR_NOTMOUNTED;
    if (path == NULL) return FAT32_ERR_INVAL;

    path_list_t pl;
    path_list_init(&pl);

    spinlock_acquire(&fat32_lock);

    uint32_t dir_cluster = vol.root_cluster;
    dentry_t d;
    int r = lookup_path(path, &d, NULL);
    if (r == 1) {
        if (!dirent_is_dir(d.raw)) { r = FAT32_ERR_NOTDIR; goto fail; }
        uint32_t c = dirent_cluster(d.raw);
        dir_cluster = valid_cluster(c) ? c : vol.root_cluster;
    } else if (r != 2) {
        goto fail;
    }

    uint32_t c = dir_cluster;
    for (uint32_t n = 0; n < vol.total_clusters && valid_cluster(c); n++) {
        uint32_t base = cluster_to_lba(c);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (sd_read_block(base + s, sec_buf) != 0) { r = FAT32_ERR_IO; goto fail; }

            for (uint32_t off = 0; off < 512; off += DIRENT_SIZE) {
                const uint8_t *e = &sec_buf[off];
                if (e[0] == DIRENT_FREE) goto done;        /* fin du dossier */
                    if (e[0] == DIRENT_DELETED) continue;
                    if (e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME_ID)) continue;
                    if (e[0] == '.') continue;                  /* "." et ".." */

                        char name[13];
                format_name(e, name);
                path_list_add(&pl, path, name, (uint32_t)strlen(name));
            }
        }
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC_MIN) break;
        c = next;
    }

    done:
    spinlock_release(&fat32_lock);
    r = path_list_finish(&pl, out);
    return r < 0 ? FAT32_ERR_NOSPC : r;

    fail:
    spinlock_release(&fat32_lock);
    path_list_discard(&pl);
    return r;
}
