#ifndef _LINUX_HISI_WARMBOOT_H
#define _LINUX_HISI_WARMBOOT_H

#include <linux/types.h>

bool hisi_warmboot_is_ready(void);
void hisi_warmboot_disarm(void);
int hisi_warmboot_prepare(const char *cmd);
void hisi_warmboot_save_dfx(void);
void __noreturn hisi_warmboot_jump(void);
int hisi_warmboot_init_sysfs(void);
void hisi_warmboot_exit_sysfs(void);
#ifdef CONFIG_BALONG_MODEM_RESET
int hisi_warmboot_modem_power_off(void);
int hisi_warmboot_modem_power_on(void);
#else
static inline int hisi_warmboot_modem_power_off(void) { return 0; }
static inline int hisi_warmboot_modem_power_on(void) { return -1; }
#endif

#endif
