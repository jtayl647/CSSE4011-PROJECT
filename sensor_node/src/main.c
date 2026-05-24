#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include "sensor_pb.h"
#include "nanopb_types.h"
#include <sensor_info.pb.h>
#include <sensor_config.pb.h>
#include "sensor_lfs.h"
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(sensor_node, LOG_LEVEL_INF);

/* ==========================================================================
 * LED + Pump GPIO
 *   led0 = Red  (automate OFF)
 *   led1 = Green (pump ON)
 *   led0 + led1 = Orange (automate ON, pump OFF)
 *   pump0 = D1 P0.03, HIGH = pump on
 * ========================================================================== */
static const struct gpio_dt_spec led_r  = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g  = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec pump_pin = GPIO_DT_SPEC_GET(DT_ALIAS(pump0), gpios);

/* Config received from mobile node */
static struct k_mutex g_config_mutex;
static bool    g_automate      = false;
static int32_t g_water_period  = 0;   /* seconds to run pump */
static int32_t g_water_trigger = 0;   /* moisture % threshold to trigger watering */
static bool    g_pump_active   = false;

/* --------------------------------------------------------------------------
 * LED helpers — call while NOT holding any mutex (just GPIO ops)
 * -------------------------------------------------------------------------- */
static void led_state_automate_off(void)
{
	/* Red only */
	gpio_pin_set_dt(&led_r, 1);
	gpio_pin_set_dt(&led_g, 0);
}

static void led_state_automate_on(void)
{
	/* Orange = Red + Green */
	gpio_pin_set_dt(&led_r, 1);
	gpio_pin_set_dt(&led_g, 1);
}

static void led_state_pump_on(void)
{
	/* Green only */
	gpio_pin_set_dt(&led_r, 0);
	gpio_pin_set_dt(&led_g, 1);
}

/* --------------------------------------------------------------------------
 * Pump off — scheduled work item fires after water_period seconds
 * -------------------------------------------------------------------------- */
static void pump_off_work_fn(struct k_work *work)
{
	gpio_pin_set_dt(&pump_pin, 0);
	g_pump_active = false;
	printk("Pump OFF\n");

	/* Return LED to orange (automate still on) */
	led_state_automate_on();
}
static K_WORK_DELAYABLE_DEFINE(pump_off_work, pump_off_work_fn);

/* Turn pump on for 'seconds' seconds — no-op if already running */
static void pump_on(int32_t seconds)
{
	if (g_pump_active) {
		return;
	}
	gpio_pin_set_dt(&pump_pin, 1);
	g_pump_active = true;
	printk("Pump ON for %d seconds (moisture below trigger)\n", seconds);
	led_state_pump_on();
	k_work_schedule(&pump_off_work, K_SECONDS(seconds));
}

/* Custom channel for soil moisture percentage */
#define SENSOR_CHAN_SOIL_MOISTURE ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))

#define SENSOR_READ_INTERVAL_S 30
#define SENSOR_THREAD_STACK    2048 * 2
#define FILE_WRITE_THREAD_STACK 2048 * 2
#define SENSOR_THREAD_PRIO     5
#define FILE_WRITE_THREAD_PRIO 6

//Struct which represents the LittleFS architecture
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
	.mnt_point = "/lfs",
};

//Global pointer to the mountpoint
struct fs_mount_t *mountpoint = &lfs_storage_mnt;

//name of the file that past readings are being stored in
static const char *readings_filename = "readings.txt";

//Full path of the file where past readings are being stored
static char readings_path[MAX_PATH_LEN];

//File where past readings are being stored
static struct fs_file_t readings_file;

/* A mutex protects it because the sensor thread writes and the BLE
 * callback reads at the same time. */
static SensorNode g_node = SensorNode_init_zero;
static struct k_mutex g_node_mutex;

//A mutex to guard the file
K_MUTEX_DEFINE(file_mutex);

//A msgq that to send information between a sensor thread and a file writing thread
K_MSGQ_DEFINE(file_write_msgq,
              sizeof(DataReadings),
              10,
              4);

//number of times we have read the sensors
static int num_reads = 0;

/* Set by nus_notif_enabled after a successful BLE send.
 * file_write_thread checks this and truncates the file — keeps flash I/O
 * out of the BLE callback so the connection doesn't time out. */
