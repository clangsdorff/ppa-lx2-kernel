/*
 * hisi_pmic_dcxo.c
 *
 * driver for pmic dcxo calibration
 *
 * Copyright (c) 2013-2019 Huawei Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * either version 2 of that License or (at your option) any later version.
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/mfd/hisi_pmic_dcxo.h>
#include <linux/mfd/hisi_pmic.h>
#include <linux/mtd/hisi_nve_number.h>
#include "../mtd/hisi_nve.h"
#include <linux/module.h>
#include <linux/printk.h>
#include <securec.h>

EXPORT_SYMBOL(pmu_dcxo_get);
EXPORT_SYMBOL(pmu_dcxo_set);
EXPORT_SYMBOL(pmu_dcxo_reg_get);
EXPORT_SYMBOL(pmu_dcxo_reg_set);

static u32 c1_reg;
static u32 c2_reg;
static u32 c2_mask;
static u32 modem_cali_in_mmw_pmu;

static unsigned int pmic_reg_read(int addr)
{
	if (modem_cali_in_mmw_pmu)
		return hisi_mmw_pmic_reg_read(addr);
	else
		return hisi_pmic_reg_read(addr);
}

static void pmic_reg_write(int addr, int val)
{
	if (modem_cali_in_mmw_pmu)
		return hisi_mmw_pmic_reg_write(addr, val);
	else
		return hisi_pmic_reg_write(addr, val);
}

static int pmu_dcxo_get_set(
	uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix, bool get)
{
	int ret;
	struct pmu_dcxo *dcxo = NULL;
	struct hisi_nve_info_user nve = {0};
	errno_t err = EOK;

	if (!dcxo_ctrim || !dcxo_c2_fix)
		return -EFAULT;

	err = strncpy_s(nve.nv_name, sizeof(nve.nv_name),
			NVE_CALC1C2_NAME, sizeof(NVE_CALC1C2_NAME) - 1);
	if (err != EOK)
		pr_err("[%s]nve.nv_name strncpy_s failed\n", __func__);
	nve.nv_number = NVE_CALC1C2_NUM;
	nve.valid_size = sizeof(struct pmu_dcxo);
	nve.nv_operation = NV_READ;

	ret = hisi_nve_direct_access(&nve);
	if (ret) {
		pr_err("[%s]nve get dcxo failed: 0x%x\n", __func__, ret);
		return ret;
	}

	dcxo = (struct pmu_dcxo *)nve.nv_data;

	if (get) {
		*dcxo_ctrim = dcxo->dcxo_ctrim;
		*dcxo_c2_fix = dcxo->dcxo_c2_fix;
		pr_err("[%s]get dcxo ctrim = 0x%x, dcxo_c2_fix = 0x%x\n",
			__func__, *dcxo_ctrim, *dcxo_c2_fix);
		return ret;
	}

	dcxo->calibra_times++;
	dcxo->dcxo_ctrim = *dcxo_ctrim;
	dcxo->dcxo_c2_fix = *dcxo_c2_fix;

	nve.nv_operation = NV_WRITE;

	ret = hisi_nve_direct_access(&nve);
	if (ret)
		pr_err("[%s]nve set dcxo failed: 0x%x\n", __func__, ret);
	else
		pr_err("[%s]%d times setting dcxo\n", __func__, dcxo->calibra_times);

	return ret;
}

static void pmic_reg_write_mask(u32 addr, u32 value, u32 mask)
{
	uint32_t reg_tmp = 0;

	reg_tmp = pmic_reg_read(addr);
	reg_tmp &= ~mask;
	reg_tmp |= value;
	pmic_reg_write(addr, reg_tmp);
}

static int pmu_dcxo_reg_get_set(
	uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix, bool get)
{
	uint32_t i;
	uint32_t value;
	uint16_t dcxo_c1;
	uint16_t dcxo_c2;

	if (!dcxo_ctrim || !dcxo_c2_fix)
		return -EFAULT;

	if (get) {
		value = pmic_reg_read(c1_reg);
		*dcxo_ctrim = (uint16_t)value;
		value = pmic_reg_read(c2_reg);
		value = value & c2_mask;
		*dcxo_c2_fix = (uint16_t)value;
		return 0;
	}

	/* HISI_PMU_DCXO_CFIX1 */
	value = pmic_reg_read(c1_reg);
	dcxo_c1 = *dcxo_ctrim;
	if (value < dcxo_c1) {
		for (i = value + 1; i <= dcxo_c1; i++)
			pmic_reg_write(c1_reg, i);
	} else if (value > dcxo_c1) {
		for (i = value - 1; i >= dcxo_c1; i--) {
			pmic_reg_write(c1_reg, i);
			if (i == 0)
				break;
		}
	}

	value = pmic_reg_read(c1_reg);
	pr_err("%s, pmu reread dcxo c1 = 0x%x\n", __func__, value);

	/* HISI_PMU_DCXO_CFIX2 */
	value = pmic_reg_read(c2_reg);
	value = value & c2_mask;
	dcxo_c2 = *dcxo_c2_fix;

	if (value < dcxo_c2) {
		for (i = value + 1; i <= dcxo_c2; i++)
			pmic_reg_write_mask(c2_reg, i, c2_mask);
	} else if (value > dcxo_c2) {
		for (i = value - 1; i >= dcxo_c2; i--) {
			pmic_reg_write_mask(c2_reg, i, c2_mask);
			if (i == 0)
				break;
		}
	}
	value = pmic_reg_read(c2_reg);
	value = value & c2_mask;
	pr_err("%s, pmu reread dcxo c2 = 0x%x\n", __func__, value);

	return 0;
}

