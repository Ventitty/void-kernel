#include "src/loader/elf_loader.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

/* Charge tout le fichier path en mémoire. 0, ou -1. */
static int elf_src_open(const char *path, elf_src_t *out) {
    uint32_t size = 0;
    int dir = 1;
    out->data = NULL;
    out->size = 0;
    if (vfs_stat(path, &size, &dir) != 0 || dir || size < EHDR_SIZE) return -1;

    out->data = (uint8_t *)nmap(size);
    if (out->data == NULL) return -1;
    if (vfs_read(path, out->data, size) != (int)size) {
        unmap(out->data);
        out->data = NULL;
        return -1;
    }
    out->size = size;
    return 0;
}

static void elf_src_close(elf_src_t *s) {
    if (s->data != NULL) unmap(s->data);
    s->data = NULL;
    s->size = 0;
}

/* Copie exactement len octets depuis la position off. 0, ou -1 si la zone
 * déborde du fichier (ELF corrompu). */
static int src_read_at(elf_src_t *s, uint32_t off, void *buf, uint32_t len) {
    if (off > s->size || len > s->size - off) return -1;
    memcpy(buf, s->data + off, len);
    return 0;
}

static mem_pool_t code_pool;
static int code_pool_ready = 0;
static spinlock_t loader_lock = SPINLOCK_INIT;

static void code_pool_ensure(void) {
    spinlock_acquire(&loader_lock);
    if (!code_pool_ready) {
        uint32_t base = (uint32_t)&_user_iram_base;
        uint32_t size = (uint32_t)&_user_iram_size;
        pool_init(&code_pool, "zone de code (IRAM)", (void *)base, (void *)(base + size));
        code_pool_ready = 1;
    }
    spinlock_release(&loader_lock);
}

void elf_loader_print_layout(void) {
    code_pool_ensure();
    pool_print(&code_pool);
    pool_print(&kernel_heap);
}

#define MAX_SECTIONS 48

typedef struct {
    uint32_t type, flags, addr, offset, size, align, info;
    uint32_t delta;
    uint8_t  loaded;
} sec_t;

static void write32(uint32_t addr, uint32_t value) { *(volatile uint32_t *)addr = value; }
static uint32_t read32(uint32_t addr) { return *(volatile uint32_t *)addr; }

static int load_section(elf_src_t *src, const sec_t *s, uint32_t dst) {
    if (s->type == SHT_NOBITS) {                       /* .bss : mise à zéro */
        for (uint32_t o = 0; o < s->size; o += 4) write32(dst + o, 0);
        return 0;
    }
    uint32_t done = 0;
    while (done < s->size) {
        uint8_t buf[256];
        uint32_t chunk = s->size - done;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        memset(buf, 0, sizeof(buf));
        if (src_read_at(src, s->offset + done, buf, chunk) != 0) return -1;

        uint32_t words = (chunk + 3u) / 4u;
        const uint32_t *b32 = (const uint32_t *)buf;
        for (uint32_t w = 0; w < words; w++) write32(dst + done + w * 4u, b32[w]);
        done += chunk;
    }
    return 0;
}

static const sec_t *section_of_addr(const sec_t *secs, uint32_t n, uint32_t addr) {
    for (uint32_t i = 0; i < n; i++) {
        if (!secs[i].loaded) continue;
        if (addr >= secs[i].addr && addr < secs[i].addr + secs[i].size) return &secs[i];
    }
    return NULL;
}

