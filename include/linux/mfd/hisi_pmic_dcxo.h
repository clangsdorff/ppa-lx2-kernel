/*
 * hisi_pmic_dcxo.h
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

#ifndef	__HISI_PMIC_DCXO_H
#define	__HISI_PMIC_DCXO_H

#include <pmic_interface.h>
#include <linux/types.h>

#define NVE_CALC1C2_NAME "CALC1C2"
#define PMIC_DCXO_CFIX2_MASK 0xF

#ifdef PMIC_NP_CLK_TOP_CTRL1_ADDR
#define PMIC_DCXO_CFIX1  PMIC_NP_CLK_TOP_CTRL1_ADDR(0)
#define PMIC_DCXO_CFIX2  PMIC_NP_CLK_TOP_CTRL0_ADDR(0)
#else
#define PMIC_DCXO_CFIX1  PMIC_CLK_TOP_CTRL1_1_ADDR(0)
#define PMIC_DCXO_CFIX2  PMIC_CLK_TOP_CTRL1_0_ADDR(0)
#endif

struct pmu_dcxo {
	uint16_t dcxo_ctrim;
	uint16_t dcxo_c2_fix;
	uint16_t calibra_times;
};

#ifdef CONFIG_HISI_PMIC_DCXO
int pmu_dcxo_set(uint16_t dcxo_ctrim, uint16_t dcxo_c2_fix);
int pmu_dcxo_get(uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix);
int pmu_dcxo_reg_set(uint16_t dcxo_ctrim, uint16_t dcxo_c2_fix);
int pmu_dcxo_reg_get(uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix);
#else
static inline int pmu_dcxo_set(uint16_t dcxo_ctrim, uint16_t dcxo_c2_fix) { return 0; }
static inline int pmu_dcxo_get(uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix) { return 0; }
static inline int pmu_dcxo_reg_set(uint16_t dcxo_ctrim, uint16_t dcxo_c2_fix) { return 0; }
static inline int pmu_dcxo_reg_get(uint16_t *dcxo_ctrim, uint16_t *dcxo_c2_fix) { return 0; }
#endif

#endif
