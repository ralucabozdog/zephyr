/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Analog Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/util.h>

#include <alif_se.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(soc_usb, CONFIG_SOC_LOG_LEVEL);

/* The USB controller and PHY are held in power-on reset by USB_CTRL2[8] */
#define CLKCTL_PER_MST_ADDR DT_REG_ADDR_BY_NAME(DT_NODELABEL(clockctrl), clkctl_per_mst)
#define CLKCTL_PER_MST_SIZE DT_REG_SIZE_BY_NAME(DT_NODELABEL(clockctrl), clkctl_per_mst)
#define USB_CTRL2_OFFSET    0xACU
#define USB_CTRL2_POR       BIT(8)

/*
 * The USB PHY is power gated by the Secure Enclave. It is powered when the
 * run profile of the requesting core says so.
 */
#define SE_SVC_GET_RUN    310U
#define SE_SVC_SET_RUN    311U
#define SE_RUN_MEMORY     7U
#define SE_RUN_IP_CLOCKS  8U
#define SE_RUN_PHY_GATING 9U
#define SE_PHY_USB        BIT(1)

/* Request and answer of the get and set run profile services */
struct se_run_profile {
	struct alif_se_header hdr;
	/*
	 * Power domains, DCDC voltage and mode, AON and run clock sources,
	 * CPU and scaled clock frequencies, memory blocks, IP clock gating,
	 * PHY power gating and IOFLEX voltage, in this order.
	 */
	uint32_t field[11];
	int32_t error_code;
};

static int se_run_profile_call(struct se_run_profile *msg, uint16_t service)
{
	int ret;

	msg->hdr = (struct alif_se_header){.service_id = service};
	/* The Secure Enclave overwrites the error code when it handles the request */
	msg->error_code = ALIF_SE_NO_ANSWER;

	ret = alif_se_call(msg, sizeof(*msg));
	if (ret != 0) {
		return ret;
	}

	if (msg->error_code == ALIF_SE_NO_ANSWER) {
		LOG_ERR("Service %u not handled by the Secure Enclave", service);
		return -EIO;
	}

	if (msg->error_code != 0) {
		LOG_ERR("Service %u failed (%d)", service, msg->error_code);
		return -EIO;
	}

	return 0;
}

static int alif_usb_phy_power_on(void)
{
	struct se_run_profile msg = {0};
	int ret;

	ret = se_run_profile_call(&msg, SE_SVC_GET_RUN);
	if (ret != 0) {
		LOG_ERR("Failed to get run profile (%d)", ret);
		return ret;
	}

	LOG_DBG("Run profile: power domains 0x%x, memory 0x%x, IP clocks 0x%x, PHYs 0x%x",
		msg.field[0], msg.field[SE_RUN_MEMORY], msg.field[SE_RUN_IP_CLOCKS],
		msg.field[SE_RUN_PHY_GATING]);

	if ((msg.field[SE_RUN_PHY_GATING] & SE_PHY_USB) != 0U) {
		return 0;
	}

	/*
	 * A set bit powers the PHY. Changing any other field in the same
	 * request makes the Secure Enclave ignore the request.
	 */
	msg.field[SE_RUN_PHY_GATING] |= SE_PHY_USB;
	ret = se_run_profile_call(&msg, SE_SVC_SET_RUN);
	if (ret != 0) {
		LOG_ERR("Failed to power the USB PHY (%d)", ret);
	}

	return ret;
}

static int alif_usb_phy_init(void)
{
	mm_reg_t base;
	int ret;

	ret = alif_usb_phy_power_on();

	/* Take the controller and PHY out of reset once the PHY is powered */
	device_map(&base, CLKCTL_PER_MST_ADDR, CLKCTL_PER_MST_SIZE, K_MEM_CACHE_NONE);
	sys_clear_bits(base + USB_CTRL2_OFFSET, USB_CTRL2_POR);
	device_unmap(base, CLKCTL_PER_MST_SIZE);

	return ret;
}

/* The USB stack initializes the controller later, when it is enabled */
SYS_INIT(alif_usb_phy_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
