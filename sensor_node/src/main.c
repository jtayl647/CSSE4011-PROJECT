#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <string.h>
#include "sensor_pb.h"
#include "nanopb_types.h"

LOG_MODULE_REGISTER(sensor_node, LOG_LEVEL_INF);

/* Custom channel for soil moisture percentage */
#define SENSOR_CHAN_SOIL_MOISTURE ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))

#define SENSOR_READ_INTERVAL_S 15
#define SENSOR_THREAD_STACK    1024
#define SENSOR_THREAD_PRIO     5

/* Latest readings — updated by sensor thread, read by BLE callback */
static atomic_t latest_moisture = ATOMIC_INIT(0);
static atomic_t latest_humidity = ATOMIC_INIT(0);
static atomic_t latest_temp     = ATOMIC_INIT(0);
static atomic_t latest_pressure = ATOMIC_INIT(0);

/* ==========================================================================
 * Sensor Thread — reads all sensors every 30 seconds
 * ========================================================================== */
static void sensor_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	k_sleep(K_SECONDS(5));

	const struct device *soil    = DEVICE_DT_GET_ANY(csse4011_soil);
	const struct device *sht30   = DEVICE_DT_GET_ANY(sensirion_sht3xd);
	const struct device *qmp6988 = DEVICE_DT_GET_ANY(csse4011_qmp6988);

	if (!device_is_ready(soil)) {
		printk("Soil sensor not ready\n");
	}
	if (!device_is_ready(sht30)) {
		printk("SHT30 not ready\n");
	}
	if (!device_is_ready(qmp6988)) {
		printk("QMP6988 not ready\n");
	}

	while (1) {
		/* Soil moisture */
		if (device_is_ready(soil)) {
			struct sensor_value moisture;
			if (sensor_sample_fetch(soil) == 0 &&
			    sensor_channel_get(soil, SENSOR_CHAN_SOIL_MOISTURE, &moisture) == 0) {
				atomic_set(&latest_moisture, moisture.val1);
				printk("Moisture: %d%%\n", moisture.val1);
			}
		} else {
			printk("Soil sensor not ready\n");
		}

		/* Temperature + humidity from SHT30 */
		if (device_is_ready(sht30)) {
			struct sensor_value temp, humidity;
			if (sensor_sample_fetch(sht30) == 0) {
				sensor_channel_get(sht30, SENSOR_CHAN_AMBIENT_TEMP, &temp);
				sensor_channel_get(sht30, SENSOR_CHAN_HUMIDITY,     &humidity);
				atomic_set(&latest_temp,     temp.val1);
				atomic_set(&latest_humidity, humidity.val1);
				printk("Temp: %d.%02d C\n", temp.val1, temp.val2 / 10000);
				printk("Humidity: %d%%\n", humidity.val1);
			}
		} else {
			printk("SHT30 not ready\n");
		}

		/* Pressure from QMP6988 */
		if (device_is_ready(qmp6988)) {
			struct sensor_value pressure;
			if (sensor_sample_fetch(qmp6988) == 0 &&
			    sensor_channel_get(qmp6988, SENSOR_CHAN_PRESS, &pressure) == 0) {
				int32_t pa = pressure.val1 * 1000 + pressure.val2 / 1000;
				atomic_set(&latest_pressure, pa);
				printk("Pressure: %d.%02d hPa\n", pa / 100, pa % 100);
			}
		} else {
			printk("QMP6988 not ready\n");
		}

		// /* THIS CODE WAS USED TO COMPARE THE TWO SENSOR TEMP -> very similar result you can have a look */
		// if (device_is_ready(qmp6988)) {
		// 	struct sensor_value pressure, qmp_temp;
		// 	if (sensor_sample_fetch(qmp6988) == 0) {
		// 		sensor_channel_get(qmp6988, SENSOR_CHAN_PRESS,        &pressure);
		// 		sensor_channel_get(qmp6988, SENSOR_CHAN_AMBIENT_TEMP, &qmp_temp);

		// 		int32_t pa = pressure.val1 * 1000 + pressure.val2 / 1000;
		// 		atomic_set(&latest_pressure, pa);

		// 		printk("Pressure: %d.%02d hPa\n", pa / 100, pa % 100);
		// 		printk("Temp comparison — SHT30: %d.%02d C  QMP6988: %d.%02d C\n",
		// 			(int32_t)atomic_get(&latest_temp),  0,   /* SHT30 already stored */
		// 			qmp_temp.val1, qmp_temp.val2 / 10000);
		// 	}
		// } else {
		// 	printk("QMP6988 not ready\n");
		// }

		k_sleep(K_SECONDS(SENSOR_READ_INTERVAL_S));
	}
}

K_THREAD_DEFINE(sensor_thread, SENSOR_THREAD_STACK,
		sensor_thread_fn, NULL, NULL, NULL,
		SENSOR_THREAD_PRIO, 0, 0);

/* ==========================================================================
 * BLE
 * ========================================================================== */
#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

/* ==========================================================================
 * NUS RX Callback
 * ========================================================================== */
static void nus_received(struct bt_conn *conn, const void *data, uint16_t len,
			 void *ctx)
{
	LOG_INF("Received: %.*s", len, (const char *)data);
	/* TODO: parse config commands (threshold, pump on/off/auto, data ACK) */
}

static void nus_notif_enabled(bool enabled, void *ctx)
{
	if (!enabled) {
		return;
	}

	printk("Mobile subscribed - sending sensor data\n");

	int32_t moisture = (int32_t)atomic_get(&latest_moisture);
	int32_t humidity = (int32_t)atomic_get(&latest_humidity);
	int32_t temp     = (int32_t)atomic_get(&latest_temp);
	int32_t pa       = (int32_t)atomic_get(&latest_pressure);
	/* Send pressure as hPa with two decimal places (e.g. 1013.25) */
	int32_t hpa_int  = pa / 100;
	int32_t hpa_frac = pa % 100;

	char data[80];
	snprintk(data, sizeof(data),
		 "moisture=%d,humidity=%d,temp=%d,pressure=%d.%02d",
		 moisture, humidity, temp, hpa_int, hpa_frac);

	int err = bt_nus_send(NULL, data, strlen(data));
	if (err) {
		printk("Send failed (err %d)\n", err);
	} else {
		printk("Sent: %s\n", data);
	}
}

static struct bt_nus_cb nus_cb = {
	.received      = nus_received,
	.notif_enabled = nus_notif_enabled,
};

/* ==========================================================================
 * Advertising restart work
 * ========================================================================== */
static void adv_restart_work_fn(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to restart advertising (err %d)\n", err);
	} else {
		printk("Advertising restarted, waiting for mobile...\n");
	}
}

static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_fn);

/* ==========================================================================
 * Connection Callbacks
 * ========================================================================== */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (err %d)", err);
		return;
	}
	printk("Mobile connected, waiting for subscription...\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason 0x%02x) - restarting advertising in 1s...\n", reason);
	k_work_schedule(&adv_restart_work, K_MSEC(1000));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
};

/* ==========================================================================
 * Main
 * ========================================================================== */
int main(void)
{
	int err = bt_nus_cb_register(&nus_cb, NULL);
	if (err) {
		printk("Failed to register NUS callbacks: %d\n", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Failed to enable bluetooth: %d\n", err);
		return err;
	}

	k_sleep(K_SECONDS(3));

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to start advertising: %d\n", err);
		return err;
	}

	LOG_INF("Sensor node advertising, waiting for mobile...");

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