static atomic_t g_file_needs_clear = ATOMIC_INIT(0);

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

	// printk("Reading added (%d/%d)\n", g_node.readings_count, max);

	k_mutex_unlock(&g_node_mutex);
}

/* Adds a DataReadings struct directly into g_node — called from file_write_thread
 * so g_node stays in sync without any flash I/O in the BLE callback. */
static void add_reading_data(const DataReadings *r)
{
	k_mutex_lock(&g_node_mutex, K_FOREVER);

	pb_size_t max = (pb_size_t)ARRAY_SIZE(g_node.readings);

	if (g_node.readings_count >= max) {
		memmove(&g_node.readings[0], &g_node.readings[1],
			(max - 1) * sizeof(DataReadings));
		g_node.readings_count = max - 1;
	}

	g_node.readings[g_node.readings_count] = *r;
	g_node.readings_count++;

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
		// LOG_INF("Entering Sensor Thread While Loop\n");
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
				// LOG_INF("moisture: %d\n", moisture_val);
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
				// LOG_INF("temp: %d\n", temp_val);
				// LOG_INF("humidity: %d\n", humidity_val);
			}
		}

		/* Pressure from QMP6988 */
		if (device_is_ready(qmp6988)) {
			struct sensor_value pressure;
			if (sensor_sample_fetch(qmp6988) == 0 &&
			    sensor_channel_get(qmp6988, SENSOR_CHAN_PRESS, &pressure) == 0) {
				pressure_val = pressure.val1 * 1000 + pressure.val2 / 1000;
				// LOG_INF("pressure: %d\n", pressure_val);
			}
		}

		//make a DataReadings struct
		DataReadings reading = DataReadings_init_zero;

		//Put the values into the struct
		reading.temp = temp_val;
		reading.humidity = humidity_val;
		reading.pressure = pressure_val;
		reading.moisture = moisture_val;
		reading.meas_time = (int32_t)k_uptime_get_32();

		//send the readings to the writing thread
		k_msgq_put(&file_write_msgq, &reading, K_NO_WAIT);

		/* Always save — missing sensors just contribute 0 */
		// add_reading(temp_val, humidity_val, pressure_val, moisture_val);

		/* Check if automate is on and moisture is below trigger threshold */
		k_mutex_lock(&g_config_mutex, K_FOREVER);
		bool   automate = g_automate;
		int32_t trigger = g_water_trigger;
		int32_t period  = g_water_period;
		k_mutex_unlock(&g_config_mutex);

		if (automate && moisture_val < trigger) {
			printk("Moisture %d%% < trigger %d%% — activating pump\n",
			       moisture_val, trigger);
			pump_on(period);
		}

		k_sleep(K_SECONDS(SENSOR_READ_INTERVAL_S));
	}
}

K_THREAD_DEFINE(sensor_thread, SENSOR_THREAD_STACK,
		sensor_thread_fn, NULL, NULL, NULL,
		SENSOR_THREAD_PRIO, 0, 0);


/*=============================================================
 * File Writing Thread - writes past sensor readings to a file
 * and deletes old data if too much information has been stored
 ==============================================================*/
 
static void file_write_thread_fn(void *a, void *b, void *c) {
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	DataReadings to_write = DataReadings_init_zero;

	while(1) {

		/* BLE send completed — truncate file here (not in BLE callback) */
		if (atomic_cas(&g_file_needs_clear, 1, 0)) {
			k_mutex_lock(&file_mutex, K_FOREVER);
			int cerr = sensor_lfs_file_truncate((void*)(&readings_file), readings_path);
			if (cerr < 0) {
				LOG_ERR("Failed to truncate readings file after BLE send\n");
			} else {
				num_reads = 0;
				printk("Readings file cleared after send\n");
			}
			k_mutex_unlock(&file_mutex);
		}

		if (num_reads >= CONFIG_NUM_READINGS) {
			// LOG_INF("Entering File Reset Block\n");
			//lock the mutex for the file
			k_mutex_lock(&file_mutex, K_FOREVER);

			//truncate the file
			int err = sensor_lfs_file_truncate((void*)(&readings_file), readings_path);
			if (err < 0) {
				LOG_ERR("Sensor Thread failed to truncate readings file\n");
			}

			k_mutex_unlock(&file_mutex);
			num_reads = 0;
			// LOG_INF("Exiting File Rest Block\n");

		}

		//wait on the queue
		k_msgq_get(&file_write_msgq, &to_write, K_FOREVER);

		//write this information into the file
		k_mutex_lock(&file_mutex, K_FOREVER);
		int err = sensor_lfs_write_to_file((void*)(&readings_file), readings_path, (void*)(&to_write));
		if (err < 0) {
			LOG_ERR("Write thread failed to write to file\n");
		} else {
			printk("Reading saved (%d/%d) — temp=%d humidity=%d moisture=%d pressure=%d measured_time=%d\n",
			       num_reads + 1, CONFIG_NUM_READINGS,
			       to_write.temp, to_write.humidity, to_write.moisture, to_write.pressure, to_write.meas_time);
		}
		k_mutex_unlock(&file_mutex);

		/* Keep g_node in sync so BLE callback never needs to read the file */
		add_reading_data(&to_write);

		//increase the number of times we have sampled data
		num_reads++;
	}
}

