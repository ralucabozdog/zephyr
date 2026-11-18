/*
 * Copyright (c) 2025 Alif Semiconductor
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT snps_dwc3

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/linker/devicetree_regions.h>

#include <usb_dwc3_hw.h>

#include "udc_common.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_dwc3, CONFIG_UDC_DRIVER_LOG_LEVEL);

/*
 * Only device events and transfer completions are enabled, no SOF events, so
 * a small event buffer is enough. The controller requires the buffer address
 * to be aligned to its size.
 */
#define DWC3_EVT_BUF_SIZE	1024U
/* A data TRB and an optional zero-length TRB per transfer */
#define DWC3_TRBS_PER_EP	2U
#define DWC3_TRB_ALIGN		MAX(16U, USB_BUF_ALIGN)
#define DWC3_SETUP_BUF_SIZE	USB_BUF_ROUND_UP(8U)
#define DWC3_MPS0		64U
#define DWC3_DEPCMD_TIMEOUT_US	1000U
#define DWC3_RESET_TIMEOUT_US	10000U
#define DWC3_HALT_TIMEOUT_US	10000U
#define DWC3_WAKEUP_TIMEOUT_US	20000U
/* Called with the scheduler locked, so the resets are busy waits */
#define DWC3_PHY_RESET_US	50000U
#define DWC3_SOFT_RESET_SETTLE_US 5000U

enum dwc3_ep0_state {
	DWC3_EP0_SETUP,
	DWC3_EP0_DATA,
	DWC3_EP0_STATUS,
};

/* Physical endpoint state, indexed by the physical endpoint number */
struct udc_dwc3_ep {
	/* Transfer resource index assigned by the Start Transfer command */
	uint8_t rsc_idx;
	/* Transfer is started on the endpoint */
	bool started;
	/* Transfer ends with the zero-length TRB */
	bool zlp;
	/* Length programmed in the data TRB */
	uint32_t length;
};

struct udc_dwc3_dma_range {
	uintptr_t cpu_addr;
	uint32_t bus_addr;
	size_t length;
};

struct udc_dwc3_config {
	mm_reg_t base;
	size_t num_in_eps;
	size_t num_out_eps;
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	struct udc_dwc3_ep *eps;
	struct usb_dwc3_trb (*trbs)[DWC3_TRBS_PER_EP];
	uint8_t *evt_buf;
	uint8_t *evt_cache;
	uint8_t *setup_buf;
	const struct udc_dwc3_dma_range *dma_ranges;
	size_t num_dma_ranges;
	const struct device *clock_dev;
	const clock_control_subsys_t *clock_subsys;
	size_t num_clocks;
	void (*irq_enable_func)(const struct device *dev);
	void (*irq_disable_func)(const struct device *dev);
	void (*make_thread)(const struct device *dev);
	uint8_t num_phy_eps;
	uint8_t utmi_width;
	bool hs;
};

struct udc_dwc3_data {
	struct k_thread thread_data;
	struct k_sem irq_sem;
	const struct device *dev;
	enum udc_bus_speed speed;
	/* Next event to process in the event cache */
	uint32_t evt_pos;
	/* Next event the interrupt handler copies to the event cache */
	uint32_t evt_isr_pos;
	/* Bytes of events copied but not processed yet */
	uint32_t evt_pending;
	enum dwc3_ep0_state ep0_state;
	/* Physical endpoint that reported XferNotReady for the Status stage */
	uint8_t status_phy;
	bool setup_armed;
	bool data_armed;
	bool status_armed;
	bool status_nrdy;
	bool three_stage;
	bool expect_in;
};

static inline uint32_t dwc3_read(const struct device *dev, const uint32_t off)
{
	const struct udc_dwc3_config *const config = dev->config;

	return sys_read32(config->base + off);
}

static inline void dwc3_write(const struct device *dev, const uint32_t off,
			      const uint32_t val)
{
	const struct udc_dwc3_config *const config = dev->config;

	sys_write32(val, config->base + off);
}

static inline void dwc3_modify(const struct device *dev, const uint32_t off,
			       const uint32_t clear, const uint32_t set)
{
	dwc3_write(dev, off, (dwc3_read(dev, off) & ~clear) | set);
}

static inline uint8_t dwc3_ep_to_phy(const uint8_t ep)
{
	return (USB_EP_GET_IDX(ep) << 1) | (USB_EP_DIR_IS_IN(ep) ? 1U : 0U);
}

static inline uint8_t dwc3_phy_to_ep(const uint8_t phy)
{
	return (phy >> 1) | ((phy & 1U) != 0U ? USB_EP_DIR_IN : USB_EP_DIR_OUT);
}

/* Translate a CPU address to the address the controller uses on its bus */
static uint32_t dwc3_dma_addr(const struct device *dev, const void *const ptr)
{
	const struct udc_dwc3_config *const config = dev->config;
	uintptr_t addr = (uintptr_t)ptr;

	for (size_t i = 0; i < config->num_dma_ranges; i++) {
		const struct udc_dwc3_dma_range *r = &config->dma_ranges[i];

		if ((addr >= r->cpu_addr) && ((addr - r->cpu_addr) < r->length)) {
			return r->bus_addr + (uint32_t)(addr - r->cpu_addr);
		}
	}

	return (uint32_t)addr;
}

static int dwc3_depcmd(const struct device *dev, const uint8_t phy,
		       const uint32_t cmd, const uint32_t par0, const uint32_t par1)
{
	uint32_t phycfg = dwc3_read(dev, USB_DWC3_GUSB2PHYCFG0);
	uint32_t lpm = phycfg & (USB_DWC3_GUSB2PHYCFG_SUSPHY | USB_DWC3_GUSB2PHYCFG_ENBLSLPM);
	uint32_t reg;
	int ret = -ETIMEDOUT;

	/* Endpoint commands do not complete while the PHY is suspended */
	if (lpm != 0U) {
		dwc3_write(dev, USB_DWC3_GUSB2PHYCFG0, phycfg & ~lpm);
	}

	dwc3_write(dev, USB_DWC3_DEPCMDPAR2(phy), 0U);
	dwc3_write(dev, USB_DWC3_DEPCMDPAR1(phy), par1);
	dwc3_write(dev, USB_DWC3_DEPCMDPAR0(phy), par0);
	dwc3_write(dev, USB_DWC3_DEPCMD(phy), cmd | USB_DWC3_DEPCMD_CMDACT);

	for (uint32_t i = 0; i < DWC3_DEPCMD_TIMEOUT_US; i++) {
		reg = dwc3_read(dev, USB_DWC3_DEPCMD(phy));
		if ((reg & USB_DWC3_DEPCMD_CMDACT) == 0U) {
			ret = (FIELD_GET(USB_DWC3_DEPCMD_STATUS_MASK, reg) == 0U) ? 0 : -EIO;
			break;
		}

		k_busy_wait(1);
	}

	if (lpm != 0U) {
		dwc3_modify(dev, USB_DWC3_GUSB2PHYCFG0, 0U, lpm);
	}

	if (ret != 0) {
		LOG_ERR("Command 0x%02x on ep %u failed (%d), DEPCMD 0x%08x",
			(uint32_t)(cmd & USB_DWC3_DEPCMD_CMD_MASK), phy, ret, reg);
	}

	return ret;
}

