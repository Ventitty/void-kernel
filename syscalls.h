#ifndef VOIDOS_ABI_SYSCALLS_H
#define VOIDOS_ABI_SYSCALLS_H

/* ---------------------------------------------------------------------------
 * Numéros et contrat des appels système de voidOS. Partagé avec la libc.
 *
 * Convention :
 *   - au plus 3 arguments, de la taille d'un registre (long, 32 bits) ;
 *   - retour >= 0 en cas de succès, -VOID_Exxx en cas d'échec (voir
 *     errno.h) ;
 *   - un pointeur passé au noyau par une tâche utilisateur doit désigner
 *     sa propre mémoire (pile ou zones déclarées à sa création), sinon
 *     l'appel échoue avec -VOID_EFAULT ;
 *   - un numéro inconnu retourne -VOID_ENOSYS.
 *
 * Les numéros ne sont JAMAIS réattribués : un appel retiré laisse un trou.
 * L'instruction d'appel elle-même est dans arch/<architecture>/syscall.h.
 * ------------------------------------------------------------------------- */

/* Version de l'ABI, retournée par VOID_SYS_ABI_VERSION. Incrémentée à
 * chaque changement incompatible du contrat ci-dessous. */
#define VOID_ABI_VERSION 1

/* Cède le processeur. Aucun argument. Retour : 0. */
#define VOID_SYS_YIELD         0

/* Affiche une chaîne sur la console. a0 = const char * terminée par '\0'
 * (au plus VOID_PRINT_MAX octets). Retour : 0, -VOID_EFAULT. */
#define VOID_SYS_PRINT         1

/* Termine la tâche appelante. Aucun argument. Ne retourne pas. */
#define VOID_SYS_EXIT          2

/* Dort au moins a0 millisecondes (0 : cède le processeur). Retour : 0. */
#define VOID_SYS_SLEEP         3

/* Retourne VOID_ABI_VERSION. Permet à la libc de vérifier au démarrage
 * qu'elle parle au bon noyau. */
#define VOID_SYS_ABI_VERSION   4

/* ---- Console ------------------------------------------------------------ */

/* Écrit des octets sur la console. a0 = const void *buf, a1 = longueur.
 * Au plus VOID_WRITE_MAX octets par appel : le retour est le nombre
 * d'octets écrits, éventuellement inférieur à la longueur demandée (la
 * libc boucle). Chaque '\n' est émis sous la forme "\r\n". Une écriture
 * n'est jamais entrecoupée par une autre.
 * Retour : octets écrits, -VOID_EFAULT. */
#define VOID_SYS_WRITE         5

/* Lit des octets reçus sur la console. a0 = void *buf, a1 = longueur,
 * a2 = options (VOID_READ_*). Par défaut, attend qu'au moins un octet soit
 * arrivé puis retourne ce qui est disponible, comme read() sur un
 * terminal. Retour : octets lus (>= 1, ou 0 si longueur nulle),
 * -VOID_EAGAIN (VOID_READ_NONBLOCK et rien de reçu), -VOID_EFAULT,
 * -VOID_EINVAL (option inconnue). */
#define VOID_SYS_READ          6

/* ---- Temps --------------------------------------------------------------- */

/* Écrit dans *a0 (unsigned long long *, aligné sur 4 octets) le nombre de
 * millisecondes écoulées depuis le démarrage. Retour : 0, -VOID_EFAULT. */
#define VOID_SYS_UPTIME_MS     7

/* ---- Mémoire ------------------------------------------------------------- */

/* Alloue a0 octets remis à zéro, réservés à la tâche appelante et rendus
 * automatiquement à sa terminaison. Chaque bloc coûte au moins 128 octets
 * de tas : demander de grandes tranches et les découper côté libc.
 * Retour : adresse du bloc, -VOID_EINVAL (taille nulle), -VOID_ENOMEM. */
#define VOID_SYS_MEM_ALLOC     8

/* Rend un bloc obtenu par VOID_SYS_MEM_ALLOC. a0 = adresse exacte du bloc.
 * Retour : 0, -VOID_EINVAL (pas un bloc de cette tâche). */
#define VOID_SYS_MEM_FREE      9

/* ---- Constantes ---------------------------------------------------------- */

/* Longueur maximale d'une chaîne passée à VOID_SYS_PRINT. */
#define VOID_PRINT_MAX 1024

/* Octets écrits au plus par un VOID_SYS_WRITE. L'écriture se fait sous le
 * verrou de la console, interruptions masquées : cette borne limite le
 * temps de masquage (~22 ms à 115200 bauds). */
#define VOID_WRITE_MAX 256

/* Options de VOID_SYS_READ. */
#define VOID_READ_NONBLOCK 0x1   /* ne pas attendre : -VOID_EAGAIN si vide */

#endif /* VOIDOS_ABI_SYSCALLS_H */