int elf_load_image(const char *path, elf_image_t *out) {
    elf_src_t src;
    if (out == NULL || elf_src_open(path, &src) != 0) {
        uart_print("[elf] fichier introuvable ou trop gros : "); uart_print(path); uart_print("\n");
        return -1;
    }

    int ret = -1;
    sec_t *secs = NULL;
    void *iram_block = NULL;
    void *dram_block = NULL;

    uint8_t ehdr[EHDR_SIZE];
    if (src_read_at(&src, 0, ehdr, EHDR_SIZE) != 0) { uart_print("[elf] en-tete illisible\n"); goto done; }

    if (ehdr[0] != 0x7Fu || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F' ||
        ehdr[EI_CLASS] != ELFCLASS32 || ehdr[EI_DATA] != ELFDATA2LSB ||
        rd16(&ehdr[18]) != EM_XTENSA) {
        uart_print("[elf] pas un ELF32 Xtensa little-endian\n");
    goto done;
        }

        uint32_t e_entry     = rd32(&ehdr[24]);
        uint32_t e_shoff     = rd32(&ehdr[32]);
        uint16_t e_shentsize = rd16(&ehdr[46]);
        uint16_t e_shnum     = rd16(&ehdr[48]);

        if (e_shoff == 0 || e_shentsize != SHDR_SIZE || e_shnum == 0 || e_shnum > MAX_SECTIONS) {
            uart_print("[elf] table des sections absente ou trop grande -- compile avec -Wl,-q\n");
            goto done;
        }

        secs = (sec_t *)nmap(e_shnum * sizeof(sec_t));
        if (secs == NULL) { uart_print("[elf] memoire insuffisante\n"); goto done; }

        uint32_t text_min = 0xFFFFFFFFu, text_max = 0, text_align = 4;
        uint32_t data_min = 0xFFFFFFFFu, data_max = 0, data_align = 4;
        int has_reloc = 0;

        for (uint16_t i = 0; i < e_shnum; i++) {
            uint8_t sh[SHDR_SIZE];
            if (src_read_at(&src, e_shoff + (uint32_t)i * SHDR_SIZE, sh, SHDR_SIZE) != 0) goto done;

            secs[i].type   = rd32(&sh[4]);
            secs[i].flags  = rd32(&sh[8]);
            secs[i].addr   = rd32(&sh[12]);
            secs[i].offset = rd32(&sh[16]);
            secs[i].size   = rd32(&sh[20]);
            secs[i].info   = rd32(&sh[28]);
            secs[i].align  = rd32(&sh[32]);
            secs[i].delta  = 0;
            secs[i].loaded = 0;

            if (secs[i].type == SHT_RELA) has_reloc = 1;
            if (!(secs[i].flags & SHF_ALLOC) || secs[i].size == 0) continue;

            secs[i].loaded = 1;
            uint32_t a = secs[i].addr, e = a + secs[i].size;
            if (secs[i].flags & SHF_EXECINSTR) {
                if (a < text_min) text_min = a;
                if (e > text_max) text_max = e;
                if (secs[i].align > text_align) text_align = secs[i].align;
            } else {
                if (a < data_min) data_min = a;
                if (e > data_max) data_max = e;
                if (secs[i].align > data_align) data_align = secs[i].align;
            }
        }

        if (!has_reloc) {
            uart_print("[elf] aucune relocation : recompile avec -Wl,-q, sinon le programme\n");
            uart_print("[elf] ne peut etre charge qu'a son adresse de liaison\n");
            goto done;
        }
        if (text_max <= text_min) { uart_print("[elf] aucune section de code\n"); goto done; }

        uint32_t text_size = text_max - text_min;
        code_pool_ensure();
        iram_block = pool_alloc(&code_pool, text_size + text_align);
        if (iram_block == NULL) {
            uart_print("[elf] plus de place dans la zone de code\n");
            goto done;
        }
        uint32_t iram_base = ((uint32_t)iram_block + text_align - 1) & ~(text_align - 1);

        uint32_t data_size = (data_max > data_min) ? (data_max - data_min) : 0;
        uint32_t data_base = 0;
        if (data_size > 0) {
            dram_block = nmap(data_size + data_align);
            if (dram_block == NULL) { uart_print("[elf] memoire insuffisante (donnees)\n"); goto done; }
            data_base = ((uint32_t)dram_block + data_align - 1) & ~(data_align - 1);
        }

        uint32_t text_delta = iram_base - text_min;
        uint32_t data_delta = (data_size > 0) ? (data_base - data_min) : 0;

        for (uint16_t i = 0; i < e_shnum; i++) {
            if (!secs[i].loaded) continue;
            secs[i].delta = (secs[i].flags & SHF_EXECINSTR) ? text_delta : data_delta;
            if (load_section(&src, &secs[i], secs[i].addr + secs[i].delta) != 0) {
                uart_print("[elf] lecture d'une section echouee\n");
                goto done;
            }
        }

        uint32_t patched = 0;
        for (uint16_t i = 0; i < e_shnum; i++) {
            if (secs[i].type != SHT_RELA) continue;
            if (secs[i].info >= e_shnum || !secs[secs[i].info].loaded) continue;

            const sec_t *target = &secs[secs[i].info];
            for (uint32_t off = 0; off + RELA_SIZE <= secs[i].size; off += RELA_SIZE) {
                uint8_t ent[RELA_SIZE];
                if (src_read_at(&src, secs[i].offset + off, ent, RELA_SIZE) != 0) goto done;

                uint32_t r_offset = rd32(&ent[0]);
                uint32_t r_type   = rd32(&ent[4]) & 0xFFu;

                if (r_type == R_XTENSA_SLOT0_OP) continue;
                if (r_type != R_XTENSA_32) continue;

                uint32_t loc = r_offset + target->delta;
                if ((loc & 3u) != 0) {
                    uart_print("[elf] relocation non alignee -- abandon\n");
                    goto done;
                }

                uint32_t old = read32(loc);
                const sec_t *pointee = section_of_addr(secs, e_shnum, old);
                if (pointee == NULL) continue;
                write32(loc, old + pointee->delta);
                patched++;
            }
        }

        const sec_t *entry_sec = section_of_addr(secs, e_shnum, e_entry);
        if (entry_sec == NULL) { uart_print("[elf] point d'entree hors des sections chargees\n"); goto done; }

        cpu_sync_code();

        out->entry      = (void (*)(void))(e_entry + entry_sec->delta);
        out->iram_block = iram_block;
        out->dram_block = dram_block;

        uart_print("[elf] "); uart_print(path);
        uart_print(" : code en "); uart_print_hex(iram_base);
        uart_print(", donnees en "); uart_print_hex(data_base);
        uart_print(", "); uart_print_hex(patched); uart_print(" adresses corrigees\n");

        iram_block = NULL; dram_block = NULL;
        ret = 0;

        done:
        if (iram_block != NULL) pool_free(&code_pool, iram_block);
        if (dram_block != NULL) unmap(dram_block);
        if (secs != NULL) unmap(secs);
        elf_src_close(&src);
    return ret;
}

void elf_unload(elf_image_t *img) {
    if (img == NULL) return;

    if (img->iram_block != NULL) unmap(img->iram_block);
    if (img->dram_block != NULL) unmap(img->dram_block);
    img->entry = NULL;
    img->iram_block = NULL;
    img->dram_block = NULL;
}
