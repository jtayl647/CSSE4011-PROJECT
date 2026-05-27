#define DT_DRV_COMPAT csse4011_qmp6988

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <string.h>

/* 
	NOTE: all this is worked out from the datasheet ->
		https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/unit/enviii/QMP6988%20Datasheet.pdf 

		Have a look or none of it will make sense. 
		The I2C stuff was worked out from their driver exmple with the SHT30: zephyr/drivers/sensor/sensirion/sht3xd/sht3xd.c
*/

LOG_MODULE_REGISTER(csse4011_qmp6988, LOG_LEVEL_INF);

/* ── Registers (datasheet 4.4) ──────────────────────────────────── */
#define QMP6988_CHIP_ID_REG     0xD1   /* read-only, always returns 0x5C        */
#define QMP6988_CHIP_ID         0x5C
#define QMP6988_RESET_REG       0xE0   /* write 0xE6 to trigger a soft reset    */
#define QMP6988_RESET_VAL       0xE6
#define QMP6988_CTRL_MEAS_REG   0xF4   /* oversampling config + power mode      */
#define QMP6988_PRESS_MSB_REG   0xF7   /* first of 6 consecutive data registers */
#define QMP6988_COE_BASE_REG    0xA0   /* first OTP calibration register        */
#define QMP6988_COE_LEN         25     /* 0xA0–0xB8 inclusive                   */

/*
 * CTRL_MEAS value: bits [7:5] = temp_average, [4:2] = press_average, [1:0] = power_mode
 *   temp_average  = 011 → 4×  oversampling
 *   press_average = 100 → 8×  oversampling  (Standard accuracy, §1.9)
 *   power_mode    = 11  → Normal (continuous) mode
 *   0b_011_100_11 = 0x73
 */
#define QMP6988_CTRL_MEAS_VAL   0x73

/* ── Data structures ─────────────────────────────────────────────── */
struct qmp6988_calib {
	float a0, a1, a2;
	float b00, bt1, bt2, bp1, b11, bp2, b12, b21, bp3;
};

struct qmp6988_data {
	struct qmp6988_calib calib;
	int32_t last_temp_cdeg;   /* centi-°C  — e.g. 25.00 °C -> 2500 */
	int32_t last_press_pa;    /* Pascals   — e.g. 101325           */
};

struct qmp6988_config {
	struct i2c_dt_spec i2c;
};

/* ── Calibration parsing (datasheet 4.3) ────────────────────────── */
/*
 * The OTP block is read as 25 bytes starting at 0xA0 (through 0xB8).
 * Register layout:
 *   d[ 0] = 0xA0  COE_b00_1      b00[19:12]
 *   d[ 1] = 0xA1  COE_b00_0      b00[11:4]
 *   d[ 2] = 0xA2  COE_bt1_1      bt1[15:8]
 *   d[ 3] = 0xA3  COE_bt1_0      bt1[7:0]
 *   d[ 4] = 0xA4  COE_bt2_1      bt2[15:8]
 *   d[ 5] = 0xA5  COE_bt2_0      bt2[7:0]
 *   d[ 6] = 0xA6  COE_bp1_1      bp1[15:8]
 *   d[ 7] = 0xA7  COE_bp1_0      bp1[7:0]
 *   d[ 8] = 0xA8  COE_b11_1      b11[15:8]
 *   d[ 9] = 0xA9  COE_b11_0      b11[7:0]
 *   d[10] = 0xAA  COE_bp2_1      bp2[15:8]
 *   d[11] = 0xAB  COE_bp2_0      bp2[7:0]
 *   d[12] = 0xAC  COE_b12_1      b12[15:8]
 *   d[13] = 0xAD  COE_b12_0      b12[7:0]
 *   d[14] = 0xAE  COE_b21_1      b21[15:8]
 *   d[15] = 0xAF  COE_b21_0      b21[7:0]
 *   d[16] = 0xB0  COE_bp3_1      bp3[15:8]
 *   d[17] = 0xB1  COE_bp3_0      bp3[7:0]
 *   d[18] = 0xB2  COE_a0_1       a0[19:12]
 *   d[19] = 0xB3  COE_a0_0       a0[11:4]
 *   d[20] = 0xB4  COE_a1_1       a1[15:8]
 *   d[21] = 0xB5  COE_a1_0       a1[7:0]
 *   d[22] = 0xB6  COE_a2_1       a2[15:8]
 *   d[23] = 0xB7  COE_a2_0       a2[7:0]
 *   d[24] = 0xB8  COE_b00_a0_ex  bits[7:4]=b00[3:0]  bits[3:0]=a0[3:0]
 *
 * a0 and b00 are 20-bit Q4 signed fixed-point values (divide raw by 16
 * to get the float). Their 4 LSBs are packed into the shared register d[24].
 *
 * All other coefficients are 16-bit signed integers mapped to floats via:
 *   coeff = raw_int16 * S / 32767.0f + K
 * where K and S come from the coefficient table in the datasheet (§4.3).
 */
