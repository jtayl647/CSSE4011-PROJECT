#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/printk.h>
#include <string.h>

struct sensor_reading {
	int moisture;
	int temp;
	int humidity;
	int pressure;
};

static const struct json_obj_descr reading_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct sensor_reading, moisture, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct sensor_reading, temp,     JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct sensor_reading, humidity, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct sensor_reading, pressure, JSON_TOK_NUMBER),
};

int main(void)
{
	/* --- encode --- */
	struct sensor_reading reading = {
		.moisture = 65,
		.temp     = 2476,
		.humidity = 55,
		.pressure = 101325,
	};

	char buf[128];
	int ret = json_obj_encode_buf(reading_descr, ARRAY_SIZE(reading_descr),
				      &reading, buf, sizeof(buf));

	if (ret == 0) {
		printk("encoded: %s\n", buf);
	} else {
		printk("encode failed: %d\n", ret);
		return 0;
	}

	/* --- decode --- */
	struct sensor_reading out = {0};
	ret = json_obj_parse(buf, strlen(buf),
			     reading_descr, ARRAY_SIZE(reading_descr),
			     &out);

	if (ret > 0) {
		printk("moisture=%d temp=%d.%02d humidity=%d pressure=%d.%02d\n",
		       out.moisture,
		       out.temp / 100, out.temp % 100,
		       out.humidity,
		       out.pressure / 100, out.pressure % 100);
	} else {
		printk("decode failed: %d\n", ret);
	}

	return 0;
}
