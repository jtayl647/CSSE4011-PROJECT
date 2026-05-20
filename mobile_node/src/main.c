#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <string.h>
//#include "../build/sensor_info.pb.h"
//#include "../build/sensor_config.pb.h"
#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include "mobile_pb.h"
#include "nanopb_types.h"
#include "mobile_lfs.h"
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(mobile_node, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const char* SENSOR_NAME = "SensorNode";
static const char* BASE_NAME = "BaseNode";

static bool sensor_seen = false;
static bool base_seen = false;

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
static const char *sensor_nodes = "sensor_readings.txt";

//name of the file that configs are being stored in
static const char *sensor_configs = "sensor_configs.txt";

//Full path of the file where past readings are being stored
static char sensor_nodes_path[MAX_PATH_LEN];

//Full path of the file where sensor configurations are saved
static char sensor_configs_path[MAX_PATH_LEN];

//File where past readings are being stored
static struct fs_file_t sensor_nodes_file;

//File where sensor configurations are being stored
static struct fs_file_t sensor_configs_file;

//Mutex to protect writing and reading to the sensor nodes file
K_MUTEX_DEFINE(sensor_node_file_mutex);

//Mutex to protect writing and reading to the configs file
K_MUTEX_DEFINE(sensor_configs_file_mutex);


/* ==========================================================================
 * Sensor name filter list — add names here to connect to more nodes
 * ========================================================================== */
static const char *const target_names[] = {
	"SensorNode",
	/* "SensorNode2", */
	/* "GardenNode",  */
};

static bool name_in_list(const uint8_t *data, uint8_t data_len)
{
	for (int i = 0; i < ARRAY_SIZE(target_names); i++) {
		if (data_len == strlen(target_names[i]) &&
		    memcmp(data, target_names[i], data_len) == 0) {
			return true;
		}
	}
	return false;
}

static struct bt_conn *default_conn;

#define RETRY_DELAY_MS 30000

/* Forward declarations */
static void start_scan(void);
static void discover_nus_tx(struct bt_conn *conn);
static void discover_nus_rx(struct bt_conn *conn);

static void scan_retry_work_fn(struct k_work *work)
{
	printk("Retrying scan...\n");
	start_scan();
}

static K_WORK_DELAYABLE_DEFINE(scan_retry_work, scan_retry_work_fn);

/* ==========================================================================
 * NUS TX Subscription + RX Write
 *
 * Discovery chain:
 *   MTU exchange → TX discover → CCC discover → subscribe
 *                             → RX discover → (ready to send config)
 *
 * On notification received:
 *   notify_func → bt_gatt_write(config) → write_done → bt_conn_disconnect
 * ========================================================================== */
static struct bt_gatt_subscribe_params subscribe_params;
static struct bt_gatt_discover_params  ccc_discover_params;
static struct bt_gatt_discover_params  tx_discover_params;
static struct bt_gatt_discover_params  rx_discover_params;
static struct bt_uuid_128              tx_discover_uuid;
static struct bt_uuid_128              rx_discover_uuid;
static uint16_t                        nus_tx_handle;
static uint16_t                        nus_rx_handle;
static struct bt_gatt_exchange_params  exchange_params;
static struct bt_gatt_write_params     write_params;

/* Encoded config buffer — filled once in notify_func, sent in write_done */
static uint8_t config_buf[SensorConfig_size];
static size_t  config_buf_len;

/* ------------------------------------------------------------------
 * Write callback — fires after sensor ACKs the config write.
 * ------------------------------------------------------------------ */
static void write_done(struct bt_conn *conn, uint8_t err,
		       struct bt_gatt_write_params *params)
{
	if (err) {
		printk("Config write failed (err %d)\n", err);
	} else {
		printk("Config sent successfully\n");
	}

	printk("Disconnecting...\n");
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/* ------------------------------------------------------------------
 * Notification callback — decodes incoming SensorNode, then sends
 * an encoded SensorConfig back.
 * ------------------------------------------------------------------ */
static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	printk("In the notify function\n");
	if (!data) {
		LOG_INF("Unsubscribed\n");
		return BT_GATT_ITER_STOP;
	}

	int ret;

	if (sensor_seen) {
		/* --- decode incoming SensorNode --- */
		SensorNode node = SensorNode_init_zero;
		ret = mobile_decode((uint8_t *)data, length, SENSOR_NODE, &node);
		if (ret == 0) {


			LOG_INF("MAC: %02x:%02x:%02x:%02x:%02x:%02x  ble_time=%d ms\n",
				node.mac_address[5], node.mac_address[4],
				node.mac_address[3], node.mac_address[2],
				node.mac_address[1], node.mac_address[0],
				node.ble_time);

			for (int i = 0; i < node.readings_count; i++) {
				DataReadings *r = &node.readings[i];
				LOG_INF("[%d] temp=%d.%02d humidity=%d moisture=%d pressure=%d.%02d meas_time=%d ms\n",
					i,
					r->temp / 100, r->temp % 100,
					r->humidity,
					r->moisture,
					r->pressure / 100, r->pressure % 100,
					r->meas_time);
				}
		} else {
			LOG_INF("Failed to decode SensorNode\n");
		}

		//Try writing the SensorNode to a file
		k_mutex_lock(&sensor_node_file_mutex, K_FOREVER);
		ret = mobile_lfs_write_sensor_readings(&sensor_nodes_file, sensor_nodes_path, &node);
		if (ret < 0) {
			LOG_ERR("Failed to write properly to the Sensor Readings file\n");
		}
		k_mutex_unlock(&sensor_node_file_mutex);

		//instantly try and read what I just wrote
		Nodes nodes = Nodes_init_zero;
		//Try reading the file that I just wrote to
		k_mutex_lock(&sensor_node_file_mutex, K_FOREVER);
		ret = mobile_lfs_read_sensor_readings(&sensor_nodes_file, sensor_nodes_path, &nodes);
		if (ret < 0) {
			LOG_ERR("Failed to read properly from the sensor file\n");
		}
		k_mutex_unlock(&sensor_node_file_mutex);

		//testing what is in Nodes
		int sensor_num = nodes.nodes_count;
		LOG_INF("Number of sensors read from file: %d\n", sensor_num);

		for (int i = 0; i < sensor_num; i++) {
			//grab the current sensor node
			SensorNode node = nodes.nodes[i];
			//print the mac address of the node gotten
			LOG_INF("MAC Adress Read from file: %d:%d:%d:%d:%d:%d",
				node.mac_address[0],
				node.mac_address[1],
				node.mac_address[2],
				node.mac_address[3],
				node.mac_address[4],
				node.mac_address[5]
			);
			LOG_INF("BLE time read: %d\n", node.ble_time);
			//grab the number of readings for this node
			int num_readings = node.readings_count;

			for (int j = 0; j < num_readings; j++) {
				DataReadings data = node.readings[j];
				LOG_INF("temp read: %d\n", data.temp);
				LOG_INF("humidity read: %d\n", data.humidity);
				LOG_INF("pressure read: %d\n", data.pressure);
				LOG_INF("moisture read: %d\n", data.moisture);
				LOG_INF("meas_time read: %d\n", data.meas_time);
			}
		}

	}
	////////////// Testing for writing to the SensorReadings file ////////////

	if (nus_rx_handle == 0) {
		printk("RX handle not ready, disconnecting\n");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}

	/* --- encode a test SensorConfig to send back --- */
	SensorConfig config = SensorConfig_init_zero;
	config.automate      = true;
	config.water_period  = 10;   /* seconds */
	config.water_trigger = 90;   /* moisture % threshold */

	config_buf_len = 0;
	ret = mobile_encode(config_buf, sizeof(config_buf),
			    &config_buf_len, SENSOR_CONFIG, &config);
	if (ret != 0) {
		printk("Failed to encode SensorConfig\n");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}

	/* Write config to sensor RX; disconnect in write_done */
	write_params.func   = write_done;
	write_params.handle = nus_rx_handle;
	write_params.offset = 0;
	write_params.data   = config_buf;
	write_params.length = config_buf_len;

	int err = bt_gatt_write(conn, &write_params);
	if (err) {
		printk("Config write failed (err %d), disconnecting\n", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	return BT_GATT_ITER_STOP;
}

/* ------------------------------------------------------------------
 * CCC discovery — subscribe to TX notifications, then find RX handle
 * ------------------------------------------------------------------ */
static uint8_t ccc_discover_func(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr,
				  struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("CCC not found\n");
		return BT_GATT_ITER_STOP;
	}

	subscribe_params.notify       = notify_func;
	subscribe_params.value        = BT_GATT_CCC_NOTIFY;
	subscribe_params.ccc_handle   = attr->handle;
	subscribe_params.value_handle = nus_tx_handle;

	int err = bt_gatt_subscribe(conn, &subscribe_params);
	if (err && err != -EALREADY) {
		printk("Subscribe failed (err %d)\n", err);
	} else {
		printk("Subscribed to sensor notifications\n");
	}

	return BT_GATT_ITER_STOP;
}

/* ------------------------------------------------------------------
 * TX characteristic discovery
 * ------------------------------------------------------------------ */
static uint8_t tx_discover_func(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("NUS TX not found\n");
		return BT_GATT_ITER_STOP;
	}

	nus_tx_handle = bt_gatt_attr_value_handle(attr);
	printk("NUS TX handle: %u - discovering CCC...\n", nus_tx_handle);

	static struct bt_uuid_16 ccc_uuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

	ccc_discover_params.uuid         = &ccc_uuid.uuid;
	ccc_discover_params.func         = ccc_discover_func;
	ccc_discover_params.start_handle = nus_tx_handle + 1;
	ccc_discover_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	ccc_discover_params.type         = BT_GATT_DISCOVER_DESCRIPTOR;

	int err = bt_gatt_discover(conn, &ccc_discover_params);
	if (err) {
		printk("CCC discover failed (err %d)\n", err);
	}

	return BT_GATT_ITER_STOP;
}

static void discover_nus_tx(struct bt_conn *conn)
{
	memcpy(&tx_discover_uuid,
	       BT_UUID_DECLARE_128(BT_UUID_NUS_TX_CHAR_VAL),
	       sizeof(tx_discover_uuid));

	tx_discover_params.uuid         = &tx_discover_uuid.uuid;
	tx_discover_params.func         = tx_discover_func;
	tx_discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	tx_discover_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	tx_discover_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;

	int err = bt_gatt_discover(conn, &tx_discover_params);
	if (err) {
		printk("TX discover failed (err %d)\n", err);
	}
}

/* ------------------------------------------------------------------
 * RX characteristic discovery — store handle, then kick off TX discovery
 * ------------------------------------------------------------------ */
static uint8_t rx_discover_func(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("NUS RX not found\n");
	} else {
		nus_rx_handle = bt_gatt_attr_value_handle(attr);
		printk("NUS RX handle: %u\n", nus_rx_handle);
	}

	/* Always continue to TX discovery regardless */
	discover_nus_tx(conn);

	return BT_GATT_ITER_STOP;
}