static void qmp6988_parse_calib(const uint8_t *d, struct qmp6988_calib *c)
{
#define RAW16(hi, lo) ((int16_t)(((uint16_t)(d[hi]) << 8) | (d[lo])))

	/* b00: 20-bit signed Q4 — upper 16 bits in d[0:1], LSBs in d[24] upper nibble */
	int32_t raw_b00 = ((int32_t)d[0]  << 12)
	                | ((int32_t)d[1]  <<  4)
	                | ((int32_t)(d[24] >> 4) & 0x0F);
	if (raw_b00 & 0x80000) {
		raw_b00 |= ~(int32_t)0xFFFFF; /* sign-extend bit 19 to 32-bit */
	}
	c->b00 = (float)raw_b00 / 16.0f;

	/* a0: 20-bit signed Q4 — upper 16 bits in d[18:19], LSBs in d[24] lower nibble */
	int32_t raw_a0 = ((int32_t)d[18] << 12)
	               | ((int32_t)d[19] <<  4)
	               | ((int32_t)d[24] & 0x0F);
	if (raw_a0 & 0x80000) {
		raw_a0 |= ~(int32_t)0xFFFFF;
	}
	c->a0 = (float)raw_a0 / 16.0f;

	/* 16-bit coefficients: coeff = raw * S / 32767 + K  (datasheet 4.3 table)  */
	/*                                       S                   K              */
	c->bt1 = (float)RAW16( 2,  3) *  9.1E-2f  / 32767.0f +  1.0E-1f;
	c->bt2 = (float)RAW16( 4,  5) *  1.2E-6f  / 32767.0f +  1.2E-8f;
	c->bp1 = (float)RAW16( 6,  7) *  1.9E-2f  / 32767.0f +  3.3E-2f;
	c->b11 = (float)RAW16( 8,  9) *  1.4E-7f  / 32767.0f +  2.1E-7f;
	c->bp2 = (float)RAW16(10, 11) *  3.5E-10f / 32767.0f + -6.3E-10f;
	c->b12 = (float)RAW16(12, 13) *  7.6E-13f / 32767.0f +  2.9E-13f;
	c->b21 = (float)RAW16(14, 15) *  1.2E-14f / 32767.0f +  2.1E-15f;
	c->bp3 = (float)RAW16(16, 17) *  7.9E-17f / 32767.0f +  1.3E-16f;
	c->a1  = (float)RAW16(20, 21) *  4.3E-4f  / 32767.0f + -6.3E-3f;
	c->a2  = (float)RAW16(22, 23) *  1.2E-10f / 32767.0f + -1.9E-11f;

#undef RAW16
}

/* ── Compensation (datasheet 4.3 steps 6 and 7) ───────────────── */
/*
 * Inputs:
 *   Dt — signed raw temperature ADC value (raw_temp − 2^23)
 *   Dp — signed raw pressure ADC value    (raw_press − 2^23)
 *
 * Step 6: compute Tr (compensated temperature) in units of 1/256 C.
 *   Tr = a0 + a1·Dt + a2·Dt^2
 *   Actual temperature in C = Tr / 256
 *
 * Step 7: compute Pr (compensated pressure) in Pa using Tr, not Dt.
 *   The pressure polynomial is a function of the compensated temperature
 *   because the pressure sensor element has a thermal dependency that the
 *   calibration coefficients are defined against.
 */
static void qmp6988_compensate(const struct qmp6988_calib *c,
			       int32_t Dt, int32_t Dp,
			       int32_t *temp_cdeg,
			       int32_t *press_pa)
{
	float fDt = (float)Dt;
	float fDp = (float)Dp;

	/* Tr in 1/256 C */
	float Tr = c->a0 + c->a1 * fDt + c->a2 * fDt * fDt;

	/* Convert to centi-°C for output (divide by 256 for °C, multiply by 100) */
	*temp_cdeg = (int32_t)(Tr / 256.0f * 100.0f);

	/* Pressure polynomial — uses Tr, not Dt */
	float Pr = c->b00
		 + c->bt1 * Tr
		 + c->bp1 * fDp
		 + c->b11 * Tr * fDp
		 + c->bt2 * Tr * Tr
		 + c->bp2 * fDp * fDp
		 + c->b12 * Tr * Tr * fDp
		 + c->b21 * Tr * fDp * fDp
		 + c->bp3 * fDp * fDp * fDp;

	*press_pa = (int32_t)Pr;
}

