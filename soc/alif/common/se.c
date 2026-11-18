/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Analog Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/util.h>

#include <alif_se.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(soc_se, CONFIG_SOC_LOG_LEVEL);

/*
 * A core requests Secure Enclave services through the MHUv2 pair dedicated
 * to it. A request carries the bus address of a message buffer in channel 0,
 * and the Secure Enclave signals the answer on the receiver.
 */
#define SE_MHU_SEND_ADDR 0x40050000UL
#define SE_MHU_RECV_ADDR 0x40040000UL
#define SE_MHU_SIZE      0x1000UL

#define MHU_CH_ST           0x000U
#define MHU_SEND_CH_SET     0x00CU
#define MHU_SEND_CH_INT_CLR 0x014U
#define MHU_SEND_CH_INT_EN  0x018U
#define MHU_RECV_CH_CLR     0x008U
#define MHU_SEND_ACCESS_REQ 0xF88U
#define MHU_SEND_ACCESS_RDY 0xF8CU
#define MHU_SEND_INT_CLR    0xF94U
#define MHU_SEND_INT_EN     0xF98U
#define MHU_SEND_INT_ALL    GENMASK(2, 0)

#define SE_SVC_HEARTBEAT        0U
#define SE_HEARTBEAT_TRIES      20U
#define SE_HEARTBEAT_TIMEOUT_US 50000U
#define SE_SERVICE_TIMEOUT_US   1000000U
#define SE_SERVICE_TRIES        3U
#define SE_MSG_SIZE_MAX         256U

#define SE_BUF_ALIGN COND_CODE_1(CONFIG_DCACHE, (CONFIG_DCACHE_LINE_SIZE), (sizeof(uint32_t)))

static union {
	struct alif_se_header hdr;
	uint8_t raw[ROUND_UP(SE_MSG_SIZE_MAX, SE_BUF_ALIGN)];
} se_buf __aligned(SE_BUF_ALIGN);

static K_MUTEX_DEFINE(se_lock);
static mm_reg_t se_send;
static mm_reg_t se_recv;
static bool se_ready;

struct soc_dma_range {
	uint32_t cpu_addr;
	uint32_t bus_addr;
	uint32_t length;
};

#define SOC_DMA_RANGE(node_id, idx)                                                                \
	{                                                                                          \
		.cpu_addr = DT_DMA_RANGES_PARENT_BUS_ADDRESS_BY_IDX(node_id, idx),                 \
		.bus_addr = DT_DMA_RANGES_CHILD_BUS_ADDRESS_BY_IDX(node_id, idx),                  \
		.length = DT_DMA_RANGES_LENGTH_BY_IDX(node_id, idx),                               \
	},

static const struct soc_dma_range soc_dma_ranges[] = {
	DT_FOREACH_DMA_RANGE(DT_NODELABEL(soc), SOC_DMA_RANGE)
};

/* The Secure Enclave reaches the TCM of this core through the global alias */
static uint32_t se_bus_addr(const void *ptr)
{
	uint32_t addr = (uint32_t)(uintptr_t)ptr;

	for (size_t i = 0; i < ARRAY_SIZE(soc_dma_ranges); i++) {
		const struct soc_dma_range *r = &soc_dma_ranges[i];

		if ((addr >= r->cpu_addr) && ((addr - r->cpu_addr) < r->length)) {
			return addr - r->cpu_addr + r->bus_addr;
		}
	}

	return addr;
}

static bool se_wait(mm_reg_t reg, uint32_t mask, bool set, uint32_t timeout_us)
{
	for (uint32_t i = 0; i < timeout_us; i++) {
		if (((sys_read32(reg) & mask) != 0U) == set) {
			return true;
		}

		k_busy_wait(1);
	}

	return false;
}

