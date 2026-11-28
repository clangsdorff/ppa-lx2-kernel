#include <asm/compiler.h>
#include <linux/compiler.h>
#include <linux/fd.h>
#include <linux/tty.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/semaphore.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/hisi/ipc_msg.h>
#include <linux/hisi/hisi_rproc.h>
#include <linux/hisi/kirin_partition.h>
#include <linux/clk.h>
#include <linux/mm.h>
#include <securec.h>
#include "soc_acpu_baseaddr_interface.h"
#include "soc_sctrl_interface.h"
#include "hisi_hisee.h"
#include "hisi_hisee_power.h"
#include "hisi_hisee_fs.h"
#include "hisi_hisee_upgrade.h"
#include "hisi_hisee_chip_test.h"
#ifdef CONFIG_HUAWEI_DSM
#include <dsm/dsm_pub.h>
#endif

#include "hisi_flash_hisee_otp.h"

#define HISEE_EFUSE_GROUP_BIT_SIZE    32
#define HISEE_PWR_ERRNO_PRINT_SIZE	8
#define HISEE_EFUSE_GROUP_NUM 2
#define HISEE_EFUSE_LENGTH 8
#define HISEE_EFUSE_TIMEOUT 1000
#define HISEE_EFUSE_MASK 1
#define HISEE_SM_EFUSE_GROUP    (g_hisee_sm_efuse_pos/HISEE_EFUSE_GROUP_BIT_SIZE)
#define HISEE_SM_EFUSE_OFFSET   (g_hisee_sm_efuse_pos%HISEE_EFUSE_GROUP_BIT_SIZE)
#define HISEE_HIBENCH_EFUSE_GROUP    (g_hisee_hibench_efuse_pos/HISEE_EFUSE_GROUP_BIT_SIZE)
#define HISEE_HIBENCH_EFUSE_OFFSET   (g_hisee_hibench_efuse_pos%HISEE_EFUSE_GROUP_BIT_SIZE)



hisee_module_data g_hisee_data;

struct mutex hisee_apdu_mutex;
struct mutex hisee_otp_mutex;

atomic_t g_hisee_errno;
unsigned int g_misc_version[HISEE_SUPPORT_COS_FILE_NUMBER] = {0};
int g_hisee_partition_byname_find = 0;
unsigned int g_platform_id = 0;
unsigned int g_cos_image_upgrade_done = 0;

bool g_hisee_api_bypassed = false;
bool g_hisee_file_ioctl_ready = false;

/*
 the bit position for SM flag recorded in efuse
 which is started from 0.
 defualt value is invalid
*/
static u32 g_hisee_sm_efuse_pos = 0xFFFF;

/*
 the bit position for hibench flag recorded in efuse
 which is started from 0.
 defualt value is invalid
*/
static u32 g_hisee_hibench_efuse_pos = 0xFFFF;
/*
 cos upgrade procedure may cost more time on non-asic platform
*/
u32 g_hisee_cos_upgrade_time = HISEE_ATF_COS_TIMEOUT;
bool g_hisee_is_fpga = false;
extern unsigned int g_power_pre_enable_clk;
extern hisee_power_vote_status g_power_vote_status;
extern unsigned int g_runtime_cosid;


/*  hisee autoteset interface entryfor cmd "hisee_channel_test" */
int hisee_auto_test_func(const void *buf, int para);

