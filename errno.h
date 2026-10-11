#ifndef VOIDOS_ABI_ERRNO_H
#define VOIDOS_ABI_ERRNO_H

/* ---------------------------------------------------------------------------
 * Codes d'erreur des appels système de voidOS. Partagé avec la libc.
 *
 * Un appel système qui échoue retourne -VOID_Exxx (valeur négative entre
 * -4095 et -1). La libc la convertit en errno = VOID_Exxx et retourne -1
 * à l'appelant, comme sous Linux. Les numéros sont ceux de Linux : une
 * libc peut définir ENOENT etc. directement à partir de ces valeurs.
 *
 * Aucune dépendance : utilisable par le noyau comme par la libc.
 * ------------------------------------------------------------------------- */

#define VOID_EPERM         1   /* opération non permise                    */
#define VOID_ENOENT        2   /* fichier ou dossier inexistant            */
#define VOID_EINTR         4   /* appel interrompu                         */
#define VOID_EIO           5   /* erreur d'entrée/sortie                   */
#define VOID_EBADF         9   /* descripteur ou handle invalide           */
#define VOID_EAGAIN       11   /* ressource temporairement indisponible    */
#define VOID_ENOMEM       12   /* mémoire insuffisante                     */
#define VOID_EFAULT       14   /* adresse invalide passée au noyau         */
#define VOID_EBUSY        16   /* ressource occupée                        */
#define VOID_EEXIST       17   /* existe déjà                              */
#define VOID_ENOTDIR      20   /* n'est pas un dossier                     */
#define VOID_EISDIR       21   /* est un dossier                           */
#define VOID_EINVAL       22   /* argument invalide                        */
#define VOID_EMFILE       24   /* trop de fichiers ouverts                 */
#define VOID_ENOSPC       28   /* plus de place                            */
#define VOID_ESPIPE       29   /* positionnement impossible (flux)         */
#define VOID_EROFS        30   /* système de fichiers en lecture seule     */
#define VOID_ERANGE       34   /* résultat hors limites                    */
#define VOID_ENAMETOOLONG 36   /* nom trop long                            */
#define VOID_ENOSYS       38   /* appel système inexistant                 */
#define VOID_ENOTEMPTY    39   /* dossier non vide                         */
#define VOID_ETIMEDOUT   110   /* délai dépassé                            */

/* Plus grand code d'erreur : un retour r est une erreur si et seulement
 * si -VOID_EMAXERRNO <= r <= -1. */
#define VOID_EMAXERRNO  4095

#define VOID_IS_ERROR(r) ((unsigned long)(r) >= (unsigned long)-VOID_EMAXERRNO)

#endif /* VOIDOS_ABI_ERRNO_H */