static int se_transfer(size_t size, uint32_t timeout_us)
{
	mm_reg_t send = se_send;
	mm_reg_t recv = se_recv;
	int ret = 0;

	/* A late answer to an earlier request must not complete this one */
	if (sys_read32(recv + MHU_CH_ST) != 0U) {
		sys_write32(UINT32_MAX, recv + MHU_RECV_CH_CLR);
	}

	se_buf.hdr.error_code = ALIF_SE_NO_ANSWER;
	sys_cache_data_flush_range(&se_buf, size);

	sys_clear_bits(send + MHU_SEND_INT_EN, MHU_SEND_INT_ALL);
	sys_write32(MHU_SEND_INT_ALL, send + MHU_SEND_INT_CLR);

	sys_write32(1U, send + MHU_SEND_ACCESS_REQ);
	if (!se_wait(send + MHU_SEND_ACCESS_RDY, BIT(0), true, timeout_us)) {
		sys_write32(0U, send + MHU_SEND_ACCESS_REQ);
		return -EBUSY;
	}

	if (sys_read32(send + MHU_CH_ST) != 0U) {
		sys_write32(0U, send + MHU_SEND_ACCESS_REQ);
		return -EBUSY;
	}

	sys_write32(1U, send + MHU_SEND_CH_INT_CLR);
	sys_clear_bits(send + MHU_SEND_CH_INT_EN, BIT(0));
	sys_write32(se_bus_addr(&se_buf), send + MHU_SEND_CH_SET);

	/* The Secure Enclave clears the channel when it takes the request */
	if (!se_wait(send + MHU_CH_ST, UINT32_MAX, false, timeout_us)) {
		ret = -ETIMEDOUT;
	}

	sys_write32(1U, send + MHU_SEND_CH_INT_CLR);
	sys_write32(MHU_SEND_INT_ALL, send + MHU_SEND_INT_CLR);
	sys_write32(0U, send + MHU_SEND_ACCESS_REQ);

	if (ret != 0) {
		return ret;
	}

	if (!se_wait(recv + MHU_CH_ST, UINT32_MAX, true, timeout_us)) {
		return -ETIMEDOUT;
	}

	sys_write32(UINT32_MAX, recv + MHU_RECV_CH_CLR);

	/* The answer is complete once the Secure Enclave wrote the transport error code */
	for (uint32_t i = 0; i < timeout_us; i++) {
		sys_cache_data_invd_range(&se_buf, size);
		if (se_buf.hdr.error_code != ALIF_SE_NO_ANSWER) {
			return 0;
		}

		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

/* Heartbeats wake the Secure Enclave up until it answers */
static int se_wake(void)
{
	int ret = -ETIMEDOUT;

	device_map(&se_send, SE_MHU_SEND_ADDR, SE_MHU_SIZE, K_MEM_CACHE_NONE);
	device_map(&se_recv, SE_MHU_RECV_ADDR, SE_MHU_SIZE, K_MEM_CACHE_NONE);

	for (uint32_t i = 0; i < SE_HEARTBEAT_TRIES && ret != 0; i++) {
		memset(&se_buf, 0, sizeof(se_buf));
		se_buf.hdr.service_id = SE_SVC_HEARTBEAT;
		ret = se_transfer(sizeof(se_buf.hdr), SE_HEARTBEAT_TIMEOUT_US);
	}

	if (ret != 0) {
		LOG_ERR("Secure Enclave does not respond (%d)", ret);
	}

	return ret;
}

int alif_se_call(void *msg, size_t size)
{
	struct alif_se_header *hdr = msg;
	int ret = 0;

	if (size > SE_MSG_SIZE_MAX) {
		return -EINVAL;
	}

	k_mutex_lock(&se_lock, K_FOREVER);

	if (!se_ready) {
		ret = se_wake();
		se_ready = (ret == 0);
	}

	/* The Secure Enclave can be busy for a while after a reset of the core */
	for (uint32_t i = 0; i < SE_SERVICE_TRIES && se_ready; i++) {
		memcpy(&se_buf, msg, size);
		ret = se_transfer(size, SE_SERVICE_TIMEOUT_US);
		if (ret != -ETIMEDOUT) {
			break;
		}
	}

	if (ret == 0) {
		memcpy(msg, &se_buf, size);
	}

	k_mutex_unlock(&se_lock);

	if (ret != 0) {
		LOG_ERR("Service %u: transfer failed (%d)", hdr->service_id, ret);
	} else if (hdr->error_code != 0U) {
		LOG_ERR("Service %u: transport error %u", hdr->service_id, hdr->error_code);
		ret = -EIO;
	}

	return ret;
}