extern int get_rpmb_init_status(void);
extern int get_rpmb_key_status(void);
extern int get_efuse_hisee_value(unsigned char *pu8Buffer, unsigned int u32Length, unsigned int timeout);
extern int set_efuse_hisee_value(unsigned char *pu8Buffer, unsigned int u32Length, unsigned int timeout);
extern void register_flash_hisee_otp_fn(int (*fn_ptr)(void));
#ifdef CONFIG_HISI_HISEE_MNTN
extern u32 hisee_mntn_get_vote_val_lpm3(void);
extern u32 hisee_mntn_get_vote_val_atf(void);
extern int hisee_mntn_collect_vote_value_cmd(void);
extern void hisee_mntn_update_local_lcsmode_val(void);
extern int hisee_mntn_record_dmd_info(long dmd_no, const char *dmd_info);
#endif
static hisee_errcode_item_des g_errcode_items_des[] = {
	{HISEE_OK, "no error\n"},
	{HISEE_ERROR, "general error\n"},
	{HISEE_NO_RESOURCES, "general no resources\n"},
	{HISEE_INVALID_PARAMS, "general invalid paramters\n"},
	{HISEE_CMA_DEVICE_INIT_ERROR, "cma device init error\n"},
	{HISEE_IOCTL_NODE_CREATE_ERROR, "ioctl node create error\n"},
	{HISEE_POWER_NODE_CREATE_ERROR, "power node create error\n"},
	{HISEE_THREAD_CREATE_ERROR, "thread create error\n"},
	{HISEE_RPMB_MODULE_INIT_ERROR, "rpmb module init error\n"},
	{HISEE_BULK_CLK_INIT_ERROR, "bulk clk dts node init error\n"},
	{HISEE_BULK_CLK_ENABLE_ERROR, "bulk clk error\n"},

	{HISEE_RPMB_KEY_WRITE_ERROR, "write rpmb key fail\n"},
	{HISEE_RPMB_KEY_READ_ERROR, "read rpmb key fail\n"},
	{HISEE_RPMB_KEY_UNREADY_ERROR, "rpmb key not ready error\n"},
	{HISEE_COS_VERIFICATITON_ERROR, "cos img verify fail\n"},
	{HISEE_IMG_PARTITION_MAGIC_ERROR, "img partition magic error\n"},
	{HISEE_IMG_PARTITION_FILES_ERROR, "img partition files error\n"},
	{HISEE_IMG_SUB_FILE_NAME_ERROR, "sub image invalid error\n"},
	{HISEE_SUB_FILE_SIZE_CHECK_ERROR, "sub file size error\n"},
	{HISEE_SUB_FILE_OFFSET_CHECK_ERROR, "sub file offset error\n"},
	{HISEE_IMG_SUB_FILE_ABSENT_ERROR, "sub image absent error\n"},
	{HISEE_FS_SUB_FILE_ABSENT_ERROR, "sub fs file absent error\n"},

	{HISEE_OPEN_FILE_ERROR, "open file error\n"},
	{HISEE_READ_FILE_ERROR, "read file error\n"},
	{HISEE_WRITE_FILE_ERROR, "write file error\n"},
	{HISEE_CLOSE_FILE_ERROR, "close file error\n"},
	{HISEE_LSEEK_FILE_ERROR, "seek file error\n"},
	{HISEE_OUTOF_RANGE_FILE_ERROR, "out of file threshold error\n"},

	{HISEE_FS_MALLOC_ERROR, "fs partition malloc error\n"},
	{HISEE_FS_PATH_ABSENT_ERROR, "fs partition absent error\n"},
	{HISEE_FS_OPEN_PATH_ERROR, "fs partition open error\n"},
	{HISEE_FS_COUNT_FILES_ERROR, "fs partition calculate files error\n"},
	{HISEE_FS_PATH_LONG_ERROR, "sub fs file path too long error\n"},
	{HISEE_FS_READ_FILE_ERROR, "sub fs file read error\n"},

	{HISEE_POWERCTRL_TIMEOUT_ERROR, "hisee powerctrl timeout error\n"},
	{HISEE_POWERCTRL_NOTIFY_ERROR, "hisee powerctrl notify error\n"},
	{HISEE_POWERCTRL_RETRY_FAILURE_ERROR, "hisee powerctrl retry failure error\n"},
	{HISEE_POWERCTRL_FLOW_ERROR, "hisee powerctrl flow error\n"},

	{HISEE_FIRST_SMC_CMD_ERROR, "the fisrt step in smc transcation error\n"},
	{HISEE_SMC_CMD_TIMEOUT_ERROR, "the smc transcation timeout error\n"},
	{HISEE_SMC_CMD_PROCESS_ERROR, "the smc transcation failure error\n"},
	{HISEE_GET_HISEE_VALUE_ERROR,"get hisee lcs mode error\n"},
	{HISEE_SET_HISEE_VALUE_ERROR,"set hisee lcs mode error\n"},
	{HISEE_SET_HISEE_STATE_ERROR,"set hisee state error\n"},

	{HISEE_WAIT_READY_TIMEOUT,"wait hisee ready timeout\n"},

	{HISEE_ENCOS_HEAD_INIT_ERROR, "hisee encos head init error\n"},
	{HISEE_COS_IMG_ID_ERROR, "the input cosid error\n"},
	{HISEE_ENCOS_PARTITION_MAGIC_ERROR, "hisee encos magic num error\n"},
	{HISEE_ENCOS_PARTITION_FILES_ERROR, "hisee encos files error\n"},
	{HISEE_ENCOS_PARTITION_SIZE_ERROR, "hisee encos file size error\n"},
	{HISEE_ENCOS_SUBFILE_SIZE_CHECK_ERROR, "hisee encos subfile size error\n"},
	{HISEE_ENCOS_OPEN_FILE_ERROR, "hisee encos open file error\n"},
	{HISEE_ENCOS_FIND_PTN_ERROR, "hisee encos find ptn error\n"},
	{HISEE_ENCOS_LSEEK_FILE_ERROR, "hisee encos lseek error\n"},
	{HISEE_ENCOS_WRITE_FILE_ERROR, "hisee encos write file error\n"},
	{HISEE_ENCOS_READ_FILE_ERROR, "hisee encos read file error\n"},
	{HISEE_ENCOS_ACCESS_FILE_ERROR, "hisee encos access file error\n"},
	{HISEE_ENCOS_CHECK_HEAD_ERROR, "hisee encos check file head error\n"},
	{HISEE_ENCOS_NAME_NULL_ERROR, "hisee encos file name is null\n"},

	{HISEE_OLD_COS_IMAGE_ERROR,"old cos image error\n"}
};