/* ── sample_fetch ────────────────────────────────────────────────── */
static int qmp6988_sample_fetch(const struct device *dev,
				enum sensor_channel chan)
{
	const struct qmp6988_config *cfg = dev->config;
	struct qmp6988_data *data = dev->data;
	uint8_t buf[6];

	/*
	 * Registers 0xF7–0xFC hold two consecutive 24-bit ADC results:
	 *   buf[0..2] — PRESS_TXD2/1/0 (MSB first)
	 *   buf[3..5] — TEMP_TXD2/1/0  (MSB first)
	 */
	int err = i2c_burst_read_dt(&cfg->i2c, QMP6988_PRESS_MSB_REG, buf, 6);
	if (err) {
		return err;
	}

	uint32_t raw_press = ((uint32_t)buf[0] << 16) |
			     ((uint32_t)buf[1] <<  8) |
			      (uint32_t)buf[2];

	uint32_t raw_temp  = ((uint32_t)buf[3] << 16) |
			     ((uint32_t)buf[4] <<  8) |
			      (uint32_t)buf[5];

	/*
	 * The ADC outputs unsigned values centred at 2^23 (8388608).
	 * Subtracting the bias gives a signed value symmetric around 0,
	 * which is what the compensation polynomial expects.
	 */
	int32_t Dp = (int32_t)raw_press - (1 << 23);
	int32_t Dt = (int32_t)raw_temp  - (1 << 23);

	qmp6988_compensate(&data->calib, Dt, Dp,
			   &data->last_temp_cdeg,
			   &data->last_press_pa);

	return 0;
}

/* ── channel_get ─────────────────────────────────────────────────── */
static int qmp6988_channel_get(const struct device *dev,
				enum sensor_channel chan,
				struct sensor_value *val)
{
	const struct qmp6988_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_AMBIENT_TEMP:
		/* centi-°C -> sensor_value (e.g. 2521 -> val1=25, val2=210000) */
		val->val1 = data->last_temp_cdeg / 100;
		val->val2 = (data->last_temp_cdeg % 100) * 10000;
		break;

	case SENSOR_CHAN_PRESS:
		/* Pa -> kPa in sensor_value (e.g. 101325 Pa -> val1=101, val2=325000) */
		val->val1 = data->last_press_pa / 1000;
		val->val2 = (data->last_press_pa % 1000) * 1000;
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

/* ── init ────────────────────────────────────────────────────────── */
static int qmp6988_init(const struct device *dev)
{
	const struct qmp6988_config *cfg = dev->config;
	// check the bus is ready
	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C not ready");
		return -ENODEV;
	}
	// check the chip is there
	uint8_t chip_id;
	if (i2c_reg_read_byte_dt(&cfg->i2c, QMP6988_CHIP_ID_REG, &chip_id) ||
	    chip_id != QMP6988_CHIP_ID) {
		LOG_ERR("Bad chip ID 0x%02x (expected 0x%02x)",
			chip_id, QMP6988_CHIP_ID);
		return -ENODEV;
	}

	/* Soft reset — clears all registers to power-on defaults */
	i2c_reg_write_byte_dt(&cfg->i2c, QMP6988_RESET_REG, QMP6988_RESET_VAL);
	k_msleep(100);

	/* Read factory calibration coefficients from OTP */
	// get unique calibration
	uint8_t otp[QMP6988_COE_LEN];
	if (i2c_burst_read_dt(&cfg->i2c, QMP6988_COE_BASE_REG, otp,
			      QMP6988_COE_LEN)) {
		LOG_ERR("Failed calibration read");
		return -EIO;
	}
	qmp6988_parse_calib(otp, &((struct qmp6988_data *)dev->data)->calib);

	/*
	 * Put the chip into normal (continuous) measurement mode.
	 * After reset the chip is in sleep mode and the data registers
	 * do not update until this register is written.
	 */
	i2c_reg_write_byte_dt(&cfg->i2c,
			      QMP6988_CTRL_MEAS_REG,
			      QMP6988_CTRL_MEAS_VAL);

	LOG_INF("QMP6988 ready");
	return 0;
}

/* ── sensor API + device macro ───────────────────────────────────── */
static DEVICE_API(sensor, qmp6988_api) = {
	.sample_fetch = qmp6988_sample_fetch,
	.channel_get  = qmp6988_channel_get,
};

#define QMP6988_INIT(inst)                                          \
	static struct qmp6988_data qmp6988_data_##inst;                 \
	static const struct qmp6988_config qmp6988_config_##inst = {    \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                          \
	};                                                              \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, qmp6988_init, NULL,          \
				     &qmp6988_data_##inst,                          \
				     &qmp6988_config_##inst,                        \
				     POST_KERNEL,                                   \
				     CONFIG_SENSOR_INIT_PRIORITY,                   \
				     &qmp6988_api);

DT_INST_FOREACH_STATUS_OKAY(QMP6988_INIT)