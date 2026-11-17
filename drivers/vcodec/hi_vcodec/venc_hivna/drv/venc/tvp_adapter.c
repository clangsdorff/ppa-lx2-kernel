/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2018-2019. All rights reserved.
 * Description: vcodec kernel ca
 * Author:
 * Create: 2019-12-13
 */
#include "tvp_adapter.h"
#include <teek_client_api.h>
#include "drv_venc_osal.h"

static TEEC_Context g_tee_context;
static TEEC_Session g_tee_session;

 HI_S32 init_kernel_ca(void)
{
	TEEC_UUID uuid = {
		0x528822b7, 0xfc78, 0x466b,
		{ 0xb5, 0x7e, 0x62, 0x09, 0x3d, 0x60, 0x34, 0xa7 }
	};
	const char *package_name = "/vendor/bin/hw/android.hardware.media.omx@1.0-service";
	const uint32_t uid = 0;
	TEEC_Operation operation;
	TEEC_Result result;

	memset_s(&operation, sizeof(operation), 0, sizeof(operation));
	memset_s(&g_tee_context, sizeof(g_tee_context), 0, sizeof(g_tee_context));
	memset_s(&g_tee_session, sizeof(g_tee_session), 0, sizeof(g_tee_session));

	result = TEEK_InitializeContext(NULL, &g_tee_context);
	if (result != TEEC_SUCCESS) {
		HI_ERR_VENC("TEEK_InitializeContext Failed\n");
		return -1;
	}

	operation.started = 1;
	operation.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE,
						TEEC_NONE,
						TEEC_MEMREF_TEMP_INPUT,
						TEEC_MEMREF_TEMP_INPUT);
	operation.params[2].tmpref.buffer = (void *)&uid;
	operation.params[2].tmpref.size = sizeof(uid);
	operation.params[3].tmpref.buffer = (void *)package_name;
	operation.params[3].tmpref.size = strlen(package_name) + 1;

	result = TEEK_OpenSession(&g_tee_context, &g_tee_session,
		&uuid, TEEC_LOGIN_IDENTIFY, NULL, &operation, NULL);
	if (g_tee_session.context == NULL) {
		HI_ERR_VENC("g_tee_session.context is nullptr\n");
		return -1;
	}

	if (result != TEEC_SUCCESS) {
		HI_ERR_VENC("TEEC_OpenSession Failed\n");
		TEEK_FinalizeContext(&g_tee_context);
		return -1;
	}
	HI_INFO_VENC("TEEK_InitializeContext success.\n");
	return 0;
}

HI_VOID deinit_kernel_ca(void)
{
	TEEK_CloseSession(&g_tee_session);
	TEEK_FinalizeContext(&g_tee_context);
}

HI_VOID config_master(HI_U32 smmu_common_base_addr,
	venc_smmu_err_add_t smmu_err_addr, HI_U64 smmu_page_base_addr)
{
	TEEC_Result result;
	TEEC_Operation operation;
	struct config_master_addr s_addr;

	memset_s(&operation, sizeof(operation), 0, sizeof(TEEC_Operation));

	s_addr.smmu_err_rdaddr0 = (uint32_t)(smmu_err_addr.read_addr >> 32);
	s_addr.smmu_err_rdaddr1 = (uint32_t)(smmu_err_addr.read_addr);
	s_addr.smmu_err_wtaddr0 = (uint32_t)(smmu_err_addr.write_addr >> 32);
	s_addr.smmu_err_wtaddr1 = (uint32_t)(smmu_err_addr.write_addr);
	s_addr.err_addr_size = (uint32_t)(smmu_err_addr.size);
	s_addr.smmu_page_base_addr0 = (uint32_t)(smmu_page_base_addr >> 32);
	s_addr.smmu_page_base_addr1 = (uint32_t)(smmu_page_base_addr);
	s_addr.smmu_common_base_addr = (uint32_t)(smmu_common_base_addr);

	operation.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
						TEEC_NONE,
						TEEC_NONE,
						TEEC_NONE);
	operation.started = 1;
	operation.params[0].tmpref.buffer = &s_addr;
	operation.params[0].tmpref.size  = sizeof(struct config_master_addr);

	result = TEEK_InvokeCommand(&g_tee_session, HIVCODEC_CMD_ID_CFG_MASTER,
		&operation, NULL);
	if (result != TEEC_SUCCESS)
		HI_ERR_VENC("TEEC_InvokeCommand HIVCODEC_CMD_ID_CFG_MASTER failed");
}

HI_VOID reset_master(HI_U32 smmu_common_base_addr)
{
	TEEC_Result result;
	TEEC_Operation operation;
	struct config_master_addr s_addr;

	memset_s(&operation, sizeof(operation), 0, sizeof(TEEC_Operation));

	s_addr.smmu_common_base_addr = (uint32_t)(smmu_common_base_addr);

	operation.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
						TEEC_NONE,
						TEEC_NONE,
						TEEC_NONE);
	operation.started = 1;
	operation.params[0].tmpref.buffer = &s_addr;
	operation.params[0].tmpref.size  = sizeof(struct config_master_addr);

	result = TEEK_InvokeCommand(&g_tee_session, HIVCODEC_CMD_ID_RESET_MASTER,
		&operation, NULL);
	if (result != TEEC_SUCCESS)
		HI_ERR_VENC("TEEC_InvokeCommand HIVCODEC_CMD_ID_RESET_MASTER failed\n");
}