static void dwc3_prep_trb(const struct device *dev, struct usb_dwc3_trb *const trb,
			  const void *const buf, const uint32_t len, const uint32_t type,
			  const uint32_t flags, const bool chain)
{
	trb->bpl = dwc3_dma_addr(dev, buf);
	trb->bph = 0U;
	trb->size = FIELD_PREP(USB_DWC3_TRB_SIZE_LENGTH_MASK, len);
	trb->ctrl = FIELD_PREP(USB_DWC3_TRB_CTRL_TRBCTL_MASK, type) | flags |
		    USB_DWC3_TRB_CTRL_HWO;

	if (chain) {
		trb->ctrl |= USB_DWC3_TRB_CTRL_CHN;
	} else {
		trb->ctrl |= USB_DWC3_TRB_CTRL_LST | USB_DWC3_TRB_CTRL_IOC;
	}
}

static int dwc3_start_xfer(const struct device *dev, const uint8_t phy)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_ep *const ep = &config->eps[phy];
	struct usb_dwc3_trb *const trb = config->trbs[phy];
	uint32_t reg;
	int ret;

	sys_cache_data_flush_range(trb, sizeof(config->trbs[phy]));

	ret = dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_STARTTRANSFER, 0U, dwc3_dma_addr(dev, trb));
	if (ret != 0) {
		return ret;
	}

	reg = dwc3_read(dev, USB_DWC3_DEPCMD(phy));
	ep->rsc_idx = FIELD_GET(USB_DWC3_DEPCMD_RSC_IDX_MASK, reg);
	ep->started = true;

	return 0;
}

static void dwc3_end_xfer(const struct device *dev, const uint8_t phy)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_ep *const ep = &config->eps[phy];
	uint32_t cmd;

	if (!ep->started) {
		return;
	}

	cmd = USB_DWC3_DEPCMD_ENDTRANSFER | USB_DWC3_DEPCMD_CMDIOC |
	      USB_DWC3_DEPCMD_HIPRI_FORCERM |
	      FIELD_PREP(USB_DWC3_DEPCMD_PARAM_MASK, ep->rsc_idx);
	(void)dwc3_depcmd(dev, phy, cmd, 0U, 0U);

	/* The controller needs time to retire the transfer on the bus */
	k_busy_wait(100);
	ep->started = false;
}

/* Bytes transferred by a completed transfer */
static uint32_t dwc3_xfer_length(const struct device *dev, const uint8_t phy)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct usb_dwc3_trb *const trb = &config->trbs[phy][0];
	uint32_t remaining = FIELD_GET(USB_DWC3_TRB_SIZE_LENGTH_MASK, trb->size);

	return config->eps[phy].length - remaining;
}

static void dwc3_ep0_setup_start(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	priv->ep0_state = DWC3_EP0_SETUP;
	priv->data_armed = false;
	priv->status_armed = false;
	priv->status_nrdy = false;

	if (priv->setup_armed) {
		return;
	}

	sys_cache_data_invd_range(config->setup_buf, DWC3_SETUP_BUF_SIZE);
	dwc3_prep_trb(dev, &config->trbs[0][0], config->setup_buf, 8U,
		      USB_DWC3_TRBCTL_CONTROL_SETUP, USB_DWC3_TRB_CTRL_ISP_IMI, false);
	config->eps[0].length = 8U;

	if (dwc3_start_xfer(dev, 0U) == 0) {
		priv->setup_armed = true;
	}
}

static void dwc3_ep0_stall_restart(const struct device *dev)
{
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	dwc3_end_xfer(dev, 0U);
	dwc3_end_xfer(dev, 1U);
	priv->setup_armed = false;

	/* A control endpoint is always stalled on physical endpoint 0 */
	(void)dwc3_depcmd(dev, 0U, USB_DWC3_DEPCMD_SETSTALL, 0U, 0U);
	dwc3_ep0_setup_start(dev);
}

static void dwc3_ep0_arm_data(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	const uint8_t phy = priv->expect_in ? 1U : 0U;
	struct usb_dwc3_trb *const trb = config->trbs[phy];
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, dwc3_phy_to_ep(phy));
	struct net_buf *buf = udc_buf_peek(cfg);
	uint32_t len;
	bool zlp = false;

	if (buf == NULL || !udc_get_buf_info(buf)->data) {
		return;
	}

	if (priv->expect_in) {
		len = buf->len;
		zlp = udc_ep_buf_has_zlp(buf) && len != 0U && (len % DWC3_MPS0) == 0U;
		sys_cache_data_flush_range(buf->data, len);
	} else {
		/* The USB stack rounds control OUT buffers up to bMaxPacketSize0 */
		len = buf->size;
		sys_cache_data_invd_range(buf->data, len);
	}

	dwc3_prep_trb(dev, &trb[0], buf->data, len, USB_DWC3_TRBCTL_CONTROL_DATA,
		      USB_DWC3_TRB_CTRL_ISP_IMI, zlp);
	if (zlp) {
		dwc3_prep_trb(dev, &trb[1], config->setup_buf, 0U,
			      USB_DWC3_TRBCTL_CONTROL_DATA, USB_DWC3_TRB_CTRL_ISP_IMI, false);
	}

	config->eps[phy].length = len;
	config->eps[phy].zlp = zlp;

	LOG_DBG("ep0 data %s len %u%s", priv->expect_in ? "in" : "out", len, zlp ? " zlp" : "");
	if (dwc3_start_xfer(dev, phy) == 0) {
		priv->data_armed = true;
	}
}

static void dwc3_ep0_arm_status(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	const uint8_t phy = priv->status_phy;
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, dwc3_phy_to_ep(phy));
	struct net_buf *buf = udc_buf_peek(cfg);
	uint32_t type;

	if (buf == NULL || !udc_get_buf_info(buf)->status) {
		return;
	}

	type = priv->three_stage ? USB_DWC3_TRBCTL_CONTROL_STATUS3 :
				   USB_DWC3_TRBCTL_CONTROL_STATUS2;
	dwc3_prep_trb(dev, &config->trbs[phy][0], config->setup_buf, 0U, type,
		      USB_DWC3_TRB_CTRL_ISP_IMI, false);
	config->eps[phy].length = 0U;
	config->eps[phy].zlp = false;

	LOG_DBG("ep0 status on ep %u", phy);
	if (dwc3_start_xfer(dev, phy) == 0) {
		priv->status_armed = true;
	}
}

