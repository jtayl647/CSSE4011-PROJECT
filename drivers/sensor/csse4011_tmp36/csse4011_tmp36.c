#define DT_DRV_COMPAT csse4011_tmp36

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>

LOG_MODULE_REGISTER(csse4011_tmp36, CONFIG_SENSOR_LOG_LEVEL);

#define TMP36_ADC_NODE    DT_NODELABEL(adc)
#define TMP36_ADC_CHANNEL 1      
#define TMP36_RESOLUTION  12
#define TMP36_PERIOD_MS   100 // THIS CHANGES...
#define TMP36_STACK_SIZE  512
#define TMP36_PRIO        5

struct tmp36_data {
	const struct device *dev;
	atomic_t             sampled_mv;   /* raw millivolts from ADC */
	int32_t              last_mv;
	uint16_t             sample_buffer;
	struct adc_sequence  sequence;
	struct k_thread      thread;
	K_KERNEL_STACK_MEMBER(stack, TMP36_STACK_SIZE);
};

struct tmp36_config {
	const struct device    *adc_dev;
	struct adc_channel_cfg  adc_cfg;
};

static void tmp36_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct tmp36_data         *data = arg1;
	const struct device       *dev  = data->dev;
	const struct tmp36_config *cfg  = dev->config;

	while (1) {
		if (adc_read(cfg->adc_dev, &data->sequence) == 0) {
			int32_t mv = (int32_t)data->sample_buffer;

			if (adc_raw_to_millivolts(adc_ref_internal(cfg->adc_dev),
						  cfg->adc_cfg.gain,
						  TMP36_RESOLUTION,
						  &mv) == 0) {
				atomic_set(&data->sampled_mv, mv);
			}
		}
		k_msleep(TMP36_PERIOD_MS);
	}
}

static int tmp36_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct tmp36_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL &&
	    chan != SENSOR_CHAN_VOLTAGE &&
	    chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	data->last_mv = (int32_t)atomic_get(&data->sampled_mv);
	return 0;
}

static int tmp36_channel_get(const struct device *dev,
			     enum sensor_channel chan,
			     struct sensor_value *val)
{
	struct tmp36_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_VOLTAGE:
		/* Report raw millivolts from ADC */
		val->val1 = data->last_mv / 1000;
		val->val2 = (data->last_mv % 1000) * 1000;
		break;

	case SENSOR_CHAN_AMBIENT_TEMP:
		/* TMP36 formula: Temp(C) = (mV - 500) / 10
		 * e.g. 750mV = 25C, 500mV = 0C */
		val->val1 = (data->last_mv - 500) / 10;
		val->val2 = ((data->last_mv - 500) % 10) * 100000;
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static int tmp36_init(const struct device *dev)
{
	struct tmp36_data         *data = dev->data;
	const struct tmp36_config *cfg  = dev->config;

	if (!device_is_ready(cfg->adc_dev)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	data->dev = dev;
	atomic_set(&data->sampled_mv, 0);

	if (adc_channel_setup(cfg->adc_dev, &cfg->adc_cfg) < 0) {
		LOG_ERR("ADC channel setup failed");
		return -EIO;
	}

	data->sequence = (struct adc_sequence){
		.channels    = BIT(TMP36_ADC_CHANNEL),
		.buffer      = &data->sample_buffer,
		.buffer_size = sizeof(data->sample_buffer),
		.resolution  = TMP36_RESOLUTION,
	};

	k_thread_create(&data->thread, data->stack,
			K_KERNEL_STACK_SIZEOF(data->stack),
			tmp36_thread_fn, data, NULL, NULL,
			TMP36_PRIO, 0, K_NO_WAIT);

	k_thread_name_set(&data->thread, "tmp36");
	return 0;
}

static DEVICE_API(sensor, tmp36_api) = {
	.sample_fetch = tmp36_sample_fetch,
	.channel_get  = tmp36_channel_get,
};

#define TMP36_INIT(inst)                                           \
	static struct tmp36_data tmp36_data_##inst;                \
	static const struct tmp36_config tmp36_config_##inst = {   \
		.adc_dev = DEVICE_DT_GET(TMP36_ADC_NODE),          \
		.adc_cfg = {                                        \
			.gain             = ADC_GAIN_1_6,           \
			.reference        = ADC_REF_INTERNAL,       \
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,   \
			.channel_id       = TMP36_ADC_CHANNEL,      \
			.input_positive   = NRF_SAADC_AIN1,         \
			.differential     = 0,                      \
		},                                                  \
	};                                                          \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, tmp36_init, NULL,        \
				     &tmp36_data_##inst,            \
				     &tmp36_config_##inst,          \
				     POST_KERNEL,                   \
				     CONFIG_SENSOR_INIT_PRIORITY,   \
				     &tmp36_api);

DT_INST_FOREACH_STATUS_OKAY(TMP36_INIT)