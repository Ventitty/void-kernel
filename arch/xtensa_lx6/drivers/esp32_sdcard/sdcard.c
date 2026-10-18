#include "arch/xtensa_lx6/drivers/esp32_sdcard/sdcard.h"
#include "arch/xtensa_lx6/drivers/esp32_gpio/gpio.h"
#include "src/memory_manager/memory.h"
#include "src/utils/utils.h"

extern void uart_print(const char *str);
extern void uart_print_hex(uint32_t val);

#define CMD0   0    /* GO_IDLE_STATE                          */
#define CMD1   1    /* SEND_OP_COND (MMC classique, repli)    */
#define CMD8   8    /* SEND_IF_COND                           */
#define CMD9   9    /* SEND_CSD                               */
#define CMD16  16   /* SET_BLOCKLEN                           */
#define CMD17  17   /* READ_SINGLE_BLOCK                      */
#define CMD24  24   /* WRITE_BLOCK                            */
#define CMD55  55   /* APP_CMD (préfixe pour les ACMDxx)      */
#define CMD58  58   /* READ_OCR                               */
#define ACMD41 41   /* SD_SEND_OP_COND (après CMD55)          */

#define R1_IDLE          0x01u

#define DATA_TOKEN_START 0xFEu

#define R1_WAIT_BYTES     16u
#define TOKEN_WAIT_BYTES  512u

static int is_sdhc = 0;
static uint8_t g_cs_pin = 0;

static void sd_cs_select(void)   { gpio_write(g_cs_pin, 0); }
static void sd_cs_deselect(void) { gpio_write(g_cs_pin, 1); }

static void sd_xfer_chunks(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = (len - offset > 64) ? 64 : (len - offset);
        spi_transfer(tx ? tx + offset : 0, rx ? rx + offset : 0, chunk);
        offset += chunk;
    }
}

static void sd_xfer_cmd(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    sd_cs_select();
    sd_xfer_chunks(tx, rx, len);
    sd_cs_deselect();
}

static uint8_t crc7(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t d = data[i];
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80) crc ^= 0x09;
            d <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1u);
}

static int scan_response(const uint8_t *rx, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) {
        if ((rx[i] & 0x80) == 0) return (int)i;
    }
    return -1;
}

static int sd_command(uint8_t cmd, uint32_t arg, uint8_t *resp, int resp_len) {
    uint8_t tx[6 + R1_WAIT_BYTES];
    uint8_t rx[6 + R1_WAIT_BYTES];

    uint8_t frame[5] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
    };

    tx[0] = frame[0]; tx[1] = frame[1]; tx[2] = frame[2]; tx[3] = frame[3]; tx[4] = frame[4];
    tx[5] = crc7(frame, 5);
    for (uint32_t i = 6; i < sizeof(tx); i++) tx[i] = 0xFF;

    sd_xfer_cmd(tx, rx, sizeof(tx));

    int start = scan_response(rx, 6, sizeof(rx));
    if (start < 0) return -1;

    for (int i = 0; i < resp_len; i++) {
        int idx = start + i;
        resp[i] = (idx < (int)sizeof(rx)) ? rx[idx] : 0xFF;
    }
    return 0;
}

#define SD_APPCMD_CMD1_END  6u
#define SD_APPCMD_GAP1_END  (SD_APPCMD_CMD1_END + R1_WAIT_BYTES)
#define SD_APPCMD_CMD2_END  (SD_APPCMD_GAP1_END + 6u)
#define SD_APPCMD_GAP2_END  (SD_APPCMD_CMD2_END + R1_WAIT_BYTES)