#define MANAFACTOR_CMD_INDEX 2

static hisee_driver_function g_hisee_atf_function_list[] = {
	{"cos_image_upgrade", cos_image_upgrade_func},
	{"hisee_channel_test", hisee_auto_test_func},
	{"hisee_parallel_factory_action", hisee_parallel_manufacture_func},


	{"hisee_factory_check", hisee_factory_check_func},
#if defined(CONFIG_HISI_SMX_PROCESS) || defined(CONFIG_HISEE_AT_SMX)
	{"hisee_get_smx", hisee_get_smx_func},
#endif
	{NULL, NULL},
};

static hisee_driver_function g_hisee_lpm3_function_list[] = {
	{"hisee_poweron_upgrade", hisee_poweron_upgrade_func},
	{"hisee_poweron_timeout", hisee_poweron_timeout_func},
	{"hisee_poweron", hisee_poweron_booting_func},
	{"hisee_poweroff", hisee_poweroff_func},
	{NULL, NULL},
};

/*
    hisee autoteset interface entryfor cmd "hisee_channel_test"
*/
int hisee_auto_test_func(const void *buf, int para)
{
	return HISEE_OK;
}


noinline int atfd_hisee_smc(u64 _function_id, u64 _arg0, u64 _arg1, u64 _arg2)
{
    register u64 function_id asm("x0") = _function_id;
    register u64 arg0 asm("x1") = _arg0;
    register u64 arg1 asm("x2") = _arg1;
    register u64 arg2 asm("x3") = _arg2;
    asm volatile(
            __asmeq("%0", "x0")
            __asmeq("%1", "x1")
            __asmeq("%2", "x2")
            __asmeq("%3", "x3")
            ".inst 0xd4000003 /* smc #0 */\n"
        : "+r" (function_id)
        : "r" (arg0), "r" (arg1), "r" (arg2));

    return (int)function_id;
} /*lint !e715*/



