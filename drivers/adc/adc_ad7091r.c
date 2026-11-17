/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ADC driver for the Analog Devices AD7091R 12-bit SAR ADC.
 *
 * Conversions are started with a pulse on CONVST and the result is read
 * without the BUSY indicator as a 16-bit SPI frame: DB11 to DB0 followed by
 * four don't care bits.
 */

#define DT_DRV_COMPAT adi_ad7091r

#include <errno.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ad7091r, CONFIG_ADC_LOG_LEVEL);

#define AD7091R_RESOLUTION       12U
#define AD7091R_REF_INTERNAL_MV  2500U
#define AD7091R_FRAME_SIZE       2U
#define AD7091R_DATA_SHIFT       4U
/* A read ended by the eighth SCLK cycle requests a software reset */
#define AD7091R_RESET_FRAME_SIZE 1U
/* Conversion time is 650 ns maximum */
#define AD7091R_CONV_TIME_US     1U
/* Power-up time from power-down mode */
#define AD7091R_POWER_UP_US      100U
/* Typical charge time of the 2.2 uF capacitor on the internal reference */
#define AD7091R_REF_CHARGE_MS    50U

struct ad7091r_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec convst_gpio;
};

struct ad7091r_data {
	struct k_mutex lock;
};

static int ad7091r_start_conversion(const struct device *dev)
{
	const struct ad7091r_config *config = dev->config;
	int ret;

	/*
	 * The falling edge starts the conversion. CONVST must be back high at
	 * the end of the conversion to keep the device in normal mode;
	 * otherwise it enters power-down mode and powers up again on the
	 * rising edge, which still yields a valid result for this conversion.
	 */
	ret = gpio_pin_set_dt(&config->convst_gpio, 1);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&config->convst_gpio, 0);
	if (ret != 0) {
		return ret;
	}

	k_busy_wait(AD7091R_CONV_TIME_US);

	return 0;
}

static int ad7091r_read_frame(const struct device *dev, uint8_t *buf, size_t len)
{
	const struct ad7091r_config *config = dev->config;
	struct spi_buf rx_buf = {.buf = buf, .len = len};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1U};

	return spi_read_dt(&config->bus, &rx_set);
}

static int ad7091r_channel_setup(const struct device *dev,
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

	if ((channel_cfg->reference != ADC_REF_INTERNAL) &&
	    (channel_cfg->reference != ADC_REF_EXTERNAL0)) {
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

static int ad7091r_convert(const struct device *dev, uint16_t *result)
{
	uint8_t rx[AD7091R_FRAME_SIZE];
	int ret;

	ret = ad7091r_start_conversion(dev);
	if (ret != 0) {
		return ret;
	}

	ret = ad7091r_read_frame(dev, rx, sizeof(rx));
	if (ret != 0) {
		return ret;
	}

	*result = sys_get_be16(rx) >> AD7091R_DATA_SHIFT;

	LOG_DBG("result 0x%03x", *result);

	return 0;
}

static int ad7091r_read(const struct device *dev, const struct adc_sequence *sequence)
{
	struct ad7091r_data *data = dev->data;
	int ret;

	if (sequence->options != NULL) {
		LOG_ERR("sequence options are not supported");
		return -ENOTSUP;
	}

	if (sequence->channels != BIT(0)) {
		LOG_ERR("unsupported channels mask 0x%08x", sequence->channels);
		return -ENOTSUP;
	}

	if (sequence->resolution != AD7091R_RESOLUTION) {
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
	ret = ad7091r_convert(dev, sequence->buffer);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int ad7091r_reset(const struct device *dev)
{
	uint8_t rx[AD7091R_RESET_FRAME_SIZE];
	int ret;

	ret = ad7091r_start_conversion(dev);
	if (ret != 0) {
		return ret;
	}

	ret = ad7091r_read_frame(dev, rx, sizeof(rx));
	if (ret != 0) {
		return ret;
	}

	/* The software reset is executed at the end of the next conversion */
	return ad7091r_start_conversion(dev);
}

static int ad7091r_init(const struct device *dev)
{
	const struct ad7091r_config *config = dev->config;
	struct ad7091r_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	if (!spi_is_ready_dt(&config->bus)) {
		LOG_ERR("SPI bus %s not ready", config->bus.bus->name);
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->convst_gpio)) {
		LOG_ERR("CONVST GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->convst_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}

	k_busy_wait(AD7091R_POWER_UP_US);

	ret = ad7091r_reset(dev);
	if (ret != 0) {
		LOG_ERR("software reset failed (%d)", ret);
		return ret;
	}

	k_msleep(AD7091R_REF_CHARGE_MS);

	return 0;
}

static DEVICE_API(adc, ad7091r_api) = {
	.channel_setup = ad7091r_channel_setup,
	.read = ad7091r_read,
	.ref_internal = AD7091R_REF_INTERNAL_MV,
};

#define AD7091R_INIT(inst)                                                                         \
	static const struct ad7091r_config ad7091r_config_##inst = {                               \
		.bus = SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8) | SPI_TRANSFER_MSB),             \
		.convst_gpio = GPIO_DT_SPEC_INST_GET(inst, convst_gpios),                          \
	};                                                                                         \
                                                                                                   \
	static struct ad7091r_data ad7091r_data_##inst;                                            \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, ad7091r_init, NULL, &ad7091r_data_##inst,                      \
			      &ad7091r_config_##inst, POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,       \
			      &ad7091r_api);

DT_INST_FOREACH_STATUS_OKAY(AD7091R_INIT)
