/*
 * hisi_poweroff.c
 *
 * Power off or Reboot the hi3xxx device
 *
 * Copyright (c) 2012-2019 Huawei Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <asm/cacheflush.h>
#include <asm/system_misc.h>
#include "blackbox/rdr_print.h"
#include "pmic_interface.h"

#include <linux/kmsg_dump.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/mfd/hisi_pmic.h>
#include <soc_gpio_interface.h>
#include <soc_sctrl_interface.h>
#include <linux/hisi/hisi_log.h>
#include <linux/pm.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/hisi/hisi_warmboot.h>
#define HISI_LOG_TAG HISI_POWEROFF_TAG
#define IOMAP_ADD_INDEX 0
#define POWERHOLD_PROTECT_BIT_OFFSET 16
#define POWERHOLD_GPIO_BIT_OFFSET 2
#define BOOT0_BDEV_PATH "/dev/block/mmcblk0boot0"
#define BOOT0_VRL_ZERO_SECTORS 8
#define SCTRL_SCSYSSTAT_RESET_VAL 0xdeadbeef


static void __iomem *powerhold_gpio_base = NULL;
static unsigned int g_powerhold_gpio_offset;
static int g_powerhold_protect_offset = 0xFFFF;
static unsigned int g_powerhold_protect_bit = 0xFF;

#ifdef CONFIG_HISI_BB
#include <linux/hisi/rdr_hisi_platform.h>
#include <rdr_inner.h>
#endif

static void __iomem *sysctrl_base = NULL;
static int g_force_bootrom_reset;
static int g_bootrom_armed;

#define BOOTROM_AUTO_ARM_MAX_TRIES	30
#define BOOTROM_AUTO_ARM_DELAY_JIFFIES	(2 * HZ)

static int g_bootrom_auto_tries;
static struct delayed_work g_bootrom_auto_work;

static int hisi_invalidate_boot0_xloader(void);

static void hisi_bootrom_auto_arm_work_fn(struct work_struct *work)
{
	int ret;

	if (g_bootrom_armed)
		return;

	ret = hisi_invalidate_boot0_xloader();
	if (!ret) {
		pr_info("hisi_poweroff: auto-armed bootrom_reboot\n");
		return;
	}

	if (++g_bootrom_auto_tries < BOOTROM_AUTO_ARM_MAX_TRIES)
		schedule_delayed_work(&g_bootrom_auto_work,
				      BOOTROM_AUTO_ARM_DELAY_JIFFIES);
}

static bool hisi_is_bootrom_cmd(const char *cmd)
{
	if (!cmd)
		return false;
	return !strcmp(cmd, "bootrom") || !strcmp(cmd, "vcom");
}

static struct block_device *hisi_open_boot0_bdev(fmode_t mode)
{
	dev_t devt = blk_lookup_devt("mmcblk0boot0", 0);

	if (devt)
		return blkdev_get_by_dev(devt, mode, NULL);
	return blkdev_get_by_path(BOOT0_BDEV_PATH, mode, NULL);
}

static int hisi_invalidate_boot0_xloader(void)
{
	struct block_device *bdev;
	int ret;

	bdev = hisi_open_boot0_bdev(FMODE_READ);
	if (IS_ERR(bdev)) {
		BB_PRINT_ERR("%s: open ro %s failed: %ld\n",
			__func__, BOOT0_BDEV_PATH, PTR_ERR(bdev));
		return PTR_ERR(bdev);
	}
	if (bdev->bd_disk)
		set_disk_ro(bdev->bd_disk, 0);
	blkdev_put(bdev, FMODE_READ);

	bdev = hisi_open_boot0_bdev(FMODE_READ | FMODE_WRITE);
	if (IS_ERR(bdev)) {
		BB_PRINT_ERR("%s: open rw %s failed: %ld\n",
			__func__, BOOT0_BDEV_PATH, PTR_ERR(bdev));
		return PTR_ERR(bdev);
	}

	ret = blkdev_issue_zeroout(bdev, 0, BOOT0_VRL_ZERO_SECTORS,
		GFP_KERNEL, BLKDEV_ZERO_NOUNMAP);
	if (ret) {
		BB_PRINT_ERR("%s: zeroout %s failed: %d\n",
			__func__, BOOT0_BDEV_PATH, ret);
	} else {
		sync_blockdev(bdev);
		g_bootrom_armed = 1;
		pr_info("%s: invalidated xloader VRL header on %s\n",
			__func__, BOOT0_BDEV_PATH);
	}

	blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
	return ret;
}

static int hisi_bootrom_reboot_notify(struct notifier_block *nb,
	unsigned long action, void *data)
{
	const char *cmd = data;

	if (hisi_is_bootrom_cmd(cmd) || g_force_bootrom_reset) {
		g_force_bootrom_reset = 1;
		hisi_warmboot_disarm();
		hisi_invalidate_boot0_xloader();
	} else if (cmd && (!strcmp(cmd, "warm") ||
			   (!strcmp(cmd, "recovery") && hisi_warmboot_is_ready()))) {
		hisi_warmboot_prepare(cmd);
	}
	return NOTIFY_DONE;
}

static struct notifier_block hisi_bootrom_reboot_nb = {
	.notifier_call = hisi_bootrom_reboot_notify,
	.priority = INT_MAX,
};

static ssize_t bootrom_reboot_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "armed=%d\n", g_bootrom_armed);
}

static ssize_t bootrom_reboot_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;

	if (sysfs_streq(buf, "arm")) {
		ret = hisi_invalidate_boot0_xloader();
		return ret ? ret : count;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "now") ||
	    sysfs_streq(buf, "bootrom")) {
		g_force_bootrom_reset = 1;
		ret = hisi_invalidate_boot0_xloader();
		if (ret)
			return ret;
		kernel_restart("bootrom");
		return count;
	}
	return -EINVAL;
}

static struct kobj_attribute bootrom_reboot_attr =
	__ATTR(bootrom_reboot, 0600, bootrom_reboot_show, bootrom_reboot_store);


static void chear_powerhold_protect(void)
{
	uintptr_t protect_val;

	if (g_powerhold_protect_offset != 0xFFFF) {
		/* we need clear protect bit and set mask bit */
		protect_val = readl((void *)sysctrl_base + g_powerhold_protect_offset);
		if ((protect_val & BIT(g_powerhold_protect_bit)) == 0)
			BB_PRINT_ERR("protect_offset = 0x%x, protect_bit = 0x%x"
				"powerhold protect is already cleared!\n",
				g_powerhold_protect_offset, g_powerhold_protect_bit);
		protect_val = (protect_val & (~BIT(g_powerhold_protect_bit)))
						| (BIT(g_powerhold_protect_bit + POWERHOLD_PROTECT_BIT_OFFSET));
		writel(protect_val, (void *)sysctrl_base + g_powerhold_protect_offset);
	}
}

