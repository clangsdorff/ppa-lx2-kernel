/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2018-2019. All rights reserved.
 * Description: vcodec utils
 * Author:
 * Create: 2019-12-13
 */

#ifndef TVP_ADAPTER_H
#define TVP_ADAPTER_H

#include "hi_type.h"
#include "hi_drv_mem.h"

enum {
	HIVCODEC_CMD_ID_INIT = 1,
	HIVCODEC_CMD_ID_EXIT,
	HIVCODEC_CMD_ID_SUSPEND,
	HIVCODEC_CMD_ID_RESUME,
	HIVCODEC_CMD_ID_CONTROL,
	HIVCODEC_CMD_ID_RUN_PROCESS,
	HIVCODEC_CMD_ID_GET_IMAGE,
	HIVCODEC_CMD_ID_RELEASE_IMAGE,
	HIVCODEC_CMD_ID_CONFIG_INPUT_BUFFER,
	HIVCODEC_CMD_ID_READ_PROC,
	HIVCODEC_CMD_ID_WRITE_PROC,
	HIVCODEC_CMD_ID_MEM_CPY = 20,
	HIVCODEC_CMD_ID_CFG_MASTER,
	HIVCODEC_CMD_ID_RESET_MASTER,
};

struct config_master_addr {
	uint32_t smmu_err_rdaddr0;
	uint32_t smmu_err_rdaddr1;
	uint32_t smmu_err_wtaddr0;
	uint32_t smmu_err_wtaddr1;
	uint32_t err_addr_size;
	uint32_t smmu_page_base_addr0;
	uint32_t smmu_page_base_addr1;
	uint32_t smmu_common_base_addr;
};

HI_S32 init_kernel_ca(void);
HI_VOID deinit_kernel_ca(void);
HI_VOID config_master(HI_U32 smmu_common_base_addr,
	venc_smmu_err_add_t smmu_err_addr, HI_U64 smmu_page_base_addr);
HI_VOID reset_master(HI_U32 smmu_common_base_addr);

#endif
/** TVP_ADAPTER_H */