/*
 * hisi_warmboot.c
 *
 * In-kernel EL1 warm reboot loader for Kirin 710A (PPA-LX2)
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/pagemap.h>
#include <linux/zlib.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/reboot.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqnr.h>
#include <linux/irqdesc.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/hisi/hisi_warmboot.h>
#ifdef CONFIG_HISI_BB
#include <linux/hisi/hisi_bootup_keypoint.h>
#include <linux/hisi/rdr_pub.h>
#include <linux/hisi/rdr_hisi_platform.h>
#include <linux/hisi/util.h>
extern void mntn_print_to_ramconsole(const char *fmt, ...);
#endif
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <soc_rtctimerwdt_interface.h>

#include "../../../arch/arm64/kernel/cpu-reset.h"

#define WARMBOOT_KERNEL_PHYS		0x00080000ULL
#define WARMBOOT_KERNEL_SAFE_END	0x04800000ULL
#define WARMBOOT_RAMDISK_PHYS		0xba000000ULL
#define WARMBOOT_RAMDISK_MAX		0x00800000ULL
#define WARMBOOT_DTB_PHYS		0xba800000ULL
#define WARMBOOT_DTB_MAX		0x00200000ULL

#define WARMBOOT_TEE_MBOX_START		0x0f800000ULL
#define WARMBOOT_TEE_MBOX_END		0x0fc00000ULL
#define WARMBOOT_TEE_RESV_START		0x0fffc000ULL
#define WARMBOOT_TEE_RESV_END		0x10000000ULL

#define PART_RAMDISK			21
#define PART_ERECOVERY_RAMDISK		45
#define PART_BOOT			47
#define PART_RECOVERY_RAMDISK		53

#define ANDROID_BOOT_MAGIC		"ANDROID!"
#define ANDROID_BOOT_MAGIC_SIZE		8
#define ARM64_IMAGE_MAGIC		0x644d5241U

#define MAX_REJECT_ALLOCS		2048

struct warmboot_seg_desc {
	u64 dst_phys;
	u64 page_count;
	u64 page_list_phys;
};

struct warmboot_seg {
	u64 dst_phys;
	u64 nr_pages;
	u64 capacity;
	struct page **pages;
	u64 *page_list;
	unsigned int list_order;
};

struct android_boot_hdr_min {
	u8 magic[8];
	u32 kernel_size;
	u32 kernel_addr;
	u32 ramdisk_size;
	u32 ramdisk_addr;
	u32 second_size;
	u32 second_addr;
	u32 tags_addr;
	u32 page_size;
};

extern const unsigned char hisi_warmboot_relocate[];
extern const unsigned long hisi_warmboot_relocate_size;

#ifdef CONFIG_HISI_SP805_WATCHDOG
extern void __iomem *wdt_base;
#endif

static DEFINE_MUTEX(g_warmboot_lock);
static struct warmboot_seg g_segs[3];
static struct page *g_tramp_page;
static char g_boot_path[256] = "mmcblk0p47";
static int g_warmboot_armed;
static int g_warmboot_ready;

#define WARMBOOT_AUTO_ARM_MAX_TRIES	30
#define WARMBOOT_AUTO_ARM_DELAY_JIFFIES	(2 * HZ)

static int g_warmboot_user_disarmed;
static int g_warmboot_auto_tries;
static void warmboot_auto_arm_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(g_warmboot_auto_work, warmboot_auto_arm_work_fn);

static bool warmboot_range_excluded(u64 start, u64 size)
{
	u64 end = start + size;

	if (start < WARMBOOT_KERNEL_SAFE_END)
		return true;
	if (end > WARMBOOT_TEE_MBOX_START && start < WARMBOOT_TEE_MBOX_END)
		return true;
	if (end > WARMBOOT_TEE_RESV_START && start < WARMBOOT_TEE_RESV_END)
		return true;
	if (end > WARMBOOT_RAMDISK_PHYS &&
	    start < WARMBOOT_DTB_PHYS + WARMBOOT_DTB_MAX)
		return true;
	return false;
}

static struct page *warmboot_alloc_safe_block(unsigned int order,
					      struct list_head *rejects)
{
	int tries;

	for (tries = 0; tries < MAX_REJECT_ALLOCS; tries++) {
		struct page *p = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		u64 pa;

		if (!p)
			return NULL;
		pa = page_to_phys(p);
		if (!warmboot_range_excluded(pa, PAGE_SIZE << order))
			return p;
		set_page_private(p, order);
		list_add(&p->lru, rejects);
	}
	return NULL;
}

static void warmboot_free_rejects(struct list_head *rejects)
{
	struct page *p, *tmp;

	list_for_each_entry_safe(p, tmp, rejects, lru) {
		unsigned int order = (unsigned int)page_private(p);

		list_del(&p->lru);
		set_page_private(p, 0);
		__free_pages(p, order);
	}
}

static void warmboot_free_seg(struct warmboot_seg *seg)
{
	u64 i;

	if (seg->pages) {
		for (i = 0; i < seg->nr_pages; i++) {
			if (seg->pages[i])
				__free_page(seg->pages[i]);
		}
		vfree(seg->pages);
		seg->pages = NULL;
	}
	if (seg->page_list) {
		free_pages((unsigned long)seg->page_list, seg->list_order);
		seg->page_list = NULL;
	}
	seg->nr_pages = 0;
	seg->capacity = 0;
	seg->dst_phys = 0;
}

static void warmboot_free_all(void)
{
	int i;

	g_warmboot_ready = 0;
	for (i = 0; i < 3; i++)
		warmboot_free_seg(&g_segs[i]);
	if (g_tramp_page) {
		__free_page(g_tramp_page);
		g_tramp_page = NULL;
	}
}

static int warmboot_init_seg(struct warmboot_seg *seg, u64 dst_phys,
			     u64 max_bytes, struct list_head *rejects)
{
	struct page *list_blk;
	u64 cap = max_bytes >> PAGE_SHIFT;
	unsigned int order = get_order(cap * sizeof(u64));

	seg->dst_phys = dst_phys;
	seg->nr_pages = 0;
	seg->capacity = cap;
	seg->list_order = order;

	seg->pages = vzalloc(cap * sizeof(struct page *));
	if (!seg->pages)
		return -ENOMEM;

	list_blk = warmboot_alloc_safe_block(order, rejects);
	if (!list_blk) {
		vfree(seg->pages);
		seg->pages = NULL;
		return -ENOMEM;
	}
	seg->page_list = (u64 *)page_address(list_blk);
	return 0;
}

static struct page *warmboot_append_page(struct warmboot_seg *seg,
					 struct list_head *rejects)
{
	struct page *p;

	if (seg->nr_pages >= seg->capacity)
		return NULL;
	p = warmboot_alloc_safe_block(0, rejects);
	if (!p)
		return NULL;
	seg->pages[seg->nr_pages] = p;
	seg->page_list[seg->nr_pages] = page_to_phys(p);
	seg->nr_pages++;
	return p;
}

static int warmboot_read_bdev_range(int partno, loff_t offset, void *dst,
				    size_t len)
{
	struct block_device *bdev;
	dev_t devt;
	size_t done = 0;

	devt = blk_lookup_devt("mmcblk0", partno);
	if (!devt) {
		pr_err("warmboot: mmcblk0p%d not found\n", partno);
		return -ENODEV;
	}
	bdev = blkdev_get_by_dev(devt, FMODE_READ, NULL);
	if (IS_ERR(bdev)) {
		pr_err("warmboot: open mmcblk0p%d failed: %ld\n",
		       partno, PTR_ERR(bdev));
		return PTR_ERR(bdev);
	}

	while (done < len) {
		pgoff_t idx = (pgoff_t)((offset + done) >> PAGE_SHIFT);
		size_t pg_off = (size_t)((offset + done) & ~PAGE_MASK);
		size_t chunk = min(len - done, PAGE_SIZE - pg_off);
		struct page *p = read_mapping_page(bdev->bd_inode->i_mapping,
						   idx, NULL);
		void *vaddr;

		if (IS_ERR(p)) {
			blkdev_put(bdev, FMODE_READ);
			return PTR_ERR(p);
		}
		vaddr = kmap(p);
		memcpy((u8 *)dst + done, (u8 *)vaddr + pg_off, chunk);
		kunmap(p);
		put_page(p);
		done += chunk;
	}

	blkdev_put(bdev, FMODE_READ);
	return 0;
}

static int warmboot_read_source(const char *path_or_part, int default_partno,
				loff_t offset, void *dst, size_t len)
{
	struct file *filp;
	ssize_t n;
	size_t done = 0;
	loff_t pos = offset;

	if (!path_or_part || path_or_part[0] != '/')
		return warmboot_read_bdev_range(default_partno, offset, dst, len);

	filp = filp_open(path_or_part, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_warn("warmboot: open %s failed (%ld), fallback mmcblk0p%d\n",
			path_or_part, PTR_ERR(filp), default_partno);
		return warmboot_read_bdev_range(default_partno, offset, dst, len);
	}

	while (done < len) {
		n = kernel_read(filp, (u8 *)dst + done, len - done, &pos);
		if (n <= 0) {
			filp_close(filp, NULL);
			return n < 0 ? (int)n : -EIO;
		}
		done += (size_t)n;
	}
	filp_close(filp, NULL);
	return 0;
}

static int warmboot_skip_gzip_hdr(const u8 *buf, size_t len, size_t *hdr_off)
{
	size_t off = 10;
	u8 flg;

	if (len < 12 || buf[0] != 0x1f || buf[1] != 0x8b || buf[2] != 0x08)
		return -EINVAL;
	flg = buf[3];

	if (flg & 0x04) {
		u16 xlen;

		if (off + 2 > len)
			return -EINVAL;
		xlen = buf[off] | ((u16)buf[off + 1] << 8);
		off += 2 + xlen;
	}
	if (flg & 0x08) {
		while (off < len && buf[off])
			off++;
		off++;
	}
	if (flg & 0x10) {
		while (off < len && buf[off])
			off++;
		off++;
	}
	if (flg & 0x02)
		off += 2;
	if (off >= len)
		return -EINVAL;
	*hdr_off = off;
	return 0;
}

static int warmboot_load_kernel(struct warmboot_seg *seg,
				struct list_head *rejects)
{
	struct android_boot_hdr_min hdr;
	u32 page_sz, ksz;
	u8 *kbuf = NULL;
	u8 *first_img;
	u64 image_size, target_pages;
	size_t raw_bytes = 0;
	int ret;

	ret = warmboot_read_source(g_boot_path, PART_BOOT, 0, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (memcmp(hdr.magic, ANDROID_BOOT_MAGIC, ANDROID_BOOT_MAGIC_SIZE)) {
		pr_err("warmboot: invalid boot magic\n");
		return -EINVAL;
	}
	page_sz = hdr.page_size ? hdr.page_size : 2048;
	ksz = hdr.kernel_size;
	if (!ksz || ksz > (WARMBOOT_KERNEL_SAFE_END - WARMBOOT_KERNEL_PHYS))
		return -EINVAL;

	kbuf = vmalloc(ksz);
	if (!kbuf)
		return -ENOMEM;

	ret = warmboot_read_source(g_boot_path, PART_BOOT, page_sz, kbuf, ksz);
	if (ret)
		goto out;

	if (ksz >= 12 && kbuf[0] == 0x1f && kbuf[1] == 0x8b && kbuf[2] == 0x08) {
		struct z_stream_s strm;
		size_t gz_off = 0;
		int zrc;

		ret = warmboot_skip_gzip_hdr(kbuf, ksz, &gz_off);
		if (ret)
			goto out;

		memset(&strm, 0, sizeof(strm));
		strm.workspace = vmalloc(zlib_inflate_workspacesize());
		if (!strm.workspace) {
			ret = -ENOMEM;
			goto out;
		}
		zrc = zlib_inflateInit2(&strm, -MAX_WBITS);
		if (zrc != Z_OK) {
			vfree(strm.workspace);
			ret = -EINVAL;
			goto out;
		}
		strm.next_in = kbuf + gz_off;
		strm.avail_in = ksz - gz_off;

		do {
			struct page *p = warmboot_append_page(seg, rejects);

			if (!p) {
				zlib_inflateEnd(&strm);
				vfree(strm.workspace);
				ret = -ENOMEM;
				goto out;
			}
			strm.next_out = page_address(p);
			strm.avail_out = PAGE_SIZE;
			zrc = zlib_inflate(&strm, Z_SYNC_FLUSH);
			if (zrc != Z_OK && zrc != Z_STREAM_END) {
				pr_err("warmboot: inflate err %d\n", zrc);
				zlib_inflateEnd(&strm);
				vfree(strm.workspace);
				ret = -EIO;
				goto out;
			}
		} while (zrc != Z_STREAM_END);

		raw_bytes = strm.total_out;
		zlib_inflateEnd(&strm);
		vfree(strm.workspace);
	} else {
		size_t done = 0;

		while (done < ksz) {
			struct page *p = warmboot_append_page(seg, rejects);
			size_t chunk = min_t(size_t, ksz - done, PAGE_SIZE);

			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			memcpy(page_address(p), kbuf + done, chunk);
			done += chunk;
		}
		raw_bytes = ksz;
	}

	if (!seg->nr_pages) {
		ret = -EINVAL;
		goto out;
	}
	first_img = page_address(seg->pages[0]);
	if (le32_to_cpup((__le32 *)(first_img + 0x38)) != ARM64_IMAGE_MAGIC) {
		pr_err("warmboot: invalid ARM64 Image magic\n");
		ret = -EINVAL;
		goto out;
	}
	image_size = le64_to_cpup((__le64 *)(first_img + 0x10));
	if (image_size < raw_bytes)
		image_size = raw_bytes;
	target_pages = PAGE_ALIGN(image_size) >> PAGE_SHIFT;
	if (target_pages > seg->capacity) {
		pr_err("warmboot: kernel image_size 0x%llx too large\n",
		       image_size);
		ret = -EFBIG;
		goto out;
	}
	while (seg->nr_pages < target_pages) {
		if (!warmboot_append_page(seg, rejects)) {
			ret = -ENOMEM;
			goto out;
		}
	}
	ret = 0;
out:
	vfree(kbuf);
	return ret;
}

static int warmboot_load_ramdisk(int partno, struct warmboot_seg *seg,
				 struct list_head *rejects, u32 *out_size)
{
	struct android_boot_hdr_min hdr;
	u32 page_sz, rsz;
	u8 *rbuf;
	size_t done = 0;
	int ret;

	ret = warmboot_read_bdev_range(partno, 0, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (memcmp(hdr.magic, ANDROID_BOOT_MAGIC, ANDROID_BOOT_MAGIC_SIZE)) {
		pr_err("warmboot: invalid ramdisk hdr on mmcblk0p%d\n", partno);
		return -EINVAL;
	}
	page_sz = hdr.page_size ? hdr.page_size : 2048;
	rsz = hdr.ramdisk_size;
	if (!rsz || rsz > WARMBOOT_RAMDISK_MAX)
		return -EINVAL;

	rbuf = vmalloc(rsz);
	if (!rbuf)
		return -ENOMEM;

	ret = warmboot_read_bdev_range(partno, page_sz, rbuf, rsz);
	if (ret)
		goto out;

	while (done < rsz) {
		struct page *p = warmboot_append_page(seg, rejects);
		size_t chunk = min_t(size_t, rsz - done, PAGE_SIZE);

		if (!p) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(page_address(p), rbuf + done, chunk);
		done += chunk;
	}
	*out_size = rsz;
	ret = 0;
out:
	vfree(rbuf);
	return ret;
}

static int warmboot_fdt_set_u32_or_u64(void *fdt, int node, const char *name,
				       u64 val)
{
	int len = 0;
	const void *old = fdt_getprop(fdt, node, name, &len);

	if (old && len == 4) {
		__be32 v32 = cpu_to_fdt32((u32)val);

		return fdt_setprop(fdt, node, name, &v32, sizeof(v32));
	} else {
		__be64 v64 = cpu_to_fdt64(val);

		return fdt_setprop(fdt, node, name, &v64, sizeof(v64));
	}
}

static int warmboot_prepare_dtb(struct warmboot_seg *seg,
				struct list_head *rejects, u32 ramdisk_size)
{
	size_t orig_sz, buf_sz, packed_sz, done = 0;
	void *fdt;
	int chosen, fb_node, len = 0, ret;

	if (!initial_boot_params)
		return -EINVAL;

	orig_sz = fdt_totalsize(initial_boot_params);
	buf_sz = orig_sz + 4096;
	if (buf_sz > WARMBOOT_DTB_MAX)
		return -EFBIG;

	fdt = vmalloc(buf_sz);
	if (!fdt)
		return -ENOMEM;

	ret = fdt_open_into(initial_boot_params, fdt, (int)buf_sz);
	if (ret) {
		ret = -EINVAL;
		goto out;
	}

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen >= 0) {
		fdt_setprop_u32(fdt, chosen, "hisi,warmboot", 1);
		warmboot_fdt_set_u32_or_u64(fdt, chosen, "linux,initrd-start",
					    WARMBOOT_RAMDISK_PHYS);
		warmboot_fdt_set_u32_or_u64(fdt, chosen, "linux,initrd-end",
					    WARMBOOT_RAMDISK_PHYS + ramdisk_size);
		if (fdt_getprop(fdt, chosen, "kaslr-seed", &len) &&
		    len == sizeof(u64)) {
			__be64 v64 = 0;

			fdt_setprop(fdt, chosen, "kaslr-seed", &v64,
				    sizeof(v64));
		}
	}

	/*
	 * device_shutdown() powers off DSS before jumping, so the new kernel
	 * must re-initialize display regulators/clocks instead of skipping init.
	 */
	fb_node = fdt_node_offset_by_compatible(fdt, -1, "hisilicon,hisifb");
	if (fb_node >= 0)
		fdt_setprop_u32(fdt, fb_node, "fastboot_enable_flag", 0);

	fdt_pack(fdt);
	packed_sz = fdt_totalsize(fdt);
	while (done < packed_sz) {
		struct page *p = warmboot_append_page(seg, rejects);
		size_t chunk = min_t(size_t, packed_sz - done, PAGE_SIZE);

		if (!p) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(page_address(p), (u8 *)fdt + done, chunk);
		done += chunk;
	}
	ret = 0;
