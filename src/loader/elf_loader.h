#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "src/types.h"
#include "src/file_system/vfs/vfs.h"
#include "src/scheduler/scheduler.h"
#include "src/utils/utils.h"
#include "src/memory_manager/memory.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "src/scheduler/spinlock.h"


#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define EM_XTENSA   94

#define EHDR_SIZE 52
#define SHDR_SIZE 40
#define RELA_SIZE 12

#define SHT_PROGBITS 1
#define SHT_NOBITS   8
#define SHT_RELA     4
#define SHF_ALLOC     0x2u
#define SHF_EXECINSTR 0x4u
#define R_XTENSA_32        1
#define R_XTENSA_SLOT0_OP 20

extern void uart_print(const char *str);
extern void uart_print_hex(uint32_t val);
extern uint32_t _user_iram_base, _user_iram_size;

typedef struct {
    void (*entry)(void);
    void *iram_block;
    void *dram_block;
} elf_image_t;

/* Fichier ELF en cours de chargement : lu en entier en mémoire (vfs_read
 * ne lit que des fichiers complets), libéré à la fin du chargement. */
typedef struct {
    uint8_t  *data;
    uint32_t  size;
} elf_src_t;

int elf_load_image(const char *path, elf_image_t *out);
void elf_unload(elf_image_t *img);
void elf_loader_print_layout(void);

#endif /* ELF_LOADER_H */
