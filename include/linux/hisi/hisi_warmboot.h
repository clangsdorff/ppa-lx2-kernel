#ifndef _LINUX_HISI_WARMBOOT_H
#define _LINUX_HISI_WARMBOOT_H

#include <linux/types.h>

bool hisi_warmboot_is_ready(void);
void hisi_warmboot_disarm(void);
int hisi_warmboot_prepare(const char *cmd);
void hisi_warmboot_save_dfx(void);
void hisi_warmboot_jump(void);
bool hisi_warmboot_commit(const char *cmd);
int hisi_warmboot_init_sysfs(void);
void hisi_warmboot_exit_sysfs(void);
#ifdef CONFIG_BALONG_MODEM_RESET
int hisi_warmboot_modem_power_off(void);
int hisi_warmboot_modem_power_on(void);
#else
static inline int hisi_warmboot_modem_power_off(void) { return 0; }
static inline int hisi_warmboot_modem_power_on(void) { return -1; }
#endif
#if IS_BUILTIN(CONFIG_TZDRIVER)
int hisi_warmboot_tzdriver_close_sessions(void);
#else
static inline int hisi_warmboot_tzdriver_close_sessions(void) { return 0; }
#endif

#endif