int pmu_dcxo_set(uint16_t dcxo_ctrim, uint16_t dcxo_c2_fix)
{
	pr_err("[%s]set dcxo ctrim = 0x%x, dcxo_c2_fix = 0x%x\n",
		__func__, dcxo_ctrim, dcxo_c2_fix);

	return pmu_dcxo_get_set(&dcxo_ctrim, &dcxo_c2_fix, false);
}

int pmu_dcxo_get(uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix)
{
	return pmu_dcxo_get_set(dcxo_ctrim, dcxo_c2_fix, true);
}

int pmu_dcxo_reg_set(uint16_t dcxo_ctrim, uint16_t dcxo_c2_fix)
{
	pr_err("[%s]set dcxo reg ctrim = 0x%x, dcxo_c2_fix = 0x%x\n",
		__func__, dcxo_ctrim, dcxo_c2_fix);
	return pmu_dcxo_reg_get_set(&dcxo_ctrim, &dcxo_c2_fix, false);
}

int pmu_dcxo_reg_get(uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix)
{
	return pmu_dcxo_reg_get_set(dcxo_ctrim, dcxo_c2_fix, true);
}

int pmu_dcxo_get_test(int get, uint16_t v1, uint16_t v2)
{
	int ret;
	uint16_t dcxo_ctrim = v1;
	uint16_t dcxo_c2_fix = v2;

	ret = pmu_dcxo_get_set(&dcxo_ctrim, &dcxo_c2_fix, get);

	pr_err("[%s]test get trim value, ctrim = 0x%x, dcxo_c2_fix = 0x%x\n",
		__func__, dcxo_ctrim, dcxo_c2_fix);

	return ret;
}

static int __init pmu_dcxo_cali_init(void)
{
	int ret;

	ret = of_property_read_u32(of_find_compatible_node(NULL, NULL,
		"hisi,pmic_dcxo_cp"),  "modem_cali_in_mmw_pmu",
		(u32 *)&modem_cali_in_mmw_pmu);
	if (ret || modem_cali_in_mmw_pmu == 0)
		goto use_default;

	ret = of_property_read_u32(of_find_compatible_node(NULL, NULL,
		"hisi,pmic_dcxo_cp"), "c1_reg", (u32 *)&c1_reg);

	ret += of_property_read_u32(of_find_compatible_node(NULL, NULL,
		"hisi,pmic_dcxo_cp"), "c2_reg", (u32 *)&c2_reg);

	ret += of_property_read_u32(of_find_compatible_node(NULL, NULL,
		"hisi,pmic_dcxo_cp"), "c2_mask", (u32 *)&c2_mask);

	pr_info("[%s] dts config: c1 reg 0x%x, c2 reg 0x%x, c2 mask 0x%x\n",
		__func__, c1_reg, c2_reg, c2_mask);

	if (ret) {
		pr_err("[%s] use default value\n", __func__);
		goto use_default;
	}
	return ret;

use_default:
	c1_reg = PMIC_DCXO_CFIX1;
	c2_reg = PMIC_DCXO_CFIX2;
	c2_mask = PMIC_DCXO_CFIX2_MASK;
	pr_info("[%s]use_default: c1 reg 0x%x, c2 reg 0x%x, c2 mask 0x%x\n",
		__func__, c1_reg, c2_reg, c2_mask);

	return 0;
}

late_initcall_sync(pmu_dcxo_cali_init);
