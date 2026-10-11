#ifndef CONSOLE_H
#define CONSOLE_H

#include "src/types.h"

/* ---------------------------------------------------------------------------
 * Console du noyau, sur l'UART0.
 *
 * Toutes les écritures (noyau et appels système) passent par le même
 * verrou : une écriture n'est jamais entrecoupée par une autre, même
 * depuis l'autre cœur. Chaque '\n' est envoyé sous la forme "\r\n" pour
 * les terminaux série.
 * ------------------------------------------------------------------------- */

void uart_print(const char *str);
void uart_print_hex(uint32_t val);

/* Écrit len octets d'un seul tenant. Le verrou masque les interruptions
 * jusqu'au niveau 3 pendant l'écriture : garder len raisonnable (l'appel
 * système la limite à VOID_WRITE_MAX). Retourne len. */
uint32_t console_write(const char *buf, uint32_t len);

/* Lit au plus len octets déjà reçus, sans attendre. Retourne le nombre
 * d'octets lus (0 si rien n'est arrivé). */
uint32_t console_read(void *buf, uint32_t len);

#endif /* CONSOLE_H */
