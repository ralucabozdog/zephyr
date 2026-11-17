/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ADC driver for the Analog Devices AD74115H software configurable I/O.
 *
 * The channel is configured for voltage input and the primary ADC conversion
 * (CONV1) is exposed as ADC channel 0, measuring 0 V to 12 V with 16-bit
 * resolution. Conversions are started in single conversion mode and polled.
 */

#define DT_DRV_COMPAT adi_ad74115h

#include <errno.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ad74115h, CONFIG_ADC_LOG_LEVEL);

#define AD74115H_REG_NOP           0x00U
#define AD74115H_REG_CH_FUNC_SETUP 0x01U
#define AD74115H_REG_ADC_CONFIG    0x02U
#define AD74115H_REG_ADC_CONV_CTRL 0x3BU
#define AD74115H_REG_LIVE_STATUS   0x42U
#define AD74115H_REG_ADC_RESULT1   0x44U
#define AD74115H_REG_READ_SELECT   0x64U
#define AD74115H_REG_CMD_KEY       0x78U

#define AD74115H_CH_FUNC_MASK       GENMASK(3, 0)
#define AD74115H_CH_FUNC_HIGH_Z     0x0U
#define AD74115H_CH_FUNC_VOLTAGE_IN 0x3U

#define AD74115H_CONV1_RATE_MASK   GENMASK(12, 10)
#define AD74115H_CONV1_RANGE_MASK  GENMASK(6, 4)
/* 20 SPS conversion rate */
#define AD74115H_CONV1_RATE_20SPS  1U
/* 0 V to 12 V input range */
#define AD74115H_CONV1_RANGE_0_12V 0U

#define AD74115H_CONV_SEQ_MASK    GENMASK(13, 12)
#define AD74115H_CONV_SEQ_STANDBY 0U
#define AD74115H_CONV_SEQ_SINGLE  1U
#define AD74115H_CONV1_EN         BIT(0)

#define AD74115H_ADC_DATA_RDY BIT(3)

#define AD74115H_CMD_KEY_RESET1 0x15FAU
#define AD74115H_CMD_KEY_RESET2 0xAF51U

#define AD74115H_READ_ADDR_MASK GENMASK(6, 0)

#define AD74115H_FRAME_SIZE       4U
#define AD74115H_RESOLUTION       16U
#define AD74115H_RESET_PULSE_MS   100U
#define AD74115H_RESET_DELAY_MS   1U
/* The isolated supply of the device can take about a second to come up */
#define AD74115H_READY_TIMEOUT_MS 2000U
#define AD74115H_READY_POLL_MS    10U
#define AD74115H_CH_FUNC_DELAY_MS 1U
/* Five conversion periods at 20 SPS */
#define AD74115H_CONV_TIMEOUT_MS  250U
#define AD74115H_CONV_POLL_MS     5U

struct ad74115h_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec reset_gpio;
};

struct ad74115h_data {
	struct k_mutex lock;
};

static uint8_t ad74115h_crc(const uint8_t *frame)
{
	return crc8_ccitt(0U, frame, AD74115H_FRAME_SIZE - 1U);
}

static int ad74115h_transfer(const struct device *dev, uint8_t *tx, uint8_t *rx)
{
	const struct ad74115h_config *config = dev->config;
	const struct spi_buf tx_buf = {.buf = tx, .len = AD74115H_FRAME_SIZE};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1U};
	struct spi_buf rx_buf = {.buf = rx, .len = AD74115H_FRAME_SIZE};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1U};

	return spi_transceive_dt(&config->bus, &tx_set, (rx != NULL) ? &rx_set : NULL);
}

static int ad74115h_reg_write(const struct device *dev, uint8_t reg, uint16_t val)
{
	uint8_t tx[AD74115H_FRAME_SIZE];

	tx[0] = reg;
	sys_put_be16(val, &tx[1]);
	tx[3] = ad74115h_crc(tx);

	return ad74115h_transfer(dev, tx, NULL);
}

static int ad74115h_reg_read(const struct device *dev, uint8_t reg, uint16_t *val)
{
	uint8_t tx[AD74115H_FRAME_SIZE] = {AD74115H_REG_NOP, 0U, 0U, 0U};
	uint8_t rx[AD74115H_FRAME_SIZE];
	int ret;

	ret = ad74115h_reg_write(dev, AD74115H_REG_READ_SELECT, reg);
	if (ret != 0) {
		return ret;
	}

	tx[3] = ad74115h_crc(tx);
	ret = ad74115h_transfer(dev, tx, rx);
	if (ret != 0) {
		return ret;
	}

	if ((ad74115h_crc(rx) != rx[3]) || (FIELD_GET(AD74115H_READ_ADDR_MASK, rx[0]) != reg)) {
		LOG_DBG("invalid frame reading register 0x%02x (%02x %02x %02x %02x)", reg, rx[0],
			rx[1], rx[2], rx[3]);
		return -EIO;
	}

	*val = sys_get_be16(&rx[1]);

	return 0;
}

