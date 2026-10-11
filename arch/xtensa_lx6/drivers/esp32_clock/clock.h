#ifndef CLOCK_H
#define CLOCK_H

#include "src/types.h"

/* ---------------------------------------------------------------------------
 * Mesure de la fréquence CPU.
 *
 * Le noyau ne configure pas l'horloge : il hérite de celle laissée par la
 * ROM de démarrage, dont la fréquence n'est écrite nulle part. On la mesure
 * en chronométrant (en cycles CPU) l'émission d'octets sur l'UART0, dont le
 * débit, lui, est connu : la ROM l'a réglé à CLOCK_UART_BAUD, 8N1, soit 10
 * bits par octet. C'est la même UART que celle de la console : si le texte
 * s'affiche correctement, la référence est bonne.
 * ------------------------------------------------------------------------- */

#define CLOCK_UART_BAUD      115200u
#define CLOCK_BITS_PER_BYTE  10u          /* start + 8 données + stop */
#define CLOCK_TIMED_BYTES    32u          /* octets chronométrés      */

/* Hz par cycle mesuré : baud / (octets * bits). Doit être entier, pour
 * que le calcul se fasse sans division 64 bits (voir clock.c). Avec les
 * valeurs ci-dessus : 115200 / 320 = 360. */
#define CLOCK_HZ_PER_CYCLE   (CLOCK_UART_BAUD / (CLOCK_TIMED_BYTES * CLOCK_BITS_PER_BYTE))

/* Fréquence retenue si la mesure échoue. */
#define CLOCK_DEFAULT_HZ     160000000u

/* Écrit label puis CLOCK_TIMED_BYTES points sur l'UART0 en chronométrant
 * leur émission, et retourne la fréquence CPU en Hz (arrondie à une
 * fréquence standard, voir clock_snap_hz), ou 0 si la mesure a échoué.
 * À appeler au démarrage, sur un seul cœur, sans autre écriture UART en
 * cours. label ne doit pas être vide. N'écrit pas de fin de ligne. */
uint32_t clock_measure_cpu_hz(const char *label);

/* Ramène une mesure brute à la fréquence standard la plus proche
 * (240, 160, 80, 40, 26, 20 ou 10 MHz) si elle en est à moins de 4 %,
 * sinon l'arrondit au MHz. Fonction pure, sans accès matériel. */
uint32_t clock_snap_hz(uint32_t measured_hz);

#endif /* CLOCK_H */