static void hisi_pm_system_off(void)
{
	unsigned int out_dir;

#ifdef CONFIG_HISI_BB
	set_reboot_reason(AP_S_COLDBOOT);
#endif


	while (1) {
		if (powerhold_gpio_base != NULL) {
			pr_info("system power off now\n");
			/* clear powerhold protect */
			chear_powerhold_protect();
			/* set direction */
			out_dir = readl(SOC_GPIO_GPIODIR_ADDR(powerhold_gpio_base));
			out_dir |= (1u << g_powerhold_gpio_offset);
			writel(out_dir, SOC_GPIO_GPIODIR_ADDR(powerhold_gpio_base));
			writel(0, powerhold_gpio_base + (1u << (g_powerhold_gpio_offset + POWERHOLD_GPIO_BIT_OFFSET)));

			mdelay(1000); /* wait 1s before machine_restart */
			machine_restart("chargereboot");
		}
	}
}

/*
 * Description:   Callback function registered with the machine_restart function.
 *                When the system is reset, the callback function is invoked
 * Input:         cmd  Reset Type
 * Output:        NA
 * Return:        NA
 */
static void hisi_pm_system_reset(enum reboot_mode mode, const char *cmd)
{
	bool bootrom_req = g_force_bootrom_reset || hisi_is_bootrom_cmd(cmd);
#ifdef CONFIG_HISI_BB
	unsigned int i;
	unsigned int curr_reboot_type = UNKNOWN;
	const struct cmdword *reboot_reason_map = NULL;
#endif

	if (hisi_warmboot_is_ready())
		hisi_warmboot_jump();

	if (cmd == NULL || *cmd == '\0') {
		BB_PRINT_PN("%s cmd is null\n", __func__);
		cmd = "COLDBOOT";
	} else {
		BB_PRINT_PN("%s cmd is %s\n", __func__, cmd);
	}
#ifdef CONFIG_HISI_BB
	reboot_reason_map = get_reboot_reason_map();
	if (reboot_reason_map == NULL) {
		BB_PRINT_ERR("reboot_reason_map is NULL\n");
		return;
	}
	for (i = 0; i < get_reboot_reason_map_size(); i++) {
		if (!strncmp((char *)reboot_reason_map[i].name, cmd, sizeof(reboot_reason_map[i].name))) {
			curr_reboot_type = reboot_reason_map[i].num;
			break;
		}
	}
	if (curr_reboot_type == UNKNOWN) {
		curr_reboot_type = COLDBOOT;
		if (!bootrom_req) {
			console_verbose();
			dump_stack();
		}
	}
	set_reboot_reason(curr_reboot_type);
	hisiap_nmi_notify_lpm3();
#endif
	if (bootrom_req && sysctrl_base != NULL) {
		mdelay(50);
		writel(SCTRL_SCSYSSTAT_RESET_VAL, SOC_SCTRL_SCSYSSTAT_ADDR(sysctrl_base));
	}
	BB_PRINT_PN("ap send nmi to lpm3, then goto dead loop\n");
	while (1)
		;
}
static int hi3xxx_reset_probe(struct platform_device *pdev)
{
	struct device_node *np = NULL;
	unsigned int offset = 0;
	int ret = 0;

	np = of_find_compatible_node(NULL, NULL, "hisi,powerhold");
	/* get powerhold gpio */
	if (np == NULL) {
		BB_PRINT_ERR("get powerhold np error !\n");
	} else {
		powerhold_gpio_base = of_iomap(np, IOMAP_ADD_INDEX);
		pr_info("powerhold_gpio_base = %pK !\n", powerhold_gpio_base);
		if (powerhold_gpio_base == NULL)
			BB_PRINT_ERR("%s: powerhold_gpio_base is NULL\n", __func__);

		ret = of_property_read_u32(np, "offset", (u32 *)&offset);
		if (ret) {
			BB_PRINT_ERR("get offset error !\n");
			powerhold_gpio_base = 0;
		} else {
			pr_info("offset = 0x%x !\n", offset);
			g_powerhold_gpio_offset = offset;
		}

		ret = of_property_read_u32(np, "powerhold_protect_offset", (u32 *)&offset);
		if (ret) {
			BB_PRINT_ERR("no powerhold_protect_offset !\n");
		} else {
			pr_info("powerhold_protect_offset = 0x%x !\n", offset);
			g_powerhold_protect_offset = offset;
		}

		ret = of_property_read_u32(np, "powerhold_protect_bit", (u32 *)&offset);
		if (ret) {
			BB_PRINT_ERR("no powerhold_protect_bit !\n");
			g_powerhold_protect_offset = 0xFFFF;
		} else {
			pr_info("powerhold_protect_bit = 0x%x !\n", offset);
			g_powerhold_protect_bit = offset;
		}
	}

	np = of_find_compatible_node(NULL, NULL, "hisilicon,sysctrl");
	if (np == NULL) {
		BB_PRINT_ERR("%s: hisilicon,sysctrl No compatible node found\n", __func__);
		return -ENODEV;
	}

	sysctrl_base = of_iomap(np, IOMAP_ADD_INDEX);
	if (sysctrl_base == NULL) {
		BB_PRINT_ERR("%s: hisilicon,sysctrl_base is NULL\n", __func__);
		return -ENODEV;
	}

	pm_power_off = hisi_pm_system_off;
	arm_pm_restart = hisi_pm_system_reset;

	ret = register_reboot_notifier(&hisi_bootrom_reboot_nb);
	if (ret)
		BB_PRINT_ERR("%s: register_reboot_notifier failed: %d\n", __func__, ret);

	ret = sysfs_create_file(kernel_kobj, &bootrom_reboot_attr.attr);
	if (ret)
		BB_PRINT_ERR("%s: sysfs_create_file bootrom_reboot failed: %d\n", __func__, ret);

	ret = hisi_warmboot_init_sysfs();
	if (ret)
		BB_PRINT_ERR("%s: sysfs_create_file warm_reboot failed: %d\n", __func__, ret);

	INIT_DELAYED_WORK(&g_bootrom_auto_work, hisi_bootrom_auto_arm_work_fn);
	schedule_delayed_work(&g_bootrom_auto_work, BOOTROM_AUTO_ARM_DELAY_JIFFIES);

	return 0;
}

