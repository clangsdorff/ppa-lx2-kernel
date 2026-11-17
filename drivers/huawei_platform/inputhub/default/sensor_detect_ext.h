/*
 * drivers/inputhub/sensor_detect_ext.h
 *
 * sensors detection header file
 *
 * Copyright (c) 2012-2019 Huawei Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

 #ifndef __SENSOR_DETECT_EXT_H
#define __SENSOR_DETECT_EXT_H

#include "protocol.h"

#define SENSOR_PLATFORM_EXTEND_DATA_SIZE 50
#define ADAPT_SENSOR_LIST_NUM 20
#define TP_REPLACE_PS 1
#define WIA_NUM 6
#define DATA_FLAG_LEN 4
#define HORIZONTAL_PICKUP_PARA_LEN 16

#define CURRENT_FAC_AKM_INIT  0

#define GPIO_STAT_HIGH        1
#define GPIO_STAT_LOW         0
#define RESET_SHORT_SLEEP     5
#define RESET_LONG_SLEEP      10
#define FPC_NAME_LEN          3
#define SYNA_NAME_LEN         4
#define GOODIX_NAME_LEN       6
#define GOODIX_G2_NAME_LEN    15
#define SILEAD_NAME_LEN       6
#define QFP_NAME_LEN          3
#define EGIS_NAME_LEN         4
#define FP_SPI_NUM            2
#define RET_FAIL              (-1)
#define RET_SUCC              0
#define ALS_UD_CALI_LEN       4

#define GOODIX_WRITE_CMD             0xF0
#define GF_G3_G5_CHIP_ID_ADDR_HIGH   0x00
#define GF_G3_G5_CHIP_ID_ADDR_LOW    0x00
#define GOODIX_WRITE_CMD_TX_LEN      3
#define GF_G3_G5_NAME_LEN            15
#define GF_G5X_NAME_LEN              16
#define WORD_LEN_LOW_BIT  0x01
#define WORD_LEN_HIGH_BIT 0x00
#define GOODIX_WRITE_WAKEUP_CMD_TX_LEN 7
typedef uint16_t GPIO_NUM_TYPE;

struct sensor_combo_cfg {
	uint8_t bus_type;
	uint8_t bus_num;
	uint8_t disable_sample_thread;
	union {
		uint32_t data;
		uint32_t i2c_address;
		union SPI_CTRL ctrl;
	};
} __packed;

struct g_sensor_platform_data {
	struct sensor_combo_cfg cfg;
	uint8_t axis_map_x;
	uint8_t axis_map_y;
	uint8_t axis_map_z;
	uint8_t negate_x;
	uint8_t negate_y;
	uint8_t negate_z;
	uint8_t used_int_pin;
	GPIO_NUM_TYPE gpio_int1;
	GPIO_NUM_TYPE gpio_int2;
	GPIO_NUM_TYPE gpio_int2_sh;
	uint16_t poll_interval;
	int offset_x;
	int offset_y;
	int offset_z;
	int sensitivity_x;
	int sensitivity_y;
	int sensitivity_z;
	uint8_t device_type;
	uint8_t calibrate_style;
	uint8_t calibrate_way;
	uint16_t x_calibrate_thredhold;
	uint16_t y_calibrate_thredhold;
	uint16_t z_calibrate_thredhold;
	uint8_t wakeup_duration;
	uint8_t g_sensor_extend_data[SENSOR_PLATFORM_EXTEND_DATA_SIZE];
	uint8_t gpio_int2_sh_func;
	uint8_t acc_data_thredhold; /* uint: m/s2 */
};

#define DEF_SENSOR_COM_SETTING \
{\
	.bus_type = TAG_I2C,\
	.bus_num = 0,\
	.disable_sample_thread = 0,\
	{ .data = 0 } \
}

#endif