out:
	vfree(fdt);
	return ret;
}

static void warmboot_flush_staged(void)
{
	struct warmboot_seg_desc *descs;
	void *tramp_va = page_address(g_tramp_page);
	u64 tramp_pa = page_to_phys(g_tramp_page);
	int i;

	descs = (struct warmboot_seg_desc *)((u8 *)tramp_va + 0x400);
	memset(descs, 0, sizeof(*descs) * 4);

	for (i = 0; i < 3; i++) {
		u64 j;

		for (j = 0; j < g_segs[i].nr_pages; j++)
			__flush_dcache_area(page_address(g_segs[i].pages[j]),
					    PAGE_SIZE);
		__flush_dcache_area(g_segs[i].page_list,
				    PAGE_SIZE << g_segs[i].list_order);
		descs[i].dst_phys = g_segs[i].dst_phys;
		descs[i].page_count = g_segs[i].nr_pages;
		descs[i].page_list_phys = virt_to_phys(g_segs[i].page_list);
	}

	memcpy(tramp_va, hisi_warmboot_relocate, hisi_warmboot_relocate_size);
	__flush_dcache_area(tramp_va, PAGE_SIZE);
	flush_icache_range((uintptr_t)tramp_va,
			   (uintptr_t)tramp_va + hisi_warmboot_relocate_size);
	pr_info("warmboot: trampoline at 0x%llx (reloc %lu B)\n",
		tramp_pa, hisi_warmboot_relocate_size);
}

