#ifndef XTENSA_H
#define XTENSA_H

#define UART0_FIFO   (*(volatile uint32_t *)0x3FF40000u)
#define UART0_STATUS (*(volatile uint32_t *)0x3FF4001Cu)
#define UART0_STATUS_REG      (*(volatile uint32_t *)(0x3FF40000u + 0x001C))
#define UART0_RX_FIFO_CNT     ((*(volatile uint32_t *)(0x3FF40000u + 0x001C)) & 0xFFu)

/* Registres d'état d'erreur / contrôle FIFO (offsets vérifiés esp-idf uart_reg.h) */
#define UART0_INT_ST_REG   (*(volatile uint32_t *)(0x3FF40000u + 0x0008))
#define UART0_INT_CLR_REG  (*(volatile uint32_t *)(0x3FF40000u + 0x0010))
#define UART0_CONF0_REG    (*(volatile uint32_t *)(0x3FF40000u + 0x0020))
#define UART_BRK_DET_INT_ST     (1u << 7)
#define UART_RXFIFO_OVF_INT_ST  (1u << 4)
#define UART_FRM_ERR_INT_ST     (1u << 3)
#define UART_RX_ERROR_MASK  (UART_BRK_DET_INT_ST | UART_RXFIFO_OVF_INT_ST | UART_FRM_ERR_INT_ST)
#define UART_RXFIFO_RST     (1u << 17)

#define RTC_CNTL_WDTCONFIG0_REG   0x3FF4808Cu   /* était (par erreur) 0x...090 */
#define RTC_CNTL_WDTCONFIG1_REG   0x3FF48090u   /* était (par erreur) 0x...094 */
#define RTC_CNTL_WDTFEED_REG      0x3FF480A0u   /* était (par erreur) 0x...0A4 */
#define RTC_CNTL_WDTWPROTECT_REG  0x3FF480A4u   /* était (par erreur) 0x...0A8 */
#define RTC_CNTL_WDT_WKEY         0x50D83AA1u

#define TIMG0_WDTCONFIG0_REG      0x3FF5F048u
#define TIMG0_WDTFEED_REG         0x3FF5F060u
#define TIMG0_WDTWPROTECT_REG     0x3FF5F064u

#define TIMG1_WDTCONFIG0_REG      0x3FF60048u
#define TIMG1_WDTFEED_REG         0x3FF60060u
#define TIMG1_WDTWPROTECT_REG     0x3FF60064u

#define WDT_FEED_MAGIC            0xABAD1DEAu

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define DPORT_APPCPU_CTRL_A_REG   0x3FF0002Cu   /* bit0 = APPCPU_RESETTING   */
#define DPORT_APPCPU_CTRL_B_REG   0x3FF00030u   /* bit0 = APPCPU_CLKGATE_EN  */
#define DPORT_APPCPU_CTRL_C_REG   0x3FF00034u   /* bit0 = APPCPU_RUNSTALL (1=stall) */
#define DPORT_APPCPU_CTRL_D_REG   0x3FF00038u   /* adresse de boot (32 bits) */

#define DPORT_APPCPU_RESETTING    (1u << 0)
#define DPORT_APPCPU_CLKGATE_EN   (1u << 0)
#define DPORT_APPCPU_RUNSTALL     (1u << 0)

#define RTC_CNTL_OPTIONS0_REG        0x3FF48000u
#define RTC_CNTL_SW_CPU_STALL_REG    0x3FF480ACu

#define RTC_CNTL_SW_STALL_APPCPU_C0_S   0
#define RTC_CNTL_SW_STALL_APPCPU_C0_M   (0x3u << RTC_CNTL_SW_STALL_APPCPU_C0_S)
#define RTC_CNTL_SW_STALL_APPCPU_C1_S   20
#define RTC_CNTL_SW_STALL_APPCPU_C1_M   (0x3Fu << RTC_CNTL_SW_STALL_APPCPU_C1_S)

#define EXCCAUSE_ILLEGAL_INSTRUCTION  0
#define EXCCAUSE_SYSCALL              1
#define EXCCAUSE_LEVEL1_INTERRUPT     4
#define EXCCAUSE_PRIVILEGED           8

/* PS.UM (User Mode) : bit 5, vérifié contre xtensa/corebits.h officiel. */
#define PS_UM_MASK 0x00000020u
/* PS.EXCM : bit 4, à 1 pendant le traitement d'une exception ou d'une
 * interruption de niveau 1. */
#define PS_EXCM_MASK 0x00000010u

#define TIMER0_IRQ_LINE 6

/* Pile de chaque tâche. Pire consommation mesurée (analyse statique
 * -fstack-usage + graphe d'appels, voir le détail dans l'historique) :
 *   - tâche montant la carte SD (fat32_ls -> dir_find -> fat_entry_get,
 *     chacune avec un tampon de secteur de 512 octets) : ~2400 octets ;
 *   - + une interruption tombant au pire moment, qui s'exécute SUR la pile
 *     de la tâche interrompue : ~350 octets ;
 *   - syscall lisant la carte SD : ~2000 octets + la pile du programme.
 * 2048 débordait donc déjà ; 4096 laisse ~1,3 Ko de marge au pire cas
 * actuel. Un débordement est désormais DETECTE (voir scheduler.c). */
#define TASK_STACK_SIZE   4096

/* Pile des tâches idle (une par cœur). Elle n'exécute qu'une boucle waiti,
 * mais les interruptions qui la réveillent tournent SUR cette pile
 * (~350 octets + l'ordonnanceur et la libération des tâches terminées). */
#define IDLE_STACK_SIZE   1024

#define TASK_ANY_CORE 0xFFu

#endif /* XTENSA_H */