/* Start the Data or Status stage when both the stack and the host are ready */
static void dwc3_ep0_kick(const struct device *dev)
{
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	if (priv->ep0_state == DWC3_EP0_DATA && !priv->data_armed) {
		dwc3_ep0_arm_data(dev);
	} else if (priv->ep0_state == DWC3_EP0_STATUS && priv->status_nrdy &&
		   !priv->status_armed) {
		dwc3_ep0_arm_status(dev);
	}
}

static void dwc3_ep0_setup_done(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	struct usb_setup_packet setup;

	priv->setup_armed = false;
	sys_cache_data_invd_range(config->setup_buf, DWC3_SETUP_BUF_SIZE);
	memcpy(&setup, config->setup_buf, sizeof(setup));
	LOG_HEXDUMP_DBG(&setup, sizeof(setup), "setup");

	priv->three_stage = sys_le16_to_cpu(setup.wLength) != 0U;
	priv->expect_in = priv->three_stage && usb_reqtype_is_to_host(&setup);
	priv->ep0_state = priv->three_stage ? DWC3_EP0_DATA : DWC3_EP0_STATUS;
	priv->data_armed = false;
	priv->status_armed = false;
	priv->status_nrdy = false;

	udc_setup_received(dev, &setup);
}

static void dwc3_ep0_xfer_complete(const struct device *dev, const uint8_t phy)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	struct usb_dwc3_trb *const trb = config->trbs[phy];
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, dwc3_phy_to_ep(phy));
	struct net_buf *buf;
	uint32_t trbsts;

	if (!config->eps[phy].started) {
		return;
	}

	sys_cache_data_invd_range(trb, sizeof(config->trbs[phy]));
	config->eps[phy].started = false;
	trbsts = FIELD_GET(USB_DWC3_TRB_SIZE_TRBSTS_MASK, trb[0].size);

	switch (priv->ep0_state) {
	case DWC3_EP0_SETUP:
		if (phy == 0U && priv->setup_armed) {
			dwc3_ep0_setup_done(dev);
		}
		break;
	case DWC3_EP0_DATA:
		if (!priv->data_armed || phy != (priv->expect_in ? 1U : 0U)) {
			break;
		}

		priv->data_armed = false;
		if (trbsts == USB_DWC3_TRBSTS_SETUP_PENDING) {
			/* New SETUP cancels this transfer, see udc_setup_received() */
			dwc3_ep0_setup_start(dev);
			break;
		}

		buf = udc_buf_get(cfg);
		if (buf == NULL) {
			LOG_ERR("No buffer for ep 0x%02x data stage", cfg->addr);
			break;
		}

		if (USB_EP_DIR_IS_OUT(cfg->addr)) {
			uint32_t len = dwc3_xfer_length(dev, phy);

			sys_cache_data_invd_range(buf->data, len);
			net_buf_add(buf, len);
		}

		udc_submit_ep_event(dev, buf, 0);
		priv->ep0_state = DWC3_EP0_STATUS;
		dwc3_ep0_kick(dev);
		break;
	case DWC3_EP0_STATUS:
		if (!priv->status_armed || phy != priv->status_phy) {
			break;
		}

		priv->status_armed = false;
		if (trbsts != USB_DWC3_TRBSTS_SETUP_PENDING) {
			buf = udc_buf_get(cfg);
			if (buf != NULL) {
				udc_submit_ep_event(dev, buf, 0);
			}
		}

		dwc3_ep0_setup_start(dev);
		break;
	}
}

static void dwc3_ep0_xfer_not_ready(const struct device *dev, const uint8_t phy,
				    const uint32_t status)
{
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	switch (status & USB_DWC3_DEPEVT_STATUS_CONTROL_MASK) {
	case USB_DWC3_DEPEVT_STATUS_CONTROL_DATA:
		if (priv->ep0_state == DWC3_EP0_DATA && phy != (priv->expect_in ? 1U : 0U)) {
			LOG_WRN("Host requested data stage in wrong direction");
			dwc3_ep0_stall_restart(dev);
		}
		break;
	case USB_DWC3_DEPEVT_STATUS_CONTROL_STATUS:
		priv->status_nrdy = true;
		priv->status_phy = phy;
		dwc3_ep0_kick(dev);
		break;
	default:
		break;
	}
}

static int dwc3_ep_start_next(const struct device *dev, struct udc_ep_config *const cfg)
{
	const struct udc_dwc3_config *const config = dev->config;
	const uint8_t phy = dwc3_ep_to_phy(cfg->addr);
	struct udc_dwc3_ep *const ep = &config->eps[phy];
	struct usb_dwc3_trb *const trb = config->trbs[phy];
	const uint16_t mps = udc_mps_ep_size(cfg);
	struct net_buf *buf;
	uint32_t len;
	bool zlp = false;
	int ret;

	if (ep->started || cfg->stat.halted) {
		return 0;
	}

	buf = udc_buf_peek(cfg);
	if (buf == NULL) {
		return 0;
	}

	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		len = buf->len;
		zlp = udc_ep_buf_has_zlp(buf) && len != 0U && (len % mps) == 0U;
		sys_cache_data_flush_range(buf->data, len);
		dwc3_prep_trb(dev, &trb[0], buf->data, len, USB_DWC3_TRBCTL_NORMAL, 0U, zlp);
		if (zlp) {
			dwc3_prep_trb(dev, &trb[1], buf->data, 0U, USB_DWC3_TRBCTL_NORMAL,
				      0U, false);
		}
	} else {
		/* An OUT TRB must be a multiple of the maximum packet size */
		len = ROUND_DOWN(buf->size, mps);
		if (len == 0U) {
			LOG_ERR("ep 0x%02x buffer smaller than MPS", cfg->addr);
			udc_submit_ep_event(dev, udc_buf_get(cfg), -EINVAL);
			return -EINVAL;
		}

		sys_cache_data_invd_range(buf->data, len);
		dwc3_prep_trb(dev, &trb[0], buf->data, len, USB_DWC3_TRBCTL_NORMAL,
			      USB_DWC3_TRB_CTRL_ISP_IMI, false);
	}

	ep->length = len;
	ep->zlp = zlp;

	ret = dwc3_start_xfer(dev, phy);
	if (ret == 0) {
		udc_ep_set_busy(cfg, true);
	}

	return ret;
}