bool hisi_warmboot_is_ready(void)
{
	return g_warmboot_ready != 0;
}

void hisi_warmboot_disarm(void)
{
	g_warmboot_user_disarmed = 1;
	cancel_delayed_work(&g_warmboot_auto_work);
	mutex_lock(&g_warmboot_lock);
	g_warmboot_armed = 0;
	warmboot_free_all();
	mutex_unlock(&g_warmboot_lock);
}

int hisi_warmboot_prepare(const char *cmd)
{
	LIST_HEAD(rejects);
	u32 ramdisk_size = 0;
	int rd_part = PART_RAMDISK;
	int ret;

	mutex_lock(&g_warmboot_lock);

	if (cmd && !strcmp(cmd, "recovery"))
		rd_part = PART_RECOVERY_RAMDISK;
	else if (cmd && !strcmp(cmd, "erecovery"))
		rd_part = PART_ERECOVERY_RAMDISK;

	if (g_warmboot_ready && rd_part == PART_RAMDISK &&
	    (!cmd || !strcmp(cmd, "warm"))) {
		mutex_unlock(&g_warmboot_lock);
		return 0;
	}

	warmboot_free_all();

	ret = warmboot_init_seg(&g_segs[0], WARMBOOT_KERNEL_PHYS,
				WARMBOOT_KERNEL_SAFE_END - WARMBOOT_KERNEL_PHYS,
				&rejects);
	if (ret)
		goto err;

	ret = warmboot_init_seg(&g_segs[1], WARMBOOT_RAMDISK_PHYS,
				WARMBOOT_RAMDISK_MAX, &rejects);
	if (ret)
		goto err;

	ret = warmboot_init_seg(&g_segs[2], WARMBOOT_DTB_PHYS,
				WARMBOOT_DTB_MAX, &rejects);
	if (ret)
		goto err;

	g_tramp_page = warmboot_alloc_safe_block(0, &rejects);
	if (!g_tramp_page || hisi_warmboot_relocate_size > 0x400) {
		ret = -ENOMEM;
		goto err;
	}

	ret = warmboot_load_kernel(&g_segs[0], &rejects);
	if (ret)
		goto err;

	ret = warmboot_load_ramdisk(rd_part, &g_segs[1], &rejects,
				    &ramdisk_size);
	if (ret)
		goto err;

	ret = warmboot_prepare_dtb(&g_segs[2], &rejects, ramdisk_size);
	if (ret)
		goto err;

	warmboot_free_rejects(&rejects);
	warmboot_flush_staged();
	g_warmboot_ready = 1;
	pr_info("warmboot: staged kernel=%llu ramdisk=%llu dtb=%llu pages\n",
		g_segs[0].nr_pages, g_segs[1].nr_pages, g_segs[2].nr_pages);
	mutex_unlock(&g_warmboot_lock);
	return 0;

err:
	warmboot_free_rejects(&rejects);
	warmboot_free_all();
	mutex_unlock(&g_warmboot_lock);
	pr_err("warmboot: prepare failed: %d\n", ret);
	return ret;
}

