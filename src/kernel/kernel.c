#include "src/kernel/kernel.h"

void echo(void) {
    UART0_CONF0_REG |= UART_RXFIFO_RST;
    UART0_CONF0_REG &= ~UART_RXFIFO_RST;
    UART0_INT_CLR_REG = UART_RX_ERROR_MASK;

    uart_print("Input > ");
    led_toggle();

    while (1) {
        uint32_t int_st = UART0_INT_ST_REG;
        if (int_st & UART_RX_ERROR_MASK) {
            UART0_INT_CLR_REG = UART_RX_ERROR_MASK;
            UART0_CONF0_REG |= UART_RXFIFO_RST;
            UART0_CONF0_REG &= ~UART_RXFIFO_RST;
            continue;
        }

        if (UART0_RX_FIFO_CNT > 0) {
            char c = (char)(UART0_FIFO & 0xFFu);

            if (c == '\r' || c == '\n') {
                uart0_putchar('\r');
                uart0_putchar('\n');
                uart_print("Input > ");
                led_toggle();
            } else if (c == '\b' || c == 0x7F) {
                uart_print("\b \b");
            } else {
                uart0_putchar(c);
            }
        }
    }
}

#define SD_SCLK 18
#define SD_MOSI 23
#define SD_MISO 19
#define SD_CS   5

static int spi2_write(const void *buf, uint32_t len) {
    spi_transfer((const uint8_t *)buf, NULL, len);
    return (int)len;
}

static int spi2_read(void *buf, uint32_t len) {
    spi_transfer(NULL, (uint8_t *)buf, len);
    return (int)len;
}

/* ---- Test d'écriture sur la carte SD -------------------------------------
 * Écrit un fichier, le relit, ajoute une ligne, crée des dossiers puis
 * affiche le résultat. Chaque étape affiche [OK] ou [ECHEC]. Les fichiers
 * restent sur la carte : on peut les vérifier ensuite sur un PC. */

#define TEST_FICHIER "/TEST.TXT"
#define TEST_DOSSIER "/VOID/LOGS"
#define TEST_JOURNAL "/VOID/LOGS/BOOT.LOG"

static int test_ok_count = 0, test_fail_count = 0;

static void test_resultat(int ok, const char *libelle) {
    uart_print(ok ? "  [OK]    " : "  [ECHEC] ");
    uart_print(libelle);
    uart_print("\n");
    if (ok) test_ok_count++; else test_fail_count++;
}

static void test_print_dec(uint32_t v) {
    char buf[12];
    uart_print(utoa_base(v, buf, 10));
}

/* Écrit len octets d'un coup ; retourne 1 si tout est passé. */
static int test_ecrire(const char *chemin, int options, const char *texte, uint32_t len) {
    vfs_file_t *f = vfs_open(chemin, options);
    if (f == NULL) return 0;
    int n = vfs_write(f, texte, len);
    vfs_close(f);
    return n == (int)len;
}

/* Relit le fichier dans buf (au plus max octets) ; retourne la taille lue
 * ou -1. */
static int test_relire(const char *chemin, char *buf, uint32_t max) {
    vfs_file_t *f = vfs_open(chemin, VFS_O_READ);
    if (f == NULL) return -1;
    uint32_t total = 0;
    while (total < max) {
        int n = vfs_read(f, buf + total, max - total);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    vfs_close(f);
    return (int)total;
}

static void test_ecriture_fichier(void) {
    static const char ligne1[] = "Bonjour depuis voidOS !\n";
    static const char ligne2[] = "Deuxieme ligne, ajoutee en fin de fichier.\n";
    static char lu[128];
    uint32_t taille = 0;

    uart_print("\n--- test d'ecriture sur la carte SD ---\n");

    /* 1. Création (ou remise à zéro) puis écriture */
    test_resultat(test_ecrire(TEST_FICHIER, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC,
                              ligne1, sizeof(ligne1) - 1),
                  "creation et ecriture de " TEST_FICHIER);

    /* 2. Taille et contenu relus */
    test_resultat(vfs_stat(TEST_FICHIER, &taille, NULL) == 0 && taille == sizeof(ligne1) - 1,
                  "taille sur la carte");
    int n = test_relire(TEST_FICHIER, lu, sizeof(lu));
    test_resultat(n == (int)(sizeof(ligne1) - 1) && memcmp(lu, ligne1, sizeof(ligne1) - 1) == 0,
                  "relecture identique");

    /* 3. Ajout en fin */
    test_resultat(test_ecrire(TEST_FICHIER, VFS_O_WRITE | VFS_O_APPEND,
                              ligne2, sizeof(ligne2) - 1),
                  "ajout d'une ligne en fin de fichier");
    n = test_relire(TEST_FICHIER, lu, sizeof(lu));
    test_resultat(n == (int)(sizeof(ligne1) + sizeof(ligne2) - 2) &&
    memcmp(lu + sizeof(ligne1) - 1, ligne2, sizeof(ligne2) - 1) == 0,
                  "fichier complet relu");

    /* 4. Dossiers imbriqués et fichier dedans */
    test_resultat(vfs_mkdir_p(TEST_DOSSIER) == 0 && vfs_is_dir(TEST_DOSSIER),
                  "creation de " TEST_DOSSIER);
    char journal[48];
    char nombre[12];
    strcpy(journal, "demarrage a ");
    strcat(journal, utoa_base((uint32_t)time_uptime_ms(), nombre, 10));
    strcat(journal, " ms\n");
    test_resultat(test_ecrire(TEST_JOURNAL, VFS_O_WRITE | VFS_O_CREATE | VFS_O_APPEND,
                              journal, (uint32_t)strlen(journal)),
                  "ajout dans " TEST_JOURNAL);

    /* 5. Affichage */
    n = test_relire(TEST_FICHIER, lu, sizeof(lu) - 1);
    if (n > 0) {
        lu[n] = '\0';
        uart_print("  contenu de " TEST_FICHIER " :\n");
        uart_print(lu);
    }
    uart_print("  racine de la carte :\n");
    vfs_ls("/");

    uart_print("  bilan : ");
    test_print_dec((uint32_t)test_ok_count);
    uart_print(" OK, ");
    test_print_dec((uint32_t)test_fail_count);
    uart_print(" ECHEC\n");
}

/* La tâche se termine après le test (retour = fin de tâche). */
static void tache_test_ecriture(void) {
    test_ecriture_fichier();
}

void kernel_main(void) {
    wdt_disable_all();
    mm_init();
    interrupts_init();
    time_init();          /* avant le second cœur : il arme son tick avec */
    scheduler_init();
    start_app_cpu();
    led_init();

    vfs_init();

    int sd = vfs_mount_sd(SD_SCLK, SD_MOSI, SD_MISO, SD_CS);
    if (sd == VFS_SD_NO_CARD) {
        uart_print("[init] pas de carte SD : le noyau tourne sans\n");
    } else if (sd == VFS_SD_BAD_FS) {
        uart_print("[init] carte illisible (formatee en FAT32 ?)\n");
    }

    if (vfs_mknod("/dev/uart0", uart0_read, uart0_write) != 0) {
        uart_print("[init] echec creation /dev/uart0\n");
    }
    if (vfs_mknod("/dev/spi2", spi2_read, spi2_write) != 0) {
        uart_print("[init] echec creation /dev/spi2\n");
    }

    if (sd == VFS_SD_OK) {
        task_create(tache_test_ecriture);
    }

    task_create(echo);

    scheduler_start();
}