static int sd_app_command(uint8_t acmd, uint32_t arg, uint8_t *resp, int resp_len, uint8_t *cmd55_r1_out) {
    uint8_t tx[SD_APPCMD_GAP2_END];
    uint8_t rx[SD_APPCMD_GAP2_END];

    uint8_t f1[5] = { (uint8_t)(0x40 | CMD55), 0, 0, 0, 0 };
    uint8_t f2[5] = {
        (uint8_t)(0x40 | acmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
    };

    tx[0] = f1[0]; tx[1] = f1[1]; tx[2] = f1[2]; tx[3] = f1[3]; tx[4] = f1[4];
    tx[5] = crc7(f1, 5);
    for (uint32_t i = SD_APPCMD_CMD1_END; i < SD_APPCMD_GAP1_END; i++) tx[i] = 0xFF;

    tx[SD_APPCMD_GAP1_END + 0] = f2[0]; tx[SD_APPCMD_GAP1_END + 1] = f2[1];
    tx[SD_APPCMD_GAP1_END + 2] = f2[2]; tx[SD_APPCMD_GAP1_END + 3] = f2[3];
    tx[SD_APPCMD_GAP1_END + 4] = f2[4];
    tx[SD_APPCMD_GAP1_END + 5] = crc7(f2, 5);
    for (uint32_t i = SD_APPCMD_CMD2_END; i < SD_APPCMD_GAP2_END; i++) tx[i] = 0xFF;

    sd_xfer_cmd(tx, rx, SD_APPCMD_GAP2_END);

    if (cmd55_r1_out) {
        int p55 = scan_response(rx, SD_APPCMD_CMD1_END, SD_APPCMD_GAP1_END);
        *cmd55_r1_out = (p55 >= 0) ? rx[p55] : 0xFF;
    }

    int pos = scan_response(rx, SD_APPCMD_CMD2_END, SD_APPCMD_GAP2_END);
    if (pos < 0) return -1;

    for (int i = 0; i < resp_len; i++) {
        int idx = pos + i;
        resp[i] = (idx < (int)SD_APPCMD_GAP2_END) ? rx[idx] : 0xFF;
    }
    return 0;
}

int sd_init(uint8_t sclk, uint8_t mosi, uint8_t miso, uint8_t cs) {
    g_cs_pin = cs;

    spi_config_t cfg = {
        .sclk = sclk, .mosi = mosi, .miso = miso, .cs = cs,
        .clock_div = 200, .manual_cs = 1,
    };
    spi_init(&cfg);

    sd_cs_deselect();
    uint8_t dummy_tx[10], dummy_rx[10];
    for (int i = 0; i < 10; i++) dummy_tx[i] = 0xFF;
    sd_xfer_chunks(dummy_tx, dummy_rx, 10);

    uint8_t r1;
    if (sd_command(CMD0, 0, &r1, 1) != 0) {
        uart_print("[sd][diag] CMD0 : aucune reponse recue\n");
        return -1;
    }
    if (r1 != R1_IDLE) {
        uart_print("[sd][diag] CMD0 : reponse R1="); uart_print_hex(r1);
        uart_print(" (attendu 0x01)\n");
        return -1;
    }

    uint8_t r7[5];
    int is_v2 = 0;
    if (sd_command(CMD8, 0x1AAu, r7, 5) == 0 && r7[0] == R1_IDLE) {
        if (r7[3] == 0x01 && r7[4] == 0xAA) is_v2 = 1;
        uart_print("[sd][diag] CMD8 : R1="); uart_print_hex(r7[0]);
        uart_print(" echo="); uart_print_hex(r7[3]); uart_print_hex(r7[4]);
        uart_print(is_v2 ? " -> SDv2 detectee\n" : " -> echo invalide, traite comme SDv1\n");
    } else {
        uart_print("[sd][diag] CMD8 : pas de reponse valide (R1="); uart_print_hex(r7[0]);
        uart_print(") -> traite comme SDv1/MMC\n");
    }

    uint32_t acmd41_arg = is_v2 ? 0x40000000u : 0u;
    int idle_timeout = 10000;
    uint8_t cmd55_r1 = 0xFF;
    int switched_arg = 0;
    do {
        if (sd_app_command(ACMD41, acmd41_arg, &r1, 1, &cmd55_r1) != 0) {
            uart_print("[sd][diag] ACMD41 : aucune reponse recue (CMD55 R1=");
            uart_print_hex(cmd55_r1); uart_print(")\n");
            return -1;
        }
        if (!(r1 & R1_IDLE)) break;

        if (!switched_arg && idle_timeout == 5000) {
            acmd41_arg = acmd41_arg ? 0u : 0x40000000u;
            switched_arg = 1;
            uart_print("[sd][diag] ACMD41 : toujours idle a mi-parcours, essai avec argument=");
            uart_print_hex(acmd41_arg); uart_print("\n");
        }
    } while (--idle_timeout > 0);

    if (idle_timeout <= 0) {
        uart_print("[sd][diag] ACMD41 : echec definitif (CMD55 R1="); uart_print_hex(cmd55_r1);
        uart_print(", ACMD41 R1="); uart_print_hex(r1);
        uart_print("). Repli sur CMD1 (mode MMC classique)...\n");

        int mmc_timeout = 10000;
        do {
            if (sd_command(CMD1, 0, &r1, 1) != 0) {
                uart_print("[sd][diag] CMD1 : aucune reponse recue\n");
                return -1;
            }
        } while ((r1 & R1_IDLE) && --mmc_timeout > 0);

        if (mmc_timeout <= 0) {
            uart_print("[sd][diag] CMD1 : echec egalement, R1="); uart_print_hex(r1);
            uart_print(" -- carte non reconnue (SD ni MMC), verifie le cablage/l'alimentation/la carte elle-meme.\n");
            return -1;
        }

        uart_print("[sd][diag] Carte reconnue en mode MMC classique (CMD1) apres echec ACMD41.\n");
        is_v2 = 0;
    }
    uart_print("[sd][diag] ACMD41 : carte prete, R1 final="); uart_print_hex(r1); uart_print("\n");

    is_sdhc = 0;
    if (is_v2) {
        uint8_t ocr[5];
        if (sd_command(CMD58, 0, ocr, 5) != 0) {
            uart_print("[sd][diag] CMD58 : aucune reponse recue\n");
            return -1;
        }
        is_sdhc = (ocr[1] & 0x40) != 0;
        uart_print("[sd][diag] CMD58 : OCR[1]="); uart_print_hex(ocr[1]);
        uart_print(is_sdhc ? " -> carte SDHC/SDXC (adressage par bloc)\n"
        : " -> carte SDSC (adressage par octet)\n");
    }

    if (!is_sdhc) {
        if (sd_command(CMD16, SD_BLOCK_SIZE, &r1, 1) != 0 || r1 != 0) return -1;
    }

    cfg.clock_div = 4;
    spi_init(&cfg);

    return 0;
}

int sd_read_block(uint32_t block_addr, uint8_t *buf) {
    uint32_t addr = is_sdhc ? block_addr : block_addr * SD_BLOCK_SIZE;

    uint32_t total = 6 + R1_WAIT_BYTES + TOKEN_WAIT_BYTES + SD_BLOCK_SIZE + 2;

    uint8_t *tx = (uint8_t *)nmap(total);
    uint8_t *rx = (uint8_t *)nmap(total);
    if (!tx || !rx) return -1;

    uint8_t frame[5] = {
        (uint8_t)(0x40 | CMD17),
        (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
    };
    tx[0] = frame[0]; tx[1] = frame[1]; tx[2] = frame[2]; tx[3] = frame[3]; tx[4] = frame[4];
    tx[5] = crc7(frame, 5);
    for (uint32_t i = 6; i < total; i++) tx[i] = 0xFF;

    sd_cs_select();
    sd_xfer_chunks(tx, rx, total);
    sd_cs_deselect();

    int pos = scan_response(rx, 6, 6 + R1_WAIT_BYTES);
    int ret = -1;
    if (pos >= 0 && rx[pos] == 0x00) {
        int token_pos = -1;
        for (uint32_t i = (uint32_t)pos + 1; i < total; i++) {
            if (rx[i] == DATA_TOKEN_START) { token_pos = (int)i; break; }
        }
        if (token_pos >= 0 && (uint32_t)token_pos + 1 + SD_BLOCK_SIZE <= total) {
            memcpy(buf, rx + token_pos + 1, SD_BLOCK_SIZE);
            ret = 0;
        }
    }

    unmap(tx);
    unmap(rx);
    return ret;
}

int sd_write_block(uint32_t block_addr, const uint8_t *buf) {
    uint32_t addr = is_sdhc ? block_addr : block_addr * SD_BLOCK_SIZE;

    uint32_t total = 6 + R1_WAIT_BYTES + 1 + SD_BLOCK_SIZE + 2 + 1;

    uint8_t *tx = (uint8_t *)nmap(total);
    uint8_t *rx = (uint8_t *)nmap(total);
    if (!tx || !rx) return -1;

    uint8_t frame[5] = {
        (uint8_t)(0x40 | CMD24),
        (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
    };
    tx[0] = frame[0]; tx[1] = frame[1]; tx[2] = frame[2]; tx[3] = frame[3]; tx[4] = frame[4];
    tx[5] = crc7(frame, 5);
    for (uint32_t i = 6; i < 6 + R1_WAIT_BYTES; i++) tx[i] = 0xFF;

    uint32_t token_pos = 6 + R1_WAIT_BYTES;
    tx[token_pos] = DATA_TOKEN_START;
    memcpy(tx + token_pos + 1, buf, SD_BLOCK_SIZE);
    tx[token_pos + 1 + SD_BLOCK_SIZE] = 0xFF;
    tx[token_pos + 1 + SD_BLOCK_SIZE + 1] = 0xFF;
    tx[token_pos + 1 + SD_BLOCK_SIZE + 2] = 0xFF;

    sd_cs_select();
    sd_xfer_chunks(tx, rx, total);
    sd_cs_deselect();

    int r1_pos = scan_response(rx, 6, 6 + R1_WAIT_BYTES);
    int ret = -1;
    if (r1_pos >= 0 && rx[r1_pos] == 0x00) {
        uint8_t data_resp = rx[token_pos + 1 + SD_BLOCK_SIZE + 2];
        if ((data_resp & 0x1Fu) == 0x05u) {
            ret = 0;
        }
    }
    #ifdef SD_DEBUG
    uart_print("[sd][diag] write: r1_pos="); uart_print_hex((uint32_t)r1_pos);
    uart_print(" ret="); uart_print_hex((uint32_t)ret); uart_print("\n");
    #endif

    unmap(tx);
    unmap(rx);

    if (ret != 0) return -1;

    /* La carte tient MISO à 0 tant qu'elle programme le bloc. La norme SD
     * autorise jusqu'à 250 ms (500 ms pour les SDXC) : délai mesuré en
     * temps réel plutôt qu'en nombre d'interrogations, qui dépendrait de
     * la fréquence du CPU et du SPI. */
    const uint32_t budget = (time_cpu_hz() / 1000u) * SD_WRITE_BUSY_TIMEOUT_MS;
    const uint32_t start = cpu_read_ccount();
    do {
        uint8_t poll_tx[1] = { 0xFF };
        uint8_t poll_rx[1] = { 0 };
        sd_xfer_cmd(poll_tx, poll_rx, 1);
        if (poll_rx[0] != 0x00) return 0;
    } while ((uint32_t)(cpu_read_ccount() - start) < budget);

        #ifdef SD_DEBUG
        uart_print("[sd][diag] write: TIMEOUT busy.\n");
        #endif
        return -1;
}
