#ifndef _LINUX_HISI_WARMBOOT_H
#define _LINUX_HISI_WARMBOOT_H

#include <linux/types.h>

bool hisi_warmboot_is_ready(void);
void hisi_warmboot_disarm(void);
int hisi_warmboot_prepare(const char *cmd);
void __noreturn hisi_warmboot_jump(void);
int hisi_warmboot_init_sysfs(void);
void hisi_warmboot_exit_sysfs(void);

#endif