static void discover_nus_rx(struct bt_conn *conn)
{
	memcpy(&rx_discover_uuid,
	       BT_UUID_DECLARE_128(BT_UUID_NUS_RX_CHAR_VAL),
	       sizeof(rx_discover_uuid));

	rx_discover_params.uuid         = &rx_discover_uuid.uuid;
	rx_discover_params.func         = rx_discover_func;
	rx_discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	rx_discover_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	rx_discover_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;

	int err = bt_gatt_discover(conn, &rx_discover_params);
	if (err) {
		printk("RX discover failed (err %d), trying TX anyway\n", err);
		discover_nus_tx(conn);
	}
}

/* ------------------------------------------------------------------
 * MTU exchange callback — discover RX first, then TX
 * ------------------------------------------------------------------ */
static void exchange_func(struct bt_conn *conn, uint8_t att_err,
			   struct bt_gatt_exchange_params *params)
{
	if (att_err) {
		printk("MTU exchange failed (err %d)\n", att_err);
	} else {
		printk("MTU exchanged: %u\n", bt_gatt_get_mtu(conn));
	}
	discover_nus_rx(conn);
}

/* ==========================================================================
 * Scan Callback
 * ========================================================================== */
static void device_found(const bt_addr_le_t *addr, int8_t rssi,
			 uint8_t type, struct net_buf_simple *buf)
{
	if (default_conn) {
		return;
	}

	/* Name is in ADV_IND — ignore other packet types */
	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	struct net_buf_simple_state state;
	net_buf_simple_save(buf, &state);
	sensor_seen = false;
	base_seen = false;

	while (buf->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(buf);
		if (!len || len > buf->len) break;
		uint8_t ad_type = net_buf_simple_pull_u8(buf);
		if ((ad_type == BT_DATA_NAME_COMPLETE ||
		    ad_type == BT_DATA_NAME_SHORTENED)) {
			int data_len = len - 1;
			if (data_len == strlen(SENSOR_NAME) &&
				memcmp(buf->data, SENSOR_NAME, data_len) == 0) {
				sensor_seen = true;
				LOG_INF("Sensor Type Detected\n");
			}
			if (data_len == strlen(BASE_NAME) &&
				memcmp(buf->data, BASE_NAME, data_len) == 0) {
				base_seen = true;
			}
		}
		net_buf_simple_pull(buf, len - 1);
	}

	net_buf_simple_restore(buf, &state);

	if (!base_seen && !sensor_seen) {
		//LOG_INF("In suspicious return block\n");
		return;
	}

	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Found sensor node [%s] RSSI=%d — connecting...\n", addr_str, rssi);

	bt_le_scan_stop();

	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				    BT_LE_CONN_PARAM_DEFAULT, &default_conn);
	if (err) {
		printk("Connect failed (err %d), retrying in 30s...\n", err);
		k_work_schedule(&scan_retry_work, K_MSEC(RETRY_DELAY_MS));
	}
}

