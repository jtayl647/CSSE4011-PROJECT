#define DT_DRV_COMPAT csse4011_soil

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>

LOG_MODULE_REGISTER(csse4011_soil, CONFIG_SENSOR_LOG_LEVEL);

#define SOIL_ADC_NODE    DT_NODELABEL(adc)
#define SOIL_ADC_CHANNEL 0              /* AIN0 = P0.02 = D0 */
#define SOIL_RESOLUTION  12
#define SOIL_PERIOD_MS   500
#define SOIL_STACK_SIZE  512
#define SOIL_PRIO        5

/* SparkFun soil moisture sensor outputs:
 *   ~0 mV   = completely dry
 *   ~3300 mV = completely wet (3.3V supply)
 * Convert to 0-100% moisture percentage.
 */
#define SOIL_DRY_MV   0
#define SOIL_WET_MV   3300

/* Custom sensor channel for raw moisture percentage */
#define SENSOR_CHAN_SOIL_MOISTURE ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))

struct soil_data {
	const struct device *dev;
	atomic_t             sampled_mv;
	int32_t              last_mv;
	int32_t              last_pct;
	uint16_t             sample_buffer;
	struct adc_sequence  sequence;
	struct k_thread      thread;
	K_KERNEL_STACK_MEMBER(stack, SOIL_STACK_SIZE);
};

struct soil_config {
	const struct device    *adc_dev;
	struct adc_channel_cfg  adc_cfg;
};

static void soil_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct soil_data         *data = arg1;
	const struct device      *dev  = data->dev;
	const struct soil_config *cfg  = dev->config;

	while (1) {
		if (adc_read(cfg->adc_dev, &data->sequence) == 0) {
			int32_t mv = (int32_t)data->sample_buffer;

			if (adc_raw_to_millivolts(adc_ref_internal(cfg->adc_dev),
						  cfg->adc_cfg.gain,
						  SOIL_RESOLUTION,
						  &mv) == 0) {
				atomic_set(&data->sampled_mv, mv);
			}
		}
		k_msleep(SOIL_PERIOD_MS);
	}
}

static int soil_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct soil_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL &&
	    chan != SENSOR_CHAN_VOLTAGE &&
	    chan != SENSOR_CHAN_SOIL_MOISTURE) {
		return -ENOTSUP;
	}

	data->last_mv = (int32_t)atomic_get(&data->sampled_mv);

	/* Convert mV to percentage 0-100 */
	int32_t pct = ((data->last_mv - SOIL_DRY_MV) * 100) /
		      (SOIL_WET_MV - SOIL_DRY_MV);

	if (pct < 0)   pct = 0;
	if (pct > 100) pct = 100;

	data->last_pct = pct;

	return 0;
}

static int soil_channel_get(const struct device *dev,
			    enum sensor_channel chan,
			    struct sensor_value *val)
{
	struct soil_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_VOLTAGE:
		/* Raw millivolts */
		val->val1 = data->last_mv / 1000;
		val->val2 = (data->last_mv % 1000) * 1000;
		break;

	case SENSOR_CHAN_SOIL_MOISTURE:
		/* Moisture percentage 0-100 */
		val->val1 = data->last_pct;
		val->val2 = 0;
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static int soil_init(const struct device *dev)
{
	struct soil_data         *data = dev->data;
	const struct soil_config *cfg  = dev->config;

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
		.channels    = BIT(SOIL_ADC_CHANNEL),
		.buffer      = &data->sample_buffer,
		.buffer_size = sizeof(data->sample_buffer),
		.resolution  = SOIL_RESOLUTION,
	};

	k_thread_create(&data->thread, data->stack,
			K_KERNEL_STACK_SIZEOF(data->stack),
			soil_thread_fn, data, NULL, NULL,
			SOIL_PRIO, 0, K_NO_WAIT);

	k_thread_name_set(&data->thread, "soil");
	return 0;
}

static DEVICE_API(sensor, soil_api) = {
	.sample_fetch = soil_sample_fetch,
	.channel_get  = soil_channel_get,
};

#define SOIL_INIT(inst)                                              \
	static struct soil_data soil_data_##inst;                    \
	static const struct soil_config soil_config_##inst = {       \
		.adc_dev = DEVICE_DT_GET(SOIL_ADC_NODE),             \
		.adc_cfg = {                                         \
			.gain             = ADC_GAIN_1_6,            \
			.reference        = ADC_REF_INTERNAL,        \
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,    \
			.channel_id       = SOIL_ADC_CHANNEL,        \
			.input_positive   = NRF_SAADC_AIN0,          \
			.differential     = 0,                       \
		},                                                   \
	};                                                           \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, soil_init, NULL,          \
				     &soil_data_##inst,              \
				     &soil_config_##inst,            \
				     POST_KERNEL,                    \
				     CONFIG_SENSOR_INIT_PRIORITY,    \
				     &soil_api);

DT_INST_FOREACH_STATUS_OKAY(SOIL_INIT)