static void dwc3_ep_xfer_complete(const struct device *dev, const uint8_t phy)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_ep *const ep = &config->eps[phy];
	struct usb_dwc3_trb *const trb = config->trbs[phy];
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, dwc3_phy_to_ep(phy));
	struct net_buf *buf;

	if (cfg == NULL || !ep->started) {
		return;
	}

	sys_cache_data_invd_range(trb, sizeof(config->trbs[phy]));
	if ((trb[ep->zlp ? 1 : 0].ctrl & USB_DWC3_TRB_CTRL_HWO) != 0U) {
		/* Stale event of a transfer that was ended */
		return;
	}

	ep->started = false;
	udc_ep_set_busy(cfg, false);

	buf = udc_buf_get(cfg);
	if (buf == NULL) {
		LOG_ERR("No buffer for ep 0x%02x", cfg->addr);
		return;
	}

	if (USB_EP_DIR_IS_OUT(cfg->addr)) {
		uint32_t len = dwc3_xfer_length(dev, phy);

		sys_cache_data_invd_range(buf->data, len);
		net_buf_add(buf, len);
	}

	udc_submit_ep_event(dev, buf, 0);
	(void)dwc3_ep_start_next(dev, cfg);
}

static void dwc3_handle_depevt(const struct device *dev, const uint32_t evt)
{
	const struct udc_dwc3_config *const config = dev->config;
	const uint8_t phy = FIELD_GET(USB_DWC3_DEPEVT_EP_NUM_MASK, evt);
	const uint32_t type = FIELD_GET(USB_DWC3_DEPEVT_TYPE_MASK, evt);
	const uint32_t status = FIELD_GET(USB_DWC3_DEPEVT_STATUS_MASK, evt);

	if (phy >= config->num_phy_eps) {
		return;
	}

	if (phy < 2U) {
		if (type == USB_DWC3_DEPEVT_XFERCOMPLETE) {
			dwc3_ep0_xfer_complete(dev, phy);
		} else if (type == USB_DWC3_DEPEVT_XFERNOTREADY) {
			dwc3_ep0_xfer_not_ready(dev, phy, status);
		}

		return;
	}

	if (type == USB_DWC3_DEPEVT_XFERCOMPLETE) {
		dwc3_ep_xfer_complete(dev, phy);
	}
}

static int dwc3_ep0_config(const struct device *dev, const uint8_t phy, const uint32_t action)
{
	uint32_t par0 = FIELD_PREP(USB_DWC3_DEPCFG_EP_TYPE_MASK, USB_EP_TYPE_CONTROL) |
			FIELD_PREP(USB_DWC3_DEPCFG_MAX_PACKET_SIZE_MASK, DWC3_MPS0) |
			FIELD_PREP(USB_DWC3_DEPCFG_ACTION_MASK, action);
	uint32_t par1 = FIELD_PREP(USB_DWC3_DEPCFG_EP_NUMBER_MASK, phy) |
			USB_DWC3_DEPCFG_XFER_COMPLETE_EN | USB_DWC3_DEPCFG_XFER_NOT_READY_EN;

	return dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_SETEPCONFIG, par0, par1);
}

static void dwc3_handle_reset(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	dwc3_modify(dev, USB_DWC3_DCTL, USB_DWC3_DCTL_TSTCTRL_MASK, 0U);

	for (uint8_t phy = 2U; phy < config->num_phy_eps; phy++) {
		struct udc_ep_config *cfg = udc_get_ep_cfg(dev, dwc3_phy_to_ep(phy));

		if (cfg == NULL || !cfg->stat.enabled) {
			continue;
		}

		dwc3_end_xfer(dev, phy);
		udc_ep_set_busy(cfg, false);
		if (cfg->stat.halted) {
			(void)dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_CLEARSTALL, 0U, 0U);
			cfg->stat.halted = false;
		}
	}

	if (priv->ep0_state != DWC3_EP0_SETUP || !priv->setup_armed) {
		dwc3_end_xfer(dev, 0U);
		dwc3_end_xfer(dev, 1U);
		priv->setup_armed = false;
		dwc3_ep0_setup_start(dev);
	}

	dwc3_modify(dev, USB_DWC3_DCFG, USB_DWC3_DCFG_DEVADDR_MASK, 0U);
	udc_set_suspended(dev, false);
}

static void dwc3_handle_devt(const struct device *dev, const uint32_t evt)
{
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	uint32_t link;

	switch (FIELD_GET(USB_DWC3_DEVT_TYPE_MASK, evt)) {
	case USB_DWC3_DEVT_RESET:
		LOG_DBG("Bus reset");
		dwc3_handle_reset(dev);
		udc_submit_event(dev, UDC_EVT_RESET, 0);
		break;
	case USB_DWC3_DEVT_CONNECT_DONE:
		if (FIELD_GET(USB_DWC3_DSTS_CONNECTSPD_MASK, dwc3_read(dev, USB_DWC3_DSTS)) ==
		    USB_DWC3_DSTS_CONNECTSPD_HS) {
			priv->speed = UDC_BUS_SPEED_HS;
		} else {
			priv->speed = UDC_BUS_SPEED_FS;
		}

		LOG_DBG("Connected at %s speed", priv->speed == UDC_BUS_SPEED_HS ? "high" : "full");

		/* The control endpoint is reconfigured for the negotiated speed */
		(void)dwc3_ep0_config(dev, 0U, USB_DWC3_DEPCFG_ACTION_MODIFY);
		(void)dwc3_ep0_config(dev, 1U, USB_DWC3_DEPCFG_ACTION_MODIFY);
		break;
	case USB_DWC3_DEVT_LINK_STATUS_CHANGE:
		link = FIELD_GET(USB_DWC3_DEVT_LINK_STATE_MASK, evt);
		if (link == USB_DWC3_LINK_STATE_SUSPEND && !udc_is_suspended(dev)) {
			udc_set_suspended(dev, true);
			udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
		} else if (link == USB_DWC3_LINK_STATE_ON && udc_is_suspended(dev)) {
			udc_set_suspended(dev, false);
			udc_submit_event(dev, UDC_EVT_RESUME, 0);
		}
		break;
	case USB_DWC3_DEVT_WAKEUP:
		if (udc_is_suspended(dev)) {
			udc_set_suspended(dev, false);
			udc_submit_event(dev, UDC_EVT_RESUME, 0);
		}
		break;
	case USB_DWC3_DEVT_DISCONNECT:
		LOG_DBG("Disconnected");
		break;
	default:
		break;
	}
}

/* Process the events the interrupt handler copied, then accept new ones */
static void dwc3_handle_events(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	for (; priv->evt_pending > 0U; priv->evt_pending -= sizeof(uint32_t)) {
		uint32_t evt = *(uint32_t *)&config->evt_cache[priv->evt_pos];

		priv->evt_pos = (priv->evt_pos + sizeof(uint32_t)) % DWC3_EVT_BUF_SIZE;

		if ((evt & USB_DWC3_EVT_IS_DEVICE) != 0U) {
			dwc3_handle_devt(dev, evt);
		} else {
			dwc3_handle_depevt(dev, evt);
		}
	}

	dwc3_write(dev, USB_DWC3_GEVNTSIZ0, FIELD_PREP(USB_DWC3_GEVNTSIZ_SIZE_MASK,
						       DWC3_EVT_BUF_SIZE));
}

