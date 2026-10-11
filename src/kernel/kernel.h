#ifndef KERNEL_H
#define KERNEL_H

#include "src/types.h"
#include "src/memory_manager/memory.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "src/utils/utils.h"
#include "src/interrupts/interrupts.h"
#include "src/watchdog/watchdog.h"
#include "src/smp/smp.h"
#include "src/scheduler/scheduler.h"
#include "src/scheduler/spinlock.h"
#include "src/scheduler/mutex.h"
#include "src/scheduler/semaphore.h"
#include "src/scheduler/cond.h"
#include "src/scheduler/event_group.h"
#include "src/syscall/syscall.h"
#include "src/time/time.h"
#include "src/console/console.h"
#include "arch/xtensa_lx6/drivers/esp32_led/led.h"
#include "arch/xtensa_lx6/drivers/esp32_gpio/gpio.h"
#include "arch/xtensa_lx6/drivers/esp32_spi/spi.h"
#include "arch/xtensa_lx6/drivers/esp32_sdcard/sdcard.h"
#include "arch/xtensa_lx6/drivers/esp32_uart/uart.h"
#include "src/file_system/vfs/vfs.h"
#include "src/loader/elf_loader.h"


#endif /* KERNEL_H */
