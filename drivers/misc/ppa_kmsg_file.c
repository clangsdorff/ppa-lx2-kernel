// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define KMSG_PATH "/data/local/tmp/ppa_kmsg.txt"
#define KMSG_BUF_SIZE (1 << CONFIG_LOG_BUF_SHIFT)
#define KMSG_FIRST_DELAY (30 * HZ)
#define KMSG_PERIOD (15 * HZ)
#define KMSG_RUNS 40

static struct delayed_work kmsg_work;
static int kmsg_runs;

static void kmsg_file_write(void)
{
	struct kmsg_dumper dumper = { .active = true };
	struct file *f;
	loff_t pos = 0;
	size_t len = 0;
	char *buf;

	buf = vmalloc(KMSG_BUF_SIZE);
	if (!buf)
		return;

	kmsg_dump_rewind(&dumper);
	kmsg_dump_get_buffer(&dumper, false, buf, KMSG_BUF_SIZE, &len);

	f = filp_open(KMSG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (!IS_ERR(f)) {
		kernel_write(f, buf, len, &pos);
		filp_close(f, NULL);
	}
	vfree(buf);
}

static void kmsg_work_fn(struct work_struct *work)
{
	kmsg_file_write();
	if (++kmsg_runs < KMSG_RUNS)
		schedule_delayed_work(&kmsg_work, KMSG_PERIOD);
}

static int __init ppa_kmsg_file_init(void)
{
	INIT_DELAYED_WORK(&kmsg_work, kmsg_work_fn);
	schedule_delayed_work(&kmsg_work, KMSG_FIRST_DELAY);
	return 0;
}
late_initcall(ppa_kmsg_file_init);