static void warmboot_quiesce_irqs_and_wdt(void)
{
	unsigned int i;
	struct irq_desc *desc;

#ifdef CONFIG_HISI_SP805_WATCHDOG
	if (wdt_base) {
		writel_relaxed(0x1ACCE551,
			       wdt_base + SOC_RTCTIMERWDT_WDLOCK_ADDR(0));
		writel_relaxed(0, wdt_base + SOC_RTCTIMERWDT_WDCONTROL_ADDR(0));
		writel_relaxed(15 * 32768 - 1,
			       wdt_base + SOC_RTCTIMERWDT_WDLOAD_ADDR(0));
		writel_relaxed(1, wdt_base + SOC_RTCTIMERWDT_WDINTCLR_ADDR(0));
		writel_relaxed(3, wdt_base + SOC_RTCTIMERWDT_WDCONTROL_ADDR(0));
		writel_relaxed(1, wdt_base + SOC_RTCTIMERWDT_WDLOCK_ADDR(0));
		readl_relaxed(wdt_base + SOC_RTCTIMERWDT_WDLOCK_ADDR(0));
	}
#endif

	for_each_irq_desc(i, desc) {
		struct irq_chip *chip = irq_desc_get_chip(desc);
		int ret;

		if (!chip)
			continue;
		ret = irq_set_irqchip_state(i, IRQCHIP_STATE_ACTIVE, false);
		if (ret && irqd_irq_inprogress(&desc->irq_data) &&
		    chip->irq_eoi)
			chip->irq_eoi(&desc->irq_data);
		if (chip->irq_mask)
			chip->irq_mask(&desc->irq_data);
		if (chip->irq_disable && !irqd_irq_disabled(&desc->irq_data))
			chip->irq_disable(&desc->irq_data);
	}

	asm volatile(
		"msr cntp_ctl_el0, xzr\n"
		"msr cntv_ctl_el0, xzr\n"
		"isb\n"
		"msr daifset, #0xf\n"
		: : : "memory");
}

