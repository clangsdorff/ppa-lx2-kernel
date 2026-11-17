// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/hisi/kirin_partition.h>
#include <linux/hisi/hisi_partition.h>
#include <linux/hisi/rdr_hisi_platform.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/syscalls.h>
#include <linux/workqueue.h>

#define DFX_FIRST_DELAY (5 * HZ)
#define DFX_POLL (1 * HZ)
#define DFX_PERIOD (15 * HZ)
#define DFX_RUNS 40

static struct delayed_work dfx_work;
static int dfx_runs;

static bool dfx_node_ready(void)
{
	char path[64];
	struct kstat st;

	if (flash_find_ptn_s(PART_DFX, path, sizeof(path)))
		return false;
	return !vfs_stat(path, &st);
}

static void dfx_work_fn(struct work_struct *work)
{
	if (!dfx_node_ready()) {
		schedule_delayed_work(&dfx_work, DFX_POLL);
		return;
	}

	/* writes the fixed temp slot only, the dfx ring head is untouched */
	save_log_to_dfx_tempbuffer(AP_S_PRESS6S);
	sys_sync();
	if (++dfx_runs < DFX_RUNS)
		schedule_delayed_work(&dfx_work, DFX_PERIOD);
}

static int __init ppa_kmsg_dfx_init(void)
{
	INIT_DELAYED_WORK(&dfx_work, dfx_work_fn);
	schedule_delayed_work(&dfx_work, DFX_FIRST_DELAY);
	return 0;
}
late_initcall(ppa_kmsg_dfx_init);
