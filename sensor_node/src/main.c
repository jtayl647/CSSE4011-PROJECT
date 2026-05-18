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
#include <sensor_info.pb.h>
#include <sensor_config.pb.h>

LOG_MODULE_REGISTER(sensor_node, LOG_LEVEL_INF);

/* Custom channel for soil moisture percentage */
#define SENSOR_CHAN_SOIL_MOISTURE ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))

#define SENSOR_READ_INTERVAL_S 30
#define SENSOR_THREAD_STACK    2048
#define SENSOR_THREAD_PRIO     5

/* A mutex protects it because the sensor thread writes and the BLE
 * callback reads at the same time. */
static SensorNode g_node = SensorNode_init_zero;
static struct k_mutex g_node_mutex;

/* If the readings array is already full (max 24 set by CONFIG_NUM_READINGS),
 * the oldest entry is dropped by shifting everything left so we always
 * keep the most recent data rather than silently discarding new readings. */
static void add_reading(int32_t temp, int32_t humidity,
			int32_t pressure, int32_t moisture)
{
	k_mutex_lock(&g_node_mutex, K_FOREVER);

	pb_size_t max = (pb_size_t)ARRAY_SIZE(g_node.readings);

	if (g_node.readings_count >= max) {
		/* Full — shift left to drop oldest, safe because we own the mutex */
		memmove(&g_node.readings[0], &g_node.readings[1],
			(max - 1) * sizeof(DataReadings));
		g_node.readings_count = max - 1;
	}

	pb_size_t i = g_node.readings_count;
	g_node.readings[i].temp      = temp;
	g_node.readings[i].humidity  = humidity;
	g_node.readings[i].pressure  = pressure;
	g_node.readings[i].moisture  = moisture;
	g_node.readings[i].meas_time = (int32_t)k_uptime_get_32();
	g_node.readings_count++;

	printk("Reading added (%d/%d)\n", g_node.readings_count, max);

	k_mutex_unlock(&g_node_mutex);
}

/* ==========================================================================
 * Sensor Thread — reads all sensors every 30 seconds
 * ========================================================================== */
static void sensor_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	k_sleep(K_SECONDS(SENSOR_READ_INTERVAL_S));

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
		/* Values default to 0 — if a sensor is missing its field
		 * is just 0 in the reading rather than blocking the whole save */
		int32_t moisture_val = 0;
		int32_t temp_val     = 0;
		int32_t humidity_val = 0;
		int32_t pressure_val = 0;

		/* Soil moisture */
		if (device_is_ready(soil)) {
			struct sensor_value moisture;
			if (sensor_sample_fetch(soil) == 0 &&
			    sensor_channel_get(soil, SENSOR_CHAN_SOIL_MOISTURE, &moisture) == 0) {
				moisture_val = moisture.val1;
			}
		}

		/* Temperature + humidity from SHT30 */
		if (device_is_ready(sht30)) {
			struct sensor_value temp, humidity;
			if (sensor_sample_fetch(sht30) == 0) {
				sensor_channel_get(sht30, SENSOR_CHAN_AMBIENT_TEMP, &temp);
				sensor_channel_get(sht30, SENSOR_CHAN_HUMIDITY,     &humidity);
				temp_val     = temp.val1 * 100 + temp.val2 / 10000;
				humidity_val = humidity.val1;
			}
		}

		/* Pressure from QMP6988 */
		if (device_is_ready(qmp6988)) {
			struct sensor_value pressure;
			if (sensor_sample_fetch(qmp6988) == 0 &&
			    sensor_channel_get(qmp6988, SENSOR_CHAN_PRESS, &pressure) == 0) {
				pressure_val = pressure.val1 * 1000 + pressure.val2 / 1000;
			}
		}

		/* Always save — missing sensors just contribute 0 */
		add_reading(temp_val, humidity_val, pressure_val, moisture_val);

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
	/* Decode incoming bytes as a SensorConfig protobuf from the mobile node */
	SensorConfig config = SensorConfig_init_zero;

	int ret = sensor_decode_function((uint8_t *)data, len, SENSOR_CONFIG, &config);
	if (ret != 0) {
		LOG_ERR("Failed to decode SensorConfig");
		return;
	}

	printk("Config received — automate=%d water_period=%d water_trigger=%d\n",
	       config.automate, config.water_period, config.water_trigger);

	/* TODO: apply config (start/stop pump, set thresholds, etc.) */
}

static void nus_notif_enabled(bool enabled, void *ctx)
{
	if (!enabled) {
		return;
	}

	printk("Mobile subscribed - sending sensor data\n");

	k_mutex_lock(&g_node_mutex, K_FOREVER);

	/* static so it lives in BSS not on the BLE callback stack (1387 bytes) */
	static uint8_t buf[SensorNode_size];
	size_t  len = 0;

	int ret = sensor_encode(buf, sizeof(buf), &len, SENSOR_NODE, &g_node);
	int sent_count = (int)g_node.readings_count;
	if (ret == 0) {
		g_node.readings_count = 0;
	}

	k_mutex_unlock(&g_node_mutex);

	if (ret != 0) {
		printk("Encode failed\n");
		return;
	}

	int err = bt_nus_send(NULL, buf, len);
	if (err) {
		printk("Send failed (err %d)\n", err);
	} else {
		printk("Sent %d bytes (%d readings)\n", (int)len, sent_count);
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
	/* CHANGED: initialise the mutex that guards g_node */
	k_mutex_init(&g_node_mutex);

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

	bt_addr_le_t addr;
	size_t count = 1;
	bt_id_get(&addr, &count);
	memcpy(g_node.mac_address, addr.a.val, sizeof(g_node.mac_address));

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