static int hi3xxx_reset_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&g_bootrom_auto_work);
	hisi_warmboot_exit_sysfs();
	sysfs_remove_file(kernel_kobj, &bootrom_reboot_attr.attr);
	unregister_reboot_notifier(&hisi_bootrom_reboot_nb);

	if (pm_power_off == hisi_pm_system_off)
		pm_power_off = NULL;

	if (arm_pm_restart == hisi_pm_system_reset)
		arm_pm_restart = NULL;

	return 0;
}

static const struct of_device_id of_hi3xxx_reset_match[] = {
	{ .compatible = "hisilicon,hi3xxx-reset", },
	{},
};

static struct platform_driver hi3xxx_reset_driver = {
	.probe = hi3xxx_reset_probe,
	.remove = hi3xxx_reset_remove,
	.driver = {
		.name = "hi3xxx-reset",
		.owner = THIS_MODULE,
		.of_match_table = of_hi3xxx_reset_match,
	},
};

static int __init hi3xxx_reset_init(void)
{
	return platform_driver_register(&hi3xxx_reset_driver);
}
module_init(hi3xxx_reset_init);

static void __exit hi3xxx_reset_exit(void)
{
	platform_driver_unregister(&hi3xxx_reset_driver);
}
module_exit(hi3xxx_reset_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("hi3xxx reset driver");
MODULE_AUTHOR("Huawei Technologies Co., Ltd.");