static void udc_dwc3_thread_handler(const struct device *dev)
{
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	while (true) {
		k_sem_take(&priv->irq_sem, K_FOREVER);

		udc_lock_internal(dev, K_FOREVER);
		dwc3_handle_events(dev);
		udc_unlock_internal(dev);
	}
}

static void udc_dwc3_isr_handler(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	uint32_t count;

	count = dwc3_read(dev, USB_DWC3_GEVNTCOUNT0) & USB_DWC3_GEVNTCOUNT_MASK;
	if (count == 0U) {
		return;
	}

	dwc3_write(dev, USB_DWC3_GEVNTSIZ0, USB_DWC3_GEVNTSIZ_INTMASK |
		   FIELD_PREP(USB_DWC3_GEVNTSIZ_SIZE_MASK, DWC3_EVT_BUF_SIZE));

	/*
	 * Copy the events and hand the controller its buffer back, the
	 * acknowledged count is what deasserts the interrupt.
	 */
	sys_cache_data_invd_range(config->evt_buf, DWC3_EVT_BUF_SIZE);
	for (uint32_t i = 0; i < count; i += sizeof(uint32_t)) {
		uint32_t pos = (priv->evt_isr_pos + i) % DWC3_EVT_BUF_SIZE;

		*(uint32_t *)&config->evt_cache[pos] = *(uint32_t *)&config->evt_buf[pos];
	}

	priv->evt_isr_pos = (priv->evt_isr_pos + count) % DWC3_EVT_BUF_SIZE;
	priv->evt_pending += count;
	dwc3_write(dev, USB_DWC3_GEVNTCOUNT0, count);

	k_sem_give(&priv->irq_sem);
}

static int udc_dwc3_ep_enqueue(const struct device *dev, struct udc_ep_config *const cfg,
			       struct net_buf *buf)
{
	udc_buf_put(cfg, buf);

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		dwc3_ep0_kick(dev);
		return 0;
	}

	(void)dwc3_ep_start_next(dev, cfg);

	return 0;
}

static int udc_dwc3_ep_dequeue(const struct device *dev, struct udc_ep_config *const cfg)
{
	const uint8_t phy = dwc3_ep_to_phy(cfg->addr);

	dwc3_end_xfer(dev, phy);
	udc_ep_cancel_queued(dev, cfg);
	udc_ep_set_busy(cfg, false);

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		struct udc_dwc3_data *const priv = udc_get_private(dev);

		/* Any control transfer in progress is gone, wait for the next SETUP */
		dwc3_end_xfer(dev, phy ^ 1U);
		priv->setup_armed = false;
		if (udc_is_enabled(dev)) {
			dwc3_ep0_setup_start(dev);
		}
	}

	return 0;
}

static int udc_dwc3_ep_set_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	const uint8_t phy = dwc3_ep_to_phy(cfg->addr);
	int ret;

	LOG_DBG("Set halt ep 0x%02x", cfg->addr);

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		struct udc_dwc3_data *const priv = udc_get_private(dev);

		/* Both directions of the control endpoint share one stall */
		if (priv->ep0_state != DWC3_EP0_SETUP) {
			dwc3_ep0_stall_restart(dev);
		}

		return 0;
	}

	dwc3_end_xfer(dev, phy);
	udc_ep_set_busy(cfg, false);

	ret = dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_SETSTALL, 0U, 0U);
	if (ret == 0) {
		cfg->stat.halted = true;
	}

	return ret;
}

static int udc_dwc3_ep_clear_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	const uint8_t phy = dwc3_ep_to_phy(cfg->addr);
	int ret;

	LOG_DBG("Clear halt ep 0x%02x", cfg->addr);

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		/* The controller clears a control endpoint stall on SETUP */
		return 0;
	}

	/* Clear Stall also resets the data toggle */
	ret = dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_CLEARSTALL, 0U, 0U);
	if (ret != 0) {
		return ret;
	}

	cfg->stat.halted = false;

	return dwc3_ep_start_next(dev, cfg);
}

static int udc_dwc3_ep_enable(const struct device *dev, struct udc_ep_config *const cfg)
{
	const struct udc_dwc3_config *const config = dev->config;
	const uint8_t phy = dwc3_ep_to_phy(cfg->addr);
	uint32_t par0;
	uint32_t par1;
	int ret;

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		/* Control endpoint is configured during controller initialization */
		return 0;
	}

	if (phy >= config->num_phy_eps) {
		return -EINVAL;
	}

	par0 = FIELD_PREP(USB_DWC3_DEPCFG_EP_TYPE_MASK,
			  cfg->attributes & USB_EP_TRANSFER_TYPE_MASK) |
	       FIELD_PREP(USB_DWC3_DEPCFG_MAX_PACKET_SIZE_MASK, udc_mps_ep_size(cfg)) |
	       FIELD_PREP(USB_DWC3_DEPCFG_ACTION_MASK, USB_DWC3_DEPCFG_ACTION_INIT);
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		par0 |= FIELD_PREP(USB_DWC3_DEPCFG_FIFO_NUMBER_MASK, USB_EP_GET_IDX(cfg->addr));
	}

	par1 = FIELD_PREP(USB_DWC3_DEPCFG_EP_NUMBER_MASK, phy) |
	       USB_DWC3_DEPCFG_XFER_COMPLETE_EN;
	if (cfg->interval != 0U) {
		par1 |= FIELD_PREP(USB_DWC3_DEPCFG_BINTERVAL_M1_MASK,
				   MIN(cfg->interval - 1U, 13U));
	}

	ret = dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_SETEPCONFIG, par0, par1);
	if (ret != 0) {
		return ret;
	}

	config->eps[phy] = (struct udc_dwc3_ep){0};
	dwc3_modify(dev, USB_DWC3_DALEPENA, 0U, BIT(phy));
	LOG_DBG("Enable ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_dwc3_ep_disable(const struct device *dev, struct udc_ep_config *const cfg)
{
	const uint8_t phy = dwc3_ep_to_phy(cfg->addr);

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		return 0;
	}

	dwc3_end_xfer(dev, phy);
	udc_ep_set_busy(cfg, false);
	dwc3_modify(dev, USB_DWC3_DALEPENA, BIT(phy), 0U);
	LOG_DBG("Disable ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_dwc3_set_address(const struct device *dev, const uint8_t addr)
{
	LOG_DBG("Set new address %u for %p", addr, dev);
	dwc3_modify(dev, USB_DWC3_DCFG, USB_DWC3_DCFG_DEVADDR_MASK,
		    FIELD_PREP(USB_DWC3_DCFG_DEVADDR_MASK, addr));

	return 0;
}

static int udc_dwc3_host_wakeup(const struct device *dev)
{
	dwc3_modify(dev, USB_DWC3_DCTL, USB_DWC3_DCTL_ULSTCHNGREQ_MASK,
		    FIELD_PREP(USB_DWC3_DCTL_ULSTCHNGREQ_MASK, USB_DWC3_DCTL_ULSTCHNGREQ_RECOVERY));

	for (uint32_t i = 0; i < DWC3_WAKEUP_TIMEOUT_US; i++) {
		uint32_t dsts = dwc3_read(dev, USB_DWC3_DSTS);

		if (FIELD_GET(USB_DWC3_DSTS_USBLNKST_MASK, dsts) == USB_DWC3_LINK_STATE_ON) {
			return 0;
		}

		k_busy_wait(1);
	}

	LOG_ERR("Remote wakeup timed out");

	return -ETIMEDOUT;
}

static enum udc_bus_speed udc_dwc3_device_speed(const struct device *dev)
{
	struct udc_dwc3_data *const priv = udc_get_private(dev);

	return priv->speed;
}

static int dwc3_wait_halted(const struct device *dev, const bool halted)
{
	for (uint32_t i = 0; i < DWC3_HALT_TIMEOUT_US; i++) {
		bool is_halted = (dwc3_read(dev, USB_DWC3_DSTS) & USB_DWC3_DSTS_DEVCTRLHLT) != 0U;

		if (is_halted == halted) {
			return 0;
		}

		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int udc_dwc3_enable(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	int ret;

	ret = udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL, DWC3_MPS0, 0);
	if (ret != 0) {
		LOG_ERR("Failed to enable control endpoint");
		return ret;
	}

	ret = udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL, DWC3_MPS0, 0);
	if (ret != 0) {
		LOG_ERR("Failed to enable control endpoint");
		return ret;
	}

	priv->setup_armed = false;
	dwc3_ep0_setup_start(dev);
	if (!priv->setup_armed) {
		return -EIO;
	}

	config->irq_enable_func(dev);
	dwc3_modify(dev, USB_DWC3_DCTL, 0U, USB_DWC3_DCTL_RUN_STOP);

	ret = dwc3_wait_halted(dev, false);
	if (ret != 0) {
		LOG_ERR("Controller did not start");
	}

	return ret;
}

static int udc_dwc3_disable(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	int ret;

	for (uint8_t phy = 0U; phy < config->num_phy_eps; phy++) {
		dwc3_end_xfer(dev, phy);
	}

	priv->setup_armed = false;
	priv->ep0_state = DWC3_EP0_SETUP;

	dwc3_modify(dev, USB_DWC3_DCTL, USB_DWC3_DCTL_RUN_STOP, 0U);
	ret = dwc3_wait_halted(dev, true);
	if (ret != 0) {
		LOG_ERR("Controller did not halt");
	}

	config->irq_disable_func(dev);

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT) != 0 ||
	    udc_ep_disable_internal(dev, USB_CONTROL_EP_IN) != 0) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	return ret;
}