void hisi_warmboot_save_dfx(void)
{
#ifdef CONFIG_HISI_BB
	pr_emerg("warmboot: saving dfx tempbuffer (tramp=0x%llx)\n",
		 g_tramp_page ? (u64)page_to_phys(g_tramp_page) : 0ULL);
	mntn_print_to_ramconsole("warmboot: saving dfx tempbuffer (tramp=0x%llx)\n",
				 g_tramp_page ? (u64)page_to_phys(g_tramp_page) : 0ULL);
	set_reboot_reason(AP_S_PRESS6S);
	save_log_to_dfx_tempbuffer(AP_S_PRESS6S);
#endif
}

static void warmboot_powerup_media1_crg(void)
{
	void __iomem *peri_crg;
	void __iomem *sctrl;
	void __iomem *pmctrl;
	void __iomem *media_crg;
	u32 pwr_before, mux_val, aod_vote;

	peri_crg = ioremap_nocache(0xFFF35000ULL, 0x1000);
	sctrl = ioremap_nocache(0xFFF0A000ULL, 0x1000);
	pmctrl = ioremap_nocache(0xFFF31000ULL, 0x1000);
	media_crg = ioremap_nocache(0xE87FF000ULL, 0x1000);

	if (!peri_crg || !sctrl || !pmctrl || !media_crg) {
		pr_err("warmboot: ioremap failed for media1 powerup\n");
		goto out_unmap;
	}

	pwr_before = readl(peri_crg + 0x158);

	/* MEDIA1_SUBSYS_PU: enable MTCMOS, unreset media_crg, toggle clks, unisolate */
	writel(0x00000020, peri_crg + 0x150); /* PERPWREN */
	udelay(100);
	writel(0x00040000, peri_crg + 0x0A0); /* PERRSTDIS5 media_crg */
	writel(0x7C002028, peri_crg + 0x410); /* PEREN6 */
	writel(0x00000040, sctrl + 0x1B0);    /* SCPEREN4 */
	writel(0x00000040, peri_crg + 0x040); /* PEREN4 */
	udelay(2);
	writel(0x7C002028, peri_crg + 0x414); /* PERDIS6 */
	writel(0x00000040, sctrl + 0x1B4);    /* SCPERDIS4 */
	writel(0x00000040, peri_crg + 0x044); /* PERDIS4 */
	udelay(2);
	writel(0x00000040, peri_crg + 0x148); /* ISODIS */
	udelay(1);
	writel(0x00020000, peri_crg + 0x0A0); /* PERRSTDIS5 media */
	udelay(1);
	writel(0x7C002028, peri_crg + 0x410); /* PEREN6 */
	writel(0x00000040, sctrl + 0x1B0);    /* SCPEREN4 */
	writel(0x00000040, peri_crg + 0x040); /* PEREN4 */
	udelay(2);

	/* VIVOBUS_PU: enable vivobus clocks and clear vivobus NoC idle */
	writel(0x00080008, media_crg + 0x084); /* MEDIA_CLKDIV9 */
	writel(0x08040040, media_crg + 0x000); /* MEDIA_PEREN0 */
	udelay(2);
	writel(0x00040040, media_crg + 0x004); /* MEDIA_PERDIS0 */
	udelay(2);
	writel(0x00040040, media_crg + 0x000); /* MEDIA_PEREN0 */
	writel(0x80000000, pmctrl + 0x380);    /* NOC_POWER_IDLEREQ clear bit 15 */
	udelay(5);

	/* Ensure DSS stays in softreset so check_dss_is_power_on() runs clean init */
	writel(0x000000C0, media_crg + 0x030); /* MEDIA_PERRSTEN0 */

	/* Clear AP media1/vivobus/dss AOD vote bits in sctrl+0x438 */
	aod_vote = readl(sctrl + 0x438);
	writel(aod_vote & ~0x15U, sctrl + 0x438);

	mux_val = readl(media_crg + 0x074);
#ifdef CONFIG_HISI_BB
	mntn_print_to_ramconsole("warmboot: media1 pu pwr=0x%x->0x%x mux074=0x%x\n",
				 pwr_before, readl(peri_crg + 0x158), mux_val);
#endif

out_unmap:
	if (media_crg)
		iounmap(media_crg);
	if (pmctrl)
		iounmap(pmctrl);
	if (sctrl)
		iounmap(sctrl);
	if (peri_crg)
		iounmap(peri_crg);
}

