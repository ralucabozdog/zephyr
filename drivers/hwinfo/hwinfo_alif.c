/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Analog Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include <alif_se.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(hwinfo_alif, CONFIG_HWINFO_LOG_LEVEL);

#define SE_SVC_GET_DEVICE_DATA 208U

/* Answer of the get device revision data service */
struct se_device_data {
	struct alif_se_header hdr;
	uint32_t revision_id;
	uint8_t version[4];
	uint8_t part_number[16];
	uint8_t hbk0[16];
	uint8_t hbk1[16];
	uint8_t hbk_fw[20];
	uint8_t config[4];
	uint8_t dcu[16];
	uint8_t mfg_data[32];
	uint8_t serial_number[8];
	uint8_t lcs;
	uint8_t padding[3];
	uint32_t external_config[4];
	uint32_t flags2;
	int32_t error_code;
};

static uint8_t serial_number[8];
static int serial_number_err = -ENODATA;

/*
 * Secure Enclave calls busy-wait for the answer. The serial number is read
 * once at boot, so that callers such as the USB stack never wait for it.
 */
static int alif_hwinfo_init(void)
{
	static struct se_device_data msg;
	bool programmed = false;
	int ret;

	msg.hdr.service_id = SE_SVC_GET_DEVICE_DATA;
	/* The Secure Enclave overwrites the error code when it handles the request */
	msg.error_code = ALIF_SE_NO_ANSWER;

	ret = alif_se_call(&msg, sizeof(msg));
	if (ret == 0 && msg.error_code != 0) {
		LOG_ERR("Device data service failed (%d)", msg.error_code);
		ret = -EIO;
	}

	if (ret != 0) {
		LOG_ERR("Failed to read the serial number (%d)", ret);
		serial_number_err = ret;
		return 0;
	}

	for (size_t i = 0; i < sizeof(msg.serial_number); i++) {
		programmed = programmed || (msg.serial_number[i] != 0U);
	}

	/* Parts without a programmed serial number report zeros, which is not unique */
	if (!programmed) {
		LOG_INF("No serial number programmed");
		serial_number_err = -ENODATA;
		return 0;
	}

	memcpy(serial_number, msg.serial_number, sizeof(serial_number));
	serial_number_err = 0;
	LOG_DBG("Serial number %02x%02x%02x%02x%02x%02x%02x%02x", serial_number[0],
		serial_number[1], serial_number[2], serial_number[3], serial_number[4],
		serial_number[5], serial_number[6], serial_number[7]);

	return 0;
}

ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	if (serial_number_err != 0) {
		return serial_number_err;
	}

	length = MIN(length, sizeof(serial_number));
	memcpy(buffer, serial_number, length);

	return length;
}

SYS_INIT(alif_hwinfo_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