/* ==========================================================================
 * Start Scan
 * ========================================================================== */
static void start_scan(void)
{
	struct bt_le_scan_param scan_param = {
		.type     = BT_LE_SCAN_TYPE_PASSIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
	};

	int err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Scan start failed (err %d)\n", err);
	} else {
		printk("Scanning for SensorNode...\n");
	}
}

/* ==========================================================================
 * Connection Callbacks
 * ========================================================================== */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("Connect failed to %s (err %d), retrying in 30s...\n", addr, err);
		bt_conn_unref(default_conn);
		default_conn = NULL;
		k_work_schedule(&scan_retry_work, K_MSEC(RETRY_DELAY_MS));
		return;
	}

	printk("Connected to %s\n", addr);

	/* Reset handles from any previous connection */
	nus_tx_handle = 0;
	nus_rx_handle = 0;

	/* Negotiate larger ATT MTU first, then discover and subscribe */
	exchange_params.func = exchange_func;
	int mtu_err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (mtu_err) {
		printk("MTU exchange start failed (err %d), discovering anyway\n", mtu_err);
		discover_nus_tx(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s (reason 0x%02x)\n", addr, reason);

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn  = NULL;
	nus_tx_handle = 0;
	nus_rx_handle = 0;

	printk("Scanning again in 30s...\n");
	k_work_schedule(&scan_retry_work, K_MSEC(RETRY_DELAY_MS));
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
	int err = bt_enable(NULL);
	if (err) {
		printk("Failed to enable bluetooth: %d\n", err);
		return err;
	}

	k_sleep(K_SECONDS(3));

	//Initialise the LittleFS architecture
	err = mobile_lfs_init(mountpoint);
	if (err < 0) {
		LOG_ERR("Failed to mount the Mobile Node LittleFS\n");
		return err;
	}

	//Initialise the configs file
	err = mobile_lfs_file_init(mountpoint, &sensor_configs_file, sensor_configs, sensor_configs_path);
	if (err < 0) {
		LOG_ERR("Failed to init the %s\n", sensor_configs);
		return err;
	}

	//initialise the readings file
	err = mobile_lfs_file_init(mountpoint, &sensor_nodes_file, sensor_nodes, sensor_nodes_path);
	if (err < 0) {
		LOG_ERR("Failed to init the %s\n", sensor_nodes);
		return err;
	}

	printk("Mobile node started\n");

	start_scan();

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