int get_hisee_lcs_mode(unsigned int *mode)
{
	int ret;
	u32 hisee_value[HISEE_EFUSE_GROUP_NUM] = {0};

	if(HISEE_SM_EFUSE_GROUP >= HISEE_EFUSE_GROUP_NUM) {
		pr_err("sm_flag_pos invalid\n");
		set_errno_and_return(HISEE_GET_HISEE_VALUE_ERROR);
	}

	ret = get_efuse_hisee_value((unsigned char *)hisee_value, HISEE_EFUSE_LENGTH, HISEE_EFUSE_TIMEOUT);
	if (HISEE_OK != ret) {
		pr_err("%s() get_efuse_hisee_value failed,ret=%d\n", __func__, ret);
		set_errno_and_return(HISEE_GET_HISEE_VALUE_ERROR);
	}
	*mode = ((hisee_value[HISEE_SM_EFUSE_GROUP] >> HISEE_SM_EFUSE_OFFSET) & HISEE_EFUSE_MASK) ?\
		 HISEE_SM_MODE_MAGIC : HISEE_DM_MODE_MAGIC;
	return ret;
}

int get_hisee_hibench_flag(unsigned int *mode)
{
	int ret;
	u32 hisee_value[HISEE_EFUSE_GROUP_NUM] = {0};

	if(HISEE_HIBENCH_EFUSE_GROUP >= HISEE_EFUSE_GROUP_NUM) {
		pr_err("hibench_flag_pos invalid\n");
		set_errno_and_return(HISEE_GET_HISEE_VALUE_ERROR);
	}

	ret = get_efuse_hisee_value((unsigned char *)hisee_value, HISEE_EFUSE_LENGTH, HISEE_EFUSE_TIMEOUT);
	if (HISEE_OK != ret) {
		pr_err("%s() get_efuse_hisee_value failed,ret=%d\n", __func__, ret);
		set_errno_and_return(HISEE_GET_HISEE_VALUE_ERROR);
	}
	*mode = ((hisee_value[HISEE_HIBENCH_EFUSE_GROUP] >> HISEE_HIBENCH_EFUSE_OFFSET) & HISEE_EFUSE_MASK);
	return ret;
}

int set_hisee_lcs_sm_efuse(void)
{
	int ret;
	u32 hisee_value[2] = {0};

	if (HISEE_SM_EFUSE_GROUP >= 2) {
		pr_err("sm_flag_pos invalid\n");
		set_errno_and_return(HISEE_SET_HISEE_VALUE_ERROR);
	}

	hisee_value[HISEE_SM_EFUSE_GROUP] = (((u32)0x1) << HISEE_SM_EFUSE_OFFSET);
	ret = set_efuse_hisee_value((unsigned char *)hisee_value, 8, 1000);
	if (HISEE_OK != ret) {
		pr_err("%s() set_efuse_hisee_value failed,ret=%d\n", __func__, ret);
		set_errno_and_return(HISEE_SET_HISEE_VALUE_ERROR);
	}
#ifdef CONFIG_HISI_HISEE_MNTN
	/*update local variable of lcs mode*/
	hisee_mntn_update_local_lcsmode_val();
#endif
	check_and_print_result();
	return ret;
}

#ifdef CONFIG_HUAWEI_DSM
static void record_hisee_log_by_dmd(long dmd_errno, int hisee_errno)
{
	int ret;
	char buff[HISEE_CMD_NAME_LEN] = {0};
	int i;

	(void)memset_s(buff, HISEE_CMD_NAME_LEN, 0, (unsigned long)HISEE_CMD_NAME_LEN);
	for (i = 0; i < ARRAY_SIZE(g_errcode_items_des); i++) {/*lint !e846 !e514 !e866 !e30 !e84 !e574 !e737*/
		if (hisee_errno == g_errcode_items_des[i].err_code) {
			break;
		}
	}
	if ((unsigned long)i == ARRAY_SIZE(g_errcode_items_des)) {/*lint !e846 !e514 !e866 !e30 !e84 !e574 !e737 !e571*/
		pr_err("%s(): can't find errcode=%d definition\n", __func__, hisee_errno);
		return;
	}

	ret = snprintf_s(buff, HISEE_CMD_NAME_LEN, (unsigned long)8, "%d,", hisee_errno);
	if (ret == HISEE_SECLIB_ERROR) {
		pr_err("%s(): snprintf failed\n", __func__);
		return;
	}

	ret = strncat_s(buff, HISEE_CMD_NAME_LEN, g_errcode_items_des[i].err_description,
					(unsigned long)((HISEE_ERROR_DESCRIPTION_MAX - 1) - strlen(buff)));/*lint !e662*/
	if (ret != EOK) {
		pr_err("%s(): strncat failed\n", __func__);
		return;
	}

#ifdef CONFIG_HISI_HISEE_MNTN
	ret = hisee_mntn_record_dmd_info(dmd_errno, (const char *)buff);
	if (0 != ret)
		pr_err("%s(): hisee_mntn_record_dmd_info return %d failed\n", __func__, ret);
#endif

	return;
}
#endif

