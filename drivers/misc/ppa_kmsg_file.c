// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define KMSG_PATH_A "/data/local/tmp/ppa_kmsg_a.txt"
#define KMSG_PATH_B "/data/local/tmp/ppa_kmsg_b.txt"
#define KMSG_BUF_SIZE (1 << CONFIG_LOG_BUF_SHIFT)
#define KMSG_FIRST_DELAY (5 * HZ)
#define KMSG_PERIOD (15 * HZ)
#define KMSG_RUNS 40

static struct delayed_work kmsg_work;
static int kmsg_runs;

static void kmsg_file_write(void)
{
	struct kmsg_dumper dumper = { .active = true };
	struct file *f, *st;
	loff_t pos = 0;
	size_t len = 0;
	ssize_t wret = -1;
	int fret = -1;
	int hlen;
	const char *path = (kmsg_runs & 1) ? KMSG_PATH_B : KMSG_PATH_A;
	char hdr[64];
	char st_path[128];
	char *buf;

	buf = vmalloc(KMSG_BUF_SIZE);
	if (!buf)
		return;

	kmsg_dump_rewind(&dumper);
	kmsg_dump_get_buffer(&dumper, false, buf, KMSG_BUF_SIZE, &len);

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (!IS_ERR(f)) {
		hlen = snprintf(hdr, sizeof(hdr), "ppa run=%d len=%zu\n",
				kmsg_runs, len);
		if (hlen > 0)
			wret = kernel_write(f, hdr, hlen, &pos);
		if (len > 0)
			wret = kernel_write(f, buf, len, &pos);
		fret = vfs_fsync(f, 0);
		filp_close(f, NULL);
	} else {
		wret = PTR_ERR(f);
	}

	snprintf(st_path, sizeof(st_path),
		 "/data/local/tmp/ppa_st_%d_%zu_%zd_%d",
		 kmsg_runs, len, wret, fret);
	st = filp_open(st_path, O_WRONLY | O_CREAT, 0644);
	if (!IS_ERR(st))
		filp_close(st, NULL);

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