void __noreturn hisi_warmboot_jump(void)
{
	u64 tramp_phys = page_to_phys(g_tramp_page);

	warmboot_powerup_media1_crg();
#ifdef CONFIG_HISI_BB
	mntn_print_to_ramconsole("warmboot: jumping to tramp=0x%llx\n",
				 tramp_phys);
	set_boot_keypoint(60);
#endif
	local_irq_disable();
	warmboot_quiesce_irqs_and_wdt();
	flush_cache_all();
	cpu_soft_restart(0, tramp_phys, tramp_phys + 0x400,
			 WARMBOOT_DTB_PHYS, WARMBOOT_KERNEL_PHYS);
	unreachable();
}

static void warmboot_auto_arm_work_fn(struct work_struct *work)
{
	int ret;

	if (g_warmboot_user_disarmed || hisi_warmboot_is_ready())
		return;

	ret = hisi_warmboot_prepare("warm");
	if (!ret) {
		mutex_lock(&g_warmboot_lock);
		if (!g_warmboot_user_disarmed && g_warmboot_ready)
			g_warmboot_armed = 1;
		mutex_unlock(&g_warmboot_lock);
		pr_info("warmboot: auto-armed default warm reboot\n");
		return;
	}

	if (++g_warmboot_auto_tries < WARMBOOT_AUTO_ARM_MAX_TRIES &&
	    !g_warmboot_user_disarmed)
		schedule_delayed_work(&g_warmboot_auto_work,
				      WARMBOOT_AUTO_ARM_DELAY_JIFFIES);
	else
		pr_err("warmboot: auto-arm gave up after %d tries (last=%d)\n",
		       g_warmboot_auto_tries, ret);
}