static int ad74115h_reg_update(const struct device *dev, uint8_t reg, uint16_t mask, uint16_t val)
{
	uint16_t tmp;
	int ret;

	ret = ad74115h_reg_read(dev, reg, &tmp);
	if (ret != 0) {
		return ret;
	}

	tmp = (tmp & ~mask) | (val & mask);

	return ad74115h_reg_write(dev, reg, tmp);
}

static int ad74115h_channel_setup(const struct device *dev,
				  const struct adc_channel_cfg *channel_cfg)
{
	if (channel_cfg->channel_id != 0U) {
		LOG_ERR("unsupported channel id %u", channel_cfg->channel_id);
		return -ENOTSUP;
	}

	if (channel_cfg->gain != ADC_GAIN_1) {
		LOG_ERR("unsupported gain %d", channel_cfg->gain);
		return -ENOTSUP;
	}

	if (channel_cfg->reference != ADC_REF_INTERNAL) {
		LOG_ERR("unsupported reference %d", channel_cfg->reference);
		return -ENOTSUP;
	}

	if (channel_cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		LOG_ERR("unsupported acquisition time %u", channel_cfg->acquisition_time);
		return -ENOTSUP;
	}

	if (channel_cfg->differential) {
		LOG_ERR("differential input is not supported");
		return -ENOTSUP;
	}

	return 0;
}

static int ad74115h_convert(const struct device *dev, uint16_t *result)
{
	int64_t timeout = k_uptime_get() + AD74115H_CONV_TIMEOUT_MS;
	uint16_t status;
	int ret;

	/* ADC_DATA_RDY is write-1-to-clear and not cleared by a new conversion */
	ret = ad74115h_reg_write(dev, AD74115H_REG_LIVE_STATUS, AD74115H_ADC_DATA_RDY);
	if (ret != 0) {
		return ret;
	}

	ret = ad74115h_reg_write(dev, AD74115H_REG_ADC_CONV_CTRL,
				 FIELD_PREP(AD74115H_CONV_SEQ_MASK, AD74115H_CONV_SEQ_SINGLE) |
					 AD74115H_CONV1_EN);
	if (ret != 0) {
		return ret;
	}

	do {
		k_msleep(AD74115H_CONV_POLL_MS);

		ret = ad74115h_reg_read(dev, AD74115H_REG_LIVE_STATUS, &status);
		if (ret != 0) {
			return ret;
		}
	} while (((status & AD74115H_ADC_DATA_RDY) == 0U) && (k_uptime_get() < timeout));

	if ((status & AD74115H_ADC_DATA_RDY) == 0U) {
		LOG_ERR("conversion timed out (LIVE_STATUS 0x%04x)", status);
		return -ETIMEDOUT;
	}

	ret = ad74115h_reg_read(dev, AD74115H_REG_ADC_RESULT1, result);
	if (ret != 0) {
		return ret;
	}

	LOG_DBG("LIVE_STATUS 0x%04x ADC_RESULT1 0x%04x", status, *result);

	return ad74115h_reg_write(dev, AD74115H_REG_ADC_CONV_CTRL,
				  FIELD_PREP(AD74115H_CONV_SEQ_MASK, AD74115H_CONV_SEQ_STANDBY));
}