static int dwc3_core_reset(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	uint32_t reg;
	bool done = false;

	dwc3_modify(dev, USB_DWC3_DCTL, 0U, USB_DWC3_DCTL_CSFTRST);
	for (uint32_t i = 0; i < DWC3_RESET_TIMEOUT_US; i++) {
		if ((dwc3_read(dev, USB_DWC3_DCTL) & USB_DWC3_DCTL_CSFTRST) == 0U) {
			done = true;
			break;
		}

		k_busy_wait(1);
	}

	if (!done) {
		LOG_ERR("Device soft reset timed out");
		return -ETIMEDOUT;
	}

	k_busy_wait(DWC3_SOFT_RESET_SETTLE_US);

	/* Reset the USB 2.0 PHY with the core held in reset */
	dwc3_modify(dev, USB_DWC3_GCTL, 0U, USB_DWC3_GCTL_CORESOFTRESET);
	dwc3_modify(dev, USB_DWC3_GUSB2PHYCFG0, 0U, USB_DWC3_GUSB2PHYCFG_PHYSOFTRST);
	k_busy_wait(DWC3_PHY_RESET_US);
	dwc3_modify(dev, USB_DWC3_GUSB2PHYCFG0, USB_DWC3_GUSB2PHYCFG_PHYSOFTRST, 0U);
	k_busy_wait(DWC3_PHY_RESET_US);
	dwc3_modify(dev, USB_DWC3_GCTL, USB_DWC3_GCTL_CORESOFTRESET, 0U);

	/* Auto-resume must be disabled in device mode */
	reg = dwc3_read(dev, USB_DWC3_GUSB2PHYCFG0);
	reg &= ~(USB_DWC3_GUSB2PHYCFG_ULPIAUTORES | USB_DWC3_GUSB2PHYCFG_PHYIF |
		 USB_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
	if (config->utmi_width == 16U) {
		reg |= USB_DWC3_GUSB2PHYCFG_PHYIF |
		       FIELD_PREP(USB_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK,
				  USB_DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT);
	} else {
		reg |= FIELD_PREP(USB_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK,
				  USB_DWC3_GUSB2PHYCFG_USBTRDTIM_8BIT);
	}

	dwc3_write(dev, USB_DWC3_GUSB2PHYCFG0, reg);

	return 0;
}

static int udc_dwc3_init(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	uint32_t num_eps;
	int ret;

	for (size_t i = 0; i < config->num_clocks; i++) {
		ret = clock_control_on(config->clock_dev, config->clock_subsys[i]);
		if (ret != 0 && ret != -EALREADY) {
			LOG_ERR("Failed to enable clock %zu (%d)", i, ret);
			return ret;
		}
	}

	if (FIELD_GET(USB_DWC3_GSNPSID_ID_MASK, dwc3_read(dev, USB_DWC3_GSNPSID)) !=
	    USB_DWC3_GSNPSID_ID_USB3) {
		LOG_ERR("Unsupported controller ID 0x%08x", dwc3_read(dev, USB_DWC3_GSNPSID));
		return -ENOTSUP;
	}

	num_eps = FIELD_GET(USB_DWC3_GHWPARAMS3_NUM_EPS_MASK,
			    dwc3_read(dev, USB_DWC3_GHWPARAMS3));
	if (config->num_phy_eps > num_eps) {
		LOG_ERR("Controller has %u endpoints, devicetree needs %u",
			num_eps, config->num_phy_eps);
		return -EINVAL;
	}

	ret = dwc3_core_reset(dev);
	if (ret != 0) {
		return ret;
	}

	dwc3_modify(dev, USB_DWC3_GCTL, USB_DWC3_GCTL_PRTCAPDIR_MASK,
		    FIELD_PREP(USB_DWC3_GCTL_PRTCAPDIR_MASK, USB_DWC3_GCTL_PRTCAPDIR_DEVICE) |
		    USB_DWC3_GCTL_DSBLCLKGTNG);
	dwc3_modify(dev, USB_DWC3_GSBUSCFG0, 0U,
		    USB_DWC3_GSBUSCFG0_INCRBRSTENA | USB_DWC3_GSBUSCFG0_INCR32BRSTENA);

	memset(config->evt_buf, 0, DWC3_EVT_BUF_SIZE);
	sys_cache_data_flush_and_invd_range(config->evt_buf, DWC3_EVT_BUF_SIZE);
	priv->evt_pos = 0U;
	priv->evt_isr_pos = 0U;
	priv->evt_pending = 0U;
	dwc3_write(dev, USB_DWC3_GEVNTADRLO0, dwc3_dma_addr(dev, config->evt_buf));
	dwc3_write(dev, USB_DWC3_GEVNTADRHI0, 0U);
	dwc3_write(dev, USB_DWC3_GEVNTSIZ0,
		   FIELD_PREP(USB_DWC3_GEVNTSIZ_SIZE_MASK, DWC3_EVT_BUF_SIZE));
	dwc3_write(dev, USB_DWC3_GEVNTCOUNT0,
		   dwc3_read(dev, USB_DWC3_GEVNTCOUNT0) & USB_DWC3_GEVNTCOUNT_MASK);

	dwc3_modify(dev, USB_DWC3_DCFG,
		    USB_DWC3_DCFG_SPEED_MASK | USB_DWC3_DCFG_DEVADDR_MASK,
		    config->hs ? USB_DWC3_DCFG_SPEED_HS : USB_DWC3_DCFG_SPEED_FS);

	ret = dwc3_depcmd(dev, 0U, USB_DWC3_DEPCMD_DEPSTARTCFG, 0U, 0U);
	if (ret != 0) {
		return ret;
	}

	/* The control endpoint is configured before any transfer resource is assigned */
	ret = dwc3_ep0_config(dev, 0U, USB_DWC3_DEPCFG_ACTION_INIT);
	if (ret == 0) {
		ret = dwc3_ep0_config(dev, 1U, USB_DWC3_DEPCFG_ACTION_INIT);
	}

	if (ret != 0) {
		return ret;
	}

	/*
	 * Transfer resources assigned here stay valid until the next core
	 * reset, so every physical endpoint gets one now.
	 */
	for (uint8_t phy = 0U; phy < num_eps; phy++) {
		ret = dwc3_depcmd(dev, phy, USB_DWC3_DEPCMD_SETTRANSFRESOURCE,
				  FIELD_PREP(USB_DWC3_DEPXFERCFG_NUM_XFER_RES_MASK, 1U), 0U);
		if (ret != 0) {
			return ret;
		}
	}

	memset(config->eps, 0, sizeof(config->eps[0]) * config->num_phy_eps);
	dwc3_write(dev, USB_DWC3_DALEPENA, BIT(0) | BIT(1));

	dwc3_write(dev, USB_DWC3_DEVTEN,
		   USB_DWC3_DEVTEN_DISSCONNEVTEN | USB_DWC3_DEVTEN_USBRSTEVTEN |
		   USB_DWC3_DEVTEN_CONNECTDONEEVTEN | USB_DWC3_DEVTEN_ULSTCNGEN |
		   USB_DWC3_DEVTEN_WKUPEVTEN);

	priv->speed = config->hs ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS;
	priv->ep0_state = DWC3_EP0_SETUP;
	priv->setup_armed = false;

	return 0;
}

static int udc_dwc3_shutdown(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;

	dwc3_write(dev, USB_DWC3_DEVTEN, 0U);
	dwc3_write(dev, USB_DWC3_GEVNTSIZ0, USB_DWC3_GEVNTSIZ_INTMASK);

	for (size_t i = 0; i < config->num_clocks; i++) {
		(void)clock_control_off(config->clock_dev, config->clock_subsys[i]);
	}

	return 0;
}

static void udc_dwc3_lock(const struct device *dev)
{
	k_sched_lock();
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_dwc3_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
	k_sched_unlock();
}

static int udc_dwc3_driver_preinit(const struct device *dev)
{
	const struct udc_dwc3_config *const config = dev->config;
	struct udc_dwc3_data *const priv = udc_get_private(dev);
	struct udc_data *const data = dev->data;
	const uint16_t mps = config->hs ? 512U : 64U;
	int err;

	data->caps.rwup = true;
	data->caps.addr_before_status = true;
	data->caps.mps0 = UDC_MPS0_64;
	data->caps.hs = config->hs;

	for (size_t i = 0; i < config->num_out_eps; i++) {
		struct udc_ep_config *cfg = &config->ep_cfg_out[i];

		cfg->caps.out = 1;
		if (i == 0) {
			cfg->caps.control = 1;
			cfg->caps.mps = DWC3_MPS0;
		} else {
			cfg->caps.bulk = 1;
			cfg->caps.interrupt = 1;
			cfg->caps.mps = mps;
		}

		cfg->addr = USB_EP_DIR_OUT | i;
		err = udc_register_ep(dev, cfg);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	for (size_t i = 0; i < config->num_in_eps; i++) {
		struct udc_ep_config *cfg = &config->ep_cfg_in[i];

		cfg->caps.in = 1;
		if (i == 0) {
			cfg->caps.control = 1;
			cfg->caps.mps = DWC3_MPS0;
		} else {
			cfg->caps.bulk = 1;
			cfg->caps.interrupt = 1;
			cfg->caps.mps = mps;
		}

		cfg->addr = USB_EP_DIR_IN | i;
		err = udc_register_ep(dev, cfg);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	priv->dev = dev;
	k_sem_init(&priv->irq_sem, 0, 1);
	config->make_thread(dev);

	return 0;
}

static const struct udc_api udc_dwc3_api = {
	.lock = udc_dwc3_lock,
	.unlock = udc_dwc3_unlock,
	.device_speed = udc_dwc3_device_speed,
	.init = udc_dwc3_init,
	.enable = udc_dwc3_enable,
	.disable = udc_dwc3_disable,
	.shutdown = udc_dwc3_shutdown,
	.set_address = udc_dwc3_set_address,
	.host_wakeup = udc_dwc3_host_wakeup,
	.ep_enable = udc_dwc3_ep_enable,
	.ep_disable = udc_dwc3_ep_disable,
	.ep_set_halt = udc_dwc3_ep_set_halt,
	.ep_clear_halt = udc_dwc3_ep_clear_halt,
	.ep_enqueue = udc_dwc3_ep_enqueue,
	.ep_dequeue = udc_dwc3_ep_dequeue,
};

/* Physical endpoints in use: OUT endpoints are even, IN endpoints odd */
#define UDC_DWC3_NUM_PHY_EPS(n)							\
	MAX(2 * DT_INST_PROP(n, num_out_eps) - 1, 2 * DT_INST_PROP(n, num_in_eps))

#define UDC_DWC3_DMA_RANGE(node_id, idx)					\
	{									\
		.cpu_addr = DT_DMA_RANGES_PARENT_BUS_ADDRESS_BY_IDX(node_id, idx),\
		.bus_addr = DT_DMA_RANGES_CHILD_BUS_ADDRESS_BY_IDX(node_id, idx),\
		.length = DT_DMA_RANGES_LENGTH_BY_IDX(node_id, idx),		\
	},

/* dma-ranges is not reported by DT_NODE_HAS_PROP(), count the ranges instead */
#define UDC_DWC3_NUM_DMA_RANGES(n) DT_NUM_DMA_RANGES(DT_INST_PARENT(n))

#define UDC_DWC3_DMA_RANGES_DEFINE(n)						\
	COND_CODE_0(UDC_DWC3_NUM_DMA_RANGES(n), (), (				\
	static const struct udc_dwc3_dma_range udc_dwc3_dma_ranges_##n[] = {	\
		DT_FOREACH_DMA_RANGE(DT_INST_PARENT(n), UDC_DWC3_DMA_RANGE)	\
	};))

#define UDC_DWC3_DMA_RANGES_INIT(n)						\
	COND_CODE_0(UDC_DWC3_NUM_DMA_RANGES(n), (),				\
		    (.dma_ranges = udc_dwc3_dma_ranges_##n,			\
		     .num_dma_ranges = ARRAY_SIZE(udc_dwc3_dma_ranges_##n),))

/* Memory the controller accesses goes to the region it can reach, if limited */
#define UDC_DWC3_DMA_SECTION(n)							\
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, memory_regions),			\
		    (Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(		\
			DT_INST_PHANDLE(n, memory_regions)))),			\
		    ())

#define UDC_DWC3_CLOCK_SUBSYS(idx, n)						\
	(clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_IDX(n, idx, clkid)

#define UDC_DWC3_DEVICE_DEFINE(n)						\
	K_THREAD_STACK_DEFINE(udc_dwc3_stack_##n, CONFIG_UDC_DWC3_STACK_SIZE);	\
										\
	static void udc_dwc3_thread_##n(void *dev, void *arg1, void *arg2)	\
	{									\
		udc_dwc3_thread_handler(dev);					\
	}									\
										\
	static void udc_dwc3_make_thread_##n(const struct device *dev)		\
	{									\
		struct udc_dwc3_data *priv = udc_get_private(dev);		\
										\
		k_thread_create(&priv->thread_data,				\
				udc_dwc3_stack_##n,				\
				K_THREAD_STACK_SIZEOF(udc_dwc3_stack_##n),	\
				udc_dwc3_thread_##n,				\
				(void *)dev, NULL, NULL,			\
				K_PRIO_COOP(CONFIG_UDC_DWC3_THREAD_PRIORITY),	\
				K_ESSENTIAL,					\
				K_NO_WAIT);					\
		k_thread_name_set(&priv->thread_data, dev->name);		\
	}									\
										\
	static void udc_dwc3_irq_enable_func_##n(const struct device *dev)	\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    udc_dwc3_isr_handler, DEVICE_DT_INST_GET(n), 0);	\
		irq_enable(DT_INST_IRQN(n));					\
	}									\
										\
	static void udc_dwc3_irq_disable_func_##n(const struct device *dev)	\
	{									\
		irq_disable(DT_INST_IRQN(n));					\
	}									\
										\
	UDC_DWC3_DMA_RANGES_DEFINE(n)						\
										\
	static const clock_control_subsys_t udc_dwc3_clocks_##n[] = {		\
		LISTIFY(DT_INST_NUM_CLOCKS(n), UDC_DWC3_CLOCK_SUBSYS, (,), n)	\
	};									\
										\
	static struct udc_ep_config						\
		ep_cfg_out_##n[DT_INST_PROP(n, num_out_eps)];			\
	static struct udc_ep_config						\
		ep_cfg_in_##n[DT_INST_PROP(n, num_in_eps)];			\
	static struct udc_dwc3_ep udc_dwc3_eps_##n[UDC_DWC3_NUM_PHY_EPS(n)];	\
	static struct usb_dwc3_trb UDC_DWC3_DMA_SECTION(n)			\
		udc_dwc3_trbs_##n[UDC_DWC3_NUM_PHY_EPS(n)][DWC3_TRBS_PER_EP]	\
		__aligned(DWC3_TRB_ALIGN);					\
	static uint8_t UDC_DWC3_DMA_SECTION(n)					\
		udc_dwc3_evt_buf_##n[DWC3_EVT_BUF_SIZE]				\
		__aligned(DWC3_EVT_BUF_SIZE);					\
	static uint8_t udc_dwc3_evt_cache_##n[DWC3_EVT_BUF_SIZE] __aligned(4);	\
	static uint8_t UDC_DWC3_DMA_SECTION(n)					\
		udc_dwc3_setup_buf_##n[DWC3_SETUP_BUF_SIZE]			\
		__aligned(USB_BUF_ALIGN);					\
										\
	static const struct udc_dwc3_config udc_dwc3_config_##n = {		\
		.base = DT_INST_REG_ADDR(n),					\
		.num_out_eps = DT_INST_PROP(n, num_out_eps),			\
		.num_in_eps = DT_INST_PROP(n, num_in_eps),			\
		.ep_cfg_in = ep_cfg_in_##n,					\
		.ep_cfg_out = ep_cfg_out_##n,					\
		.eps = udc_dwc3_eps_##n,					\
		.trbs = udc_dwc3_trbs_##n,					\
		.evt_buf = udc_dwc3_evt_buf_##n,				\
		.evt_cache = udc_dwc3_evt_cache_##n,				\
		.setup_buf = udc_dwc3_setup_buf_##n,				\
		UDC_DWC3_DMA_RANGES_INIT(n)					\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),		\
		.clock_subsys = udc_dwc3_clocks_##n,				\
		.num_clocks = DT_INST_NUM_CLOCKS(n),				\
		.irq_enable_func = udc_dwc3_irq_enable_func_##n,		\
		.irq_disable_func = udc_dwc3_irq_disable_func_##n,		\
		.make_thread = udc_dwc3_make_thread_##n,			\
		.num_phy_eps = UDC_DWC3_NUM_PHY_EPS(n),				\
		.utmi_width = DT_INST_PROP(n, utmi_width),			\
		.hs = DT_INST_ENUM_IDX_OR(n, maximum_speed, 2) >= 2,		\
	};									\
										\
	static struct udc_dwc3_data udc_priv_##n;				\
										\
	static struct udc_data udc_data_##n = {					\
		.mutex = Z_MUTEX_INITIALIZER(udc_data_##n.mutex),		\
		.priv = &udc_priv_##n,						\
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, udc_dwc3_driver_preinit, NULL,			\
			      &udc_data_##n, &udc_dwc3_config_##n,		\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &udc_dwc3_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_DWC3_DEVICE_DEFINE)