static ssize_t warm_reboot_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "armed=%d ready=%d boot=%s kpages=%llu rpages=%llu dpages=%llu\n",
			 g_warmboot_armed, g_warmboot_ready, g_boot_path,
			 g_segs[0].nr_pages, g_segs[1].nr_pages,
			 g_segs[2].nr_pages);
}

static ssize_t warm_reboot_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;

	if (!strncmp(buf, "boot=", 5)) {
		size_t len = count - 5;

		while (len && (buf[5 + len - 1] == '\n' ||
			       buf[5 + len - 1] == '\r'))
			len--;
		if (!len || len >= sizeof(g_boot_path))
			return -EINVAL;
		mutex_lock(&g_warmboot_lock);
		memcpy(g_boot_path, buf + 5, len);
		g_boot_path[len] = '\0';
		warmboot_free_all();
		mutex_unlock(&g_warmboot_lock);
		return count;
	}
	if (sysfs_streq(buf, "arm") || sysfs_streq(buf, "on")) {
		g_warmboot_user_disarmed = 0;
		ret = hisi_warmboot_prepare("warm");
		if (ret)
			return ret;
		g_warmboot_armed = 1;
		return count;
	}
	if (sysfs_streq(buf, "disarm") || sysfs_streq(buf, "0") ||
	    sysfs_streq(buf, "off")) {
		g_warmboot_user_disarmed = 1;
		cancel_delayed_work(&g_warmboot_auto_work);
		mutex_lock(&g_warmboot_lock);
		g_warmboot_armed = 0;
		warmboot_free_all();
		mutex_unlock(&g_warmboot_lock);
		return count;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "now") ||
	    sysfs_streq(buf, "warm")) {
		g_warmboot_user_disarmed = 0;
		ret = hisi_warmboot_prepare("warm");
		if (ret)
			return ret;
		g_warmboot_armed = 1;
		kernel_restart("warm");
		return count;
	}
	if (sysfs_streq(buf, "recovery")) {
		g_warmboot_user_disarmed = 0;
		ret = hisi_warmboot_prepare("recovery");
		if (ret)
			return ret;
		g_warmboot_armed = 1;
		kernel_restart("recovery");
		return count;
	}
	return -EINVAL;
}

static struct kobj_attribute warm_reboot_attr =
	__ATTR(warm_reboot, 0600, warm_reboot_show, warm_reboot_store);

int hisi_warmboot_init_sysfs(void)
{
	int ret;

	ret = sysfs_create_file(kernel_kobj, &warm_reboot_attr.attr);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&g_warmboot_auto_work, warmboot_auto_arm_work_fn);
	schedule_delayed_work(&g_warmboot_auto_work,
			      WARMBOOT_AUTO_ARM_DELAY_JIFFIES);
	return 0;
}

void hisi_warmboot_exit_sysfs(void)
{
	cancel_delayed_work_sync(&g_warmboot_auto_work);
	sysfs_remove_file(kernel_kobj, &warm_reboot_attr.attr);
	hisi_warmboot_disarm();
}
