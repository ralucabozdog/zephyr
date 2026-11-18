/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Analog Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <cmsis_core.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#define DTCM_ADDR    DT_REG_ADDR(DT_NODELABEL(dtcm))
#define DTCM_NS_ADDR DT_REG_ADDR(DT_NODELABEL(dtcm_ns))
#define DTCM_NS_SIZE DT_REG_SIZE(DT_NODELABEL(dtcm_ns))

/* CMSIS defines the SAU only for code built with CMSE, which Zephyr is not */
#define SAU_BASE_ADDR   0xE000EDD0UL
#define SAU_CTRL_OFFSET 0x00U
#define SAU_RNR_OFFSET  0x08U
#define SAU_RBAR_OFFSET 0x0CU
#define SAU_RLAR_OFFSET 0x10U
#define SAU_CTRL_ENABLE BIT(0)
#define SAU_ADDR_MASK   GENMASK(31, 5)
#define SAU_RLAR_ENABLE BIT(0)

/*
 * Bus masters other than the cores issue non-secure transactions, which the
 * DTCM gate unit rejects in blocks marked secure, as all are out of reset.
 * Mark the window non-secure both there and in the SAU, so that the core
 * reaches the same memory with matching non-secure transactions. Nothing
 * uses the window before this runs.
 */
static int alif_dtcm_ns_init(void)
{
	uint32_t blksz = BIT(FIELD_GET(MEMSYSCTL_DTGU_CFG_BLKSZ_Msk, MEMSYSCTL->DTGU_CFG) + 5U);
	uint32_t first = (DTCM_NS_ADDR - DTCM_ADDR) / blksz;
	uint32_t last = (DTCM_NS_ADDR + DTCM_NS_SIZE - 1U - DTCM_ADDR) / blksz;

	/* The window must cover whole gate unit blocks */
	if (!IS_ALIGNED(DTCM_NS_ADDR - DTCM_ADDR, blksz) || !IS_ALIGNED(DTCM_NS_SIZE, blksz)) {
		return -EINVAL;
	}

	for (uint32_t blk = first; blk <= last; blk++) {
		MEMSYSCTL->DTGU_LUT[blk / 32U] |= BIT(blk % 32U);
	}

	sys_write32(0U, SAU_BASE_ADDR + SAU_RNR_OFFSET);
	sys_write32(DTCM_NS_ADDR & SAU_ADDR_MASK, SAU_BASE_ADDR + SAU_RBAR_OFFSET);
	sys_write32(((DTCM_NS_ADDR + DTCM_NS_SIZE - 1U) & SAU_ADDR_MASK) | SAU_RLAR_ENABLE,
		    SAU_BASE_ADDR + SAU_RLAR_OFFSET);
	sys_write32(SAU_CTRL_ENABLE, SAU_BASE_ADDR + SAU_CTRL_OFFSET);

	barrier_dsync_fence_full();
	barrier_isync_fence_full();

	return 0;
}

SYS_INIT(alif_dtcm_ns_init, PRE_KERNEL_1, 0);