/*
 * hisi_hisee_active - handle hisee request from ATF
 * if wait_sem_timeout is not zero, means there has been a timeout
 * before this IPI happens, then only decrease it;
 * another case:if smc_cmd_running is zero, there nothing need to do;
 * otherwise, do up() a semaphore
 */
void hisi_hisee_active(void)
{
	if (g_hisee_data.smc_cmd_running) {
		up(&(g_hisee_data.atf_sem));
	}
}
EXPORT_SYMBOL(hisi_hisee_active);

void set_message_header(atf_message_header *header, unsigned int cmd_type)
{
	header->ack = 0;
	header->cmd = cmd_type;
	header->test_result_phy = 0;
	header->test_result_size = 0;
	return;
}

int send_smc_process(atf_message_header *p_message_header, phys_addr_t phy_addr, unsigned int size,
							unsigned int timeout, se_smc_cmd smc_cmd)
{
	int ret = HISEE_OK;
	long local_jiffies;

	mutex_lock(&g_hisee_data.hisee_mutex);
	g_hisee_data.smc_cmd_running = 1;

	if (CMD_HISEE_CHANNEL_TEST != smc_cmd)
		ret = atfd_hisee_smc((u64)HISEE_FN_MAIN_SERVICE_CMD, (u64)smc_cmd, (u64)phy_addr, (u64)size);
	else
		ret = atfd_hisee_smc((u64)HISEE_FN_CHANNEL_TEST_CMD, (u64)smc_cmd, (u64)phy_addr, (u64)size);
	if (ret != HISEE_OK) {
		pr_err("%s(): atfd_hisee_smc failed, ret=%d\n", __func__, ret);
		g_hisee_data.smc_cmd_running = 0;
		mutex_unlock(&g_hisee_data.hisee_mutex);
		set_errno_and_return(HISEE_FIRST_SMC_CMD_ERROR);
	}

	/*notice! while starting a new smc message, no need to set wait_sem_timeout to zero.
	 *because this flag only set in timeout scenario, so only clear in IPI context.
	 *If the wait_sem_timeout is set in the second smc message, there may be a uncorrection:
	 *the fisrt IPI interrupt should make the semaphore availble, then the second acquire semaphore
	 *success directly.
	*/
	if (CMD_HISEE_CHANNEL_TEST != smc_cmd)
		local_jiffies = msecs_to_jiffies(timeout);
	else
		local_jiffies = MAX_SCHEDULE_TIMEOUT;
	ret = down_timeout(&(g_hisee_data.atf_sem), local_jiffies);
	if (-ETIME == ret) {
		ret = HISEE_SMC_CMD_TIMEOUT_ERROR;
	} else {
		if (!(p_message_header->cmd == smc_cmd &&
			p_message_header->ack == HISEE_ATF_ACK_SUCCESS))
			ret = HISEE_SMC_CMD_PROCESS_ERROR;
		else
			ret = HISEE_OK;
	}

	pr_err("%s() ret=%d\n", __func__, ret);
	g_hisee_data.smc_cmd_running = 0;
	mutex_unlock(&g_hisee_data.hisee_mutex);
	set_errno_and_return(ret);
}

/***************************************************************************