K_THREAD_DEFINE(file_write_thread, FILE_WRITE_THREAD_STACK,
		file_write_thread_fn, NULL, NULL, NULL,
		FILE_WRITE_THREAD_PRIO, 0, 0);

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

	/* Apply config: store values and update LED state */
	k_mutex_lock(&g_config_mutex, K_FOREVER);
	g_automate      = config.automate;
	g_water_period  = config.water_period;
	g_water_trigger = config.water_trigger;
	k_mutex_unlock(&g_config_mutex);

	/* Update LED — don't change if pump is already running */
	if (!g_pump_active) {
		if (config.automate) {
			led_state_automate_on();   /* Orange */
		} else {
			led_state_automate_off();  /* Red */
		}
	}
}

static void nus_notif_enabled(bool enabled, void *ctx)
{
	if (!enabled) {
		return;
	}

	printk("Mobile subscribed - sending sensor data\n");

	/* Only lock g_node_mutex — no flash I/O here.
	 * g_node is kept up to date by file_write_thread via add_reading_data(). */
	k_mutex_lock(&g_node_mutex, K_FOREVER);

	/* Stamp the time this connection happened so the mobile node knows
	 * the uptime reference point for all the meas_time values */
	g_node.ble_time = (int32_t)k_uptime_get();

	/* static so it lives in BSS not on the BLE callback stack (1387 bytes) */
	static uint8_t buf[SensorNode_size];
	size_t  len = 0;

	int ret = sensor_encode(buf, sizeof(buf), &len, SENSOR_NODE, &g_node);
	int sent_count = (int)g_node.readings_count;
	if (ret == 0) {
		g_node.readings_count = 0;
		/* Signal file_write_thread to truncate the file (no flash I/O here) */
		atomic_set(&g_file_needs_clear, 1);
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
	printk("=== Sensor Node Starting ===\n");

	/* Initialise mutexes */
	k_mutex_init(&g_node_mutex);
	k_mutex_init(&g_config_mutex);

	/* Configure LEDs and pump GPIO */
	gpio_pin_configure_dt(&led_r,    GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_g,    GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&pump_pin, GPIO_OUTPUT_INACTIVE);

	/* Start with Red — automate off until config received */
	led_state_automate_off();

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

	//Set up the file system
	int f_err = sensor_lfs_init((void*)mountpoint, (void*)(&readings_file), readings_filename, readings_path);
	if (f_err < 0) {
		printk("Failed to initialise the LittleFS\n");
		return f_err;
	}


	bt_addr_le_t addr;
	size_t count = 1;
	bt_id_get(&addr, &count);
	memcpy(g_node.mac_address, addr.a.val, sizeof(g_node.mac_address));

	//transfer anything that was on the file onto g_node
	k_mutex_lock(&file_mutex, K_FOREVER);
	k_mutex_lock(&g_node_mutex, K_FOREVER);

	int ret = sensor_lfs_read_from_file(&readings_file, readings_path, &g_node);
	if (ret < 0) {
		printk("Error when reading readings file to g_node from boot\n");
	}

	//truncate after reading from the stored stuff
	int cerr = sensor_lfs_file_truncate((void*)(&readings_file), readings_path);
	if (cerr < 0) {
		LOG_ERR("Failed to truncate readings file after boot\n");
	}


	k_mutex_unlock(&g_node_mutex);
	k_mutex_unlock(&file_mutex);

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