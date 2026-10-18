#ifndef SDCARD_H
#define SDCARD_H

#include "src/types.h"
#include "arch/xtensa_lx6/drivers/esp32_spi/spi.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "src/time/time.h"

#define SD_BLOCK_SIZE 512u

/* Durée maximale de programmation d'un bloc après une écriture. */
#define SD_WRITE_BUSY_TIMEOUT_MS 500u

/* Définir SD_DEBUG (-DSD_DEBUG) pour afficher le diagnostic de chaque
 * écriture de bloc. Désactivé par défaut : quatre lignes par secteur
 * ralentiraient fortement l'écriture des fichiers. */

int sd_init(uint8_t sclk, uint8_t mosi, uint8_t miso, uint8_t cs);
int sd_read_block(uint32_t block_addr, uint8_t *buf);
int sd_write_block(uint32_t block_addr, const uint8_t *buf);

#endif /* SDCARD_H */