static int ad74115h_read(const struct device *dev, const struct adc_sequence *sequence)
{
	struct ad74115h_data *data = dev->data;
	int ret;

	if (sequence->options != NULL) {
		LOG_ERR("sequence options are not supported");
		return -ENOTSUP;
	}

	if (sequence->channels != BIT(0)) {
		LOG_ERR("unsupported channels mask 0x%08x", sequence->channels);
		return -ENOTSUP;
	}

	if (sequence->resolution != AD74115H_RESOLUTION) {
		LOG_ERR("unsupported resolution %u", sequence->resolution);
		return -ENOTSUP;
	}

	if (sequence->oversampling != 0U) {
		LOG_ERR("oversampling is not supported");
		return -ENOTSUP;
	}

	if (sequence->buffer_size < sizeof(uint16_t)) {
		LOG_ERR("buffer size %zu is too small", sequence->buffer_size);
		return -ENOMEM;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = ad74115h_convert(dev, sequence->buffer);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int ad74115h_reset(const struct device *dev)
{
	const struct ad74115h_config *config = dev->config;
	int ret;

	if (config->reset_gpio.port != NULL) {
		ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			return ret;
		}

		k_msleep(AD74115H_RESET_PULSE_MS);

		ret = gpio_pin_set_dt(&config->reset_gpio, 0);
	} else {
		ret = ad74115h_reg_write(dev, AD74115H_REG_CMD_KEY, AD74115H_CMD_KEY_RESET1);
		if (ret != 0) {
			return ret;
		}

		ret = ad74115h_reg_write(dev, AD74115H_REG_CMD_KEY, AD74115H_CMD_KEY_RESET2);
	}

	if (ret != 0) {
		return ret;
	}

	k_msleep(AD74115H_RESET_DELAY_MS);

	return 0;
}

static int ad74115h_wait_ready(const struct device *dev)
{
	int64_t timeout = k_uptime_get() + AD74115H_READY_TIMEOUT_MS;
	uint16_t val;
	int ret;

	do {
		ret = ad74115h_reg_read(dev, AD74115H_REG_CH_FUNC_SETUP, &val);
		if (ret != -EIO) {
			return ret;
		}

		k_msleep(AD74115H_READY_POLL_MS);
	} while (k_uptime_get() < timeout);

	LOG_ERR("no response from device");

	return -EIO;
}

static int ad74115h_init(const struct device *dev)
{
	const struct ad74115h_config *config = dev->config;
	struct ad74115h_data *data = dev->data;
	uint16_t val;
	int ret;

	k_mutex_init(&data->lock);

	if (!spi_is_ready_dt(&config->bus)) {
		LOG_ERR("SPI bus %s not ready", config->bus.bus->name);
		return -ENODEV;
	}

	if ((config->reset_gpio.port != NULL) && !gpio_is_ready_dt(&config->reset_gpio)) {
		LOG_ERR("reset GPIO not ready");
		return -ENODEV;
	}

	ret = ad74115h_reset(dev);
	if (ret != 0) {
		LOG_ERR("reset failed (%d)", ret);
		return ret;
	}

	ret = ad74115h_wait_ready(dev);
	if (ret != 0) {
		return ret;
	}

	/* The channel function is changed through high impedance */
	ret = ad74115h_reg_write(dev, AD74115H_REG_CH_FUNC_SETUP, AD74115H_CH_FUNC_HIGH_Z);
	if (ret != 0) {
		return ret;
	}

	k_msleep(AD74115H_CH_FUNC_DELAY_MS);

	ret = ad74115h_reg_write(dev, AD74115H_REG_CH_FUNC_SETUP, AD74115H_CH_FUNC_VOLTAGE_IN);
	if (ret != 0) {
		return ret;
	}

	k_msleep(AD74115H_CH_FUNC_DELAY_MS);

	/* Verify that writes reach the device */
	ret = ad74115h_reg_read(dev, AD74115H_REG_CH_FUNC_SETUP, &val);
	if (ret != 0) {
		return ret;
	}

	if (FIELD_GET(AD74115H_CH_FUNC_MASK, val) != AD74115H_CH_FUNC_VOLTAGE_IN) {
		LOG_ERR("channel function not applied (CH_FUNC_SETUP 0x%04x)", val);
		return -EIO;
	}

	ret = ad74115h_reg_update(
		dev, AD74115H_REG_ADC_CONFIG, AD74115H_CONV1_RATE_MASK | AD74115H_CONV1_RANGE_MASK,
		FIELD_PREP(AD74115H_CONV1_RATE_MASK, AD74115H_CONV1_RATE_20SPS) |
			FIELD_PREP(AD74115H_CONV1_RANGE_MASK, AD74115H_CONV1_RANGE_0_12V));
	if (ret != 0) {
		LOG_ERR("ADC configuration failed (%d)", ret);
		return ret;
	}

	return 0;
}

static DEVICE_API(adc, ad74115h_api) = {
	.channel_setup = ad74115h_channel_setup,
	.read = ad74115h_read,
	.ref_internal = 12000U,
};

#define AD74115H_INIT(inst)                                                                        \
	static const struct ad74115h_config ad74115h_config_##inst = {                             \
		.bus = SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8) | SPI_TRANSFER_MSB),             \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                    \
	};                                                                                         \
                                                                                                   \
	static struct ad74115h_data ad74115h_data_##inst;                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, ad74115h_init, NULL, &ad74115h_data_##inst,                    \
			      &ad74115h_config_##inst, POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,      \
			      &ad74115h_api);

DT_INST_FOREACH_STATUS_OKAY(AD74115H_INIT)
