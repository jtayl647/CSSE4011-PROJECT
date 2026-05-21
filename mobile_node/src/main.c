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
 * LFS write thread — decouples flash I/O from the BLE callback.
 * notify_func puts decoded SensorNode structs here; the thread writes them.
 * ========================================================================== */
K_MSGQ_DEFINE(lfs_write_msgq, sizeof(SensorNode), 4, 4);

static void lfs_write_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	SensorNode node;

	while (1) {
		k_msgq_get(&lfs_write_msgq, &node, K_FOREVER);

		/* Write to flash */
		k_mutex_lock(&sensor_node_file_mutex, K_FOREVER);
		int ret = mobile_lfs_write_sensor_readings(&sensor_nodes_file,
							   sensor_nodes_path, &node);
		if (ret < 0) {
			LOG_ERR("Failed to write sensor readings to file\n");
		}
		k_mutex_unlock(&sensor_node_file_mutex);

		/* Read back and print (debug) */
		static Nodes nodes;
		nodes = (Nodes)Nodes_init_zero;

		k_mutex_lock(&sensor_node_file_mutex, K_FOREVER);
		ret = mobile_lfs_read_sensor_readings(&sensor_nodes_file,
						      sensor_nodes_path, &nodes);
		if (ret < 0) {
			LOG_ERR("Failed to read sensor readings from file\n");
		}
		k_mutex_unlock(&sensor_node_file_mutex);

		LOG_INF("Sensors in file: %d\n", nodes.nodes_count);
		for (int i = 0; i < nodes.nodes_count; i++) {
			SensorNode *n = &nodes.nodes[i];
			LOG_INF("MAC: %d:%d:%d:%d:%d:%d  ble_time=%d",
				n->mac_address[0], n->mac_address[1],
				n->mac_address[2], n->mac_address[3],
				n->mac_address[4], n->mac_address[5],
				n->ble_time);
			for (int j = 0; j < n->readings_count; j++) {
				DataReadings *r = &n->readings[j];
				LOG_INF("  [%d] temp=%d humidity=%d moisture=%d "
					"pressure=%d meas_time=%d",
					j, r->temp, r->humidity,
					r->moisture, r->pressure, r->meas_time);
			}
		}
	}
}

K_THREAD_DEFINE(lfs_write_thread, 4096 * 2,
		lfs_write_thread_fn, NULL, NULL, NULL,
		7, 0, 0);

// /* ==========================================================================
//  * Sensor name filter list — add names here to connect to more nodes
//  * ========================================================================== */
// static const char *const target_names[] = {
// 	"SensorNode",
// 	/* "SensorNode2", */
// 	/* "GardenNode",  */
// };

// static bool name_in_list(const uint8_t *data, uint8_t data_len)
// {
// 	for (int i = 0; i < ARRAY_SIZE(target_names); i++) {
// 		if (data_len == strlen(target_names[i]) &&
// 		    memcmp(data, target_names[i], data_len) == 0) {
// 			return true;
// 		}
// 	}
// 	return false;
// }

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

	/* ------------------------------------------------------------------
	 * Base node path — send "hello from mobile" then disconnect
	 * ------------------------------------------------------------------ */
	if (base_seen) {
		if (nus_rx_handle == 0) {
			printk("Base RX handle not ready, disconnecting\n");
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			return BT_GATT_ITER_STOP;
		}

		static const uint8_t hello_msg[] = "hello from mobile";

		write_params.func   = write_done;
		write_params.handle = nus_rx_handle;
		write_params.offset = 0;
		write_params.data   = hello_msg;
		write_params.length = sizeof(hello_msg) - 1; /* exclude null terminator */

		int err = bt_gatt_write(conn, &write_params);
		if (err) {
			printk("Base write failed (err %d), disconnecting\n", err);
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
		return BT_GATT_ITER_STOP;
	}

	/* ------------------------------------------------------------------
	 * Sensor node path — decode readings, then send config back
	 * ------------------------------------------------------------------ */
	if (sensor_seen) {
		/* Decode — CPU only, safe in BLE callback */
		SensorNode node = SensorNode_init_zero;
		ret = mobile_decode((uint8_t *)data, length, SENSOR_NODE, &node);
		if (ret == 0) {
			LOG_INF("MAC: %02x:%02x:%02x:%02x:%02x:%02x  ble_time=%d ms",
				node.mac_address[5], node.mac_address[4],
				node.mac_address[3], node.mac_address[2],
				node.mac_address[1], node.mac_address[0],
				node.ble_time);

			for (int i = 0; i < node.readings_count; i++) {
				DataReadings *r = &node.readings[i];
				LOG_INF("[%d] temp=%d.%02d humidity=%d moisture=%d "
					"pressure=%d.%02d meas_time=%d ms",
					i,
					r->temp / 100, r->temp % 100,
					r->humidity, r->moisture,
					r->pressure / 100, r->pressure % 100,
					r->meas_time);
			}

			/* Hand off to flash thread — no flash I/O here */
			if (k_msgq_put(&lfs_write_msgq, &node, K_NO_WAIT) != 0) {
				LOG_WRN("LFS write queue full — dropping reading\n");
			}
		} else {
			LOG_ERR("Failed to decode SensorNode\n");
		}

		if (nus_rx_handle == 0) {
			printk("Sensor RX handle not ready, disconnecting\n");
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			return BT_GATT_ITER_STOP;
		}

		/* Encode SensorConfig and write back to sensor node */
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
	err = file_init(mountpoint, &sensor_configs_file, sensor_configs, sensor_configs_path);
	if (err < 0) {
		LOG_ERR("Failed to init the %s\n", sensor_configs);
		return err;
	}

	//initialise the readings file
	err = file_init(mountpoint, &sensor_nodes_file, sensor_nodes, sensor_nodes_path);
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
