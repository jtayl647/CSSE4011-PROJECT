#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <string.h>
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
#define FILE_WRITE_THREAD_STACK (4096 * 4)
#define FILE_WRITE_THREAD_PRIO  6

static const char *SENSOR_NAME = "SensorNode";
static const char *BASE_NAME   = "BaseNode";

static bool sensor_seen = false;
static bool base_seen   = false;

/* ==========================================================================
 * LittleFS
 * ========================================================================== */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
	.type        = FS_LITTLEFS,
	.fs_data     = &storage,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
	.mnt_point   = "/lfs",
};

struct fs_mount_t *mountpoint = &lfs_storage_mnt;

static const char *sensor_nodes   = "sensor_readings.txt";
static const char *sensor_configs = "sensor_configs.txt";

static char sensor_nodes_path[MAX_PATH_LEN];
static char sensor_configs_path[MAX_PATH_LEN];

static struct fs_file_t sensor_nodes_file;
static struct fs_file_t sensor_configs_file;

K_MUTEX_DEFINE(sensor_node_file_mutex);
K_MUTEX_DEFINE(sensor_configs_file_mutex);
K_MUTEX_DEFINE(shared_sensor_mutex);

K_SEM_DEFINE(config_read_sem, 0, 1);

/* ==========================================================================
 * Inter-thread message types
 * ========================================================================== */
struct SensorRx {
	uint8_t  encoded[255];
	uint16_t length;
};

struct SensorTx {
	uint8_t  encoded[255];
	uint16_t length;
};

K_MSGQ_DEFINE(readings_writing_msgq,   sizeof(struct SensorRx), 4, 4);
K_MSGQ_DEFINE(config_read_msgq,        sizeof(uint8_t[CONFIG_MAC_BYTES]), 4, 4);
K_MSGQ_DEFINE(sensor_config_send_msgq, sizeof(struct SensorTx), 4, 4);

/* ==========================================================================
 * Thread: decode raw RX bytes → write readings to flash → push MAC to
 *         config_read_msgq
 * ========================================================================== */
static void readings_file_write_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct SensorRx sensor_rx;

	while (1) {
		k_msgq_get(&readings_writing_msgq, &sensor_rx, K_FOREVER);

		int ret;

		SensorNode node = SensorNode_init_zero;
		ret = mobile_decode(sensor_rx.encoded, sensor_rx.length, SENSOR_NODE, &node);

		k_mutex_lock(&sensor_node_file_mutex, K_FOREVER);
		ret = mobile_lfs_write_sensor_readings(&sensor_nodes_file, sensor_nodes_path, &node);
		if (ret < 0) {
			printk("Failed to write to sensor readings file\n");
		}
		k_mutex_unlock(&sensor_node_file_mutex);

		ret = k_msgq_put(&config_read_msgq, node.mac_address, K_NO_WAIT);
		if (ret != 0) {
			printk("config_read_msgq put failed: %d\n", ret);
		}

		/* Read back and print for debug */
		Nodes nodes = Nodes_init_zero;
		k_mutex_lock(&sensor_node_file_mutex, K_FOREVER);
		ret = mobile_lfs_read_sensor_readings(&sensor_nodes_file, sensor_nodes_path, &nodes);
		if (ret < 0) {
			printk("Failed to read sensor readings file\n");
		}
		k_mutex_unlock(&sensor_node_file_mutex);

		for (int i = 0; i < nodes.nodes_count; i++) {
			SensorNode *n = &nodes.nodes[i];
			printk("MAC: %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX  ble_time=%d\n",
				n->mac_address[0], n->mac_address[1],
				n->mac_address[2], n->mac_address[3],
				n->mac_address[4], n->mac_address[5],
				n->ble_time);
			for (int j = 0; j < n->readings_count; j++) {
				DataReadings *r = &n->readings[j];
				printk("  [%d] temp=%d humidity=%d pressure=%d moisture=%d meas_time=%d\n",
					j, r->temp, r->humidity, r->pressure, r->moisture, r->meas_time);
			}
		}

		k_sleep(K_MSEC(1));
	}
}

K_THREAD_DEFINE(readings_file_write_thread, FILE_WRITE_THREAD_STACK,
		readings_file_write_thread_fn, NULL, NULL, NULL,
		FILE_WRITE_THREAD_PRIO, 0, 0);

/* ==========================================================================
 * Thread: read config for the MAC that just connected → encode → push to
 *         sensor_config_send_msgq so notify_func can send it back
 * ========================================================================== */
static void configs_file_read_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct SensorTx sensor_tx;

	while (1) {
		int ret;

		SensorConfig config = SensorConfig_init_zero;

		k_msgq_get(&config_read_msgq, config.mac_address, K_FOREVER);

		k_mutex_lock(&sensor_configs_file_mutex, K_FOREVER);
		ret = mobile_lfs_config_read(&sensor_configs_file, sensor_configs_path,
					     config.mac_address, &config);
		if (ret < 0) {
			printk("Config not found for MAC %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX\n",
				config.mac_address[0], config.mac_address[1],
				config.mac_address[2], config.mac_address[3],
				config.mac_address[4], config.mac_address[5]);
		}
		k_mutex_unlock(&sensor_configs_file_mutex);

		uint8_t config_buf[SensorConfig_size];
		size_t  config_buf_len = 0;

		ret = mobile_encode(config_buf, sizeof(config_buf),
				    &config_buf_len, SENSOR_CONFIG, &config);
		if (ret < 0) {
			printk("Failed to encode SensorConfig\n");
		}

		sensor_tx.length = config_buf_len;
		memcpy(sensor_tx.encoded, config_buf, config_buf_len);

		ret = k_msgq_put(&sensor_config_send_msgq, &sensor_tx, K_NO_WAIT);
		if (ret != 0) {
			printk("sensor_config_send_msgq put failed: %d\n", ret);
		}

		k_sleep(K_MSEC(1));
	}
}

K_THREAD_DEFINE(configs_file_read_thread, FILE_WRITE_THREAD_STACK,
		configs_file_read_thread_fn, NULL, NULL, NULL,
		FILE_WRITE_THREAD_PRIO, 0, 0);

/* ==========================================================================
 * BLE — GATT write / subscribe
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

static struct bt_conn *default_conn;

#define RETRY_DELAY_MS 30000

static void start_scan(void);
static void discover_nus_tx(struct bt_conn *conn);
static void discover_nus_rx(struct bt_conn *conn);

static void scan_retry_work_fn(struct k_work *work)
{
	printk("Retrying scan...\n");
	start_scan();
}

static K_WORK_DELAYABLE_DEFINE(scan_retry_work, scan_retry_work_fn);

/* ------------------------------------------------------------------
 * Write callback — fires after the remote ACKs a write.
 * ------------------------------------------------------------------ */
static void write_done(struct bt_conn *conn, uint8_t err,
		       struct bt_gatt_write_params *params)
{
	if (err) {
		printk("Write failed (err %d)\n", err);
	} else {
		printk("Write sent successfully\n");
	}
	printk("Disconnecting...\n");
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/* ------------------------------------------------------------------
 * Notification callback
 * ------------------------------------------------------------------ */
static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	if (!data) {
		printk("Unsubscribed\n");
		return BT_GATT_ITER_STOP;
	}

	/* ------------------------------------------------------------------
	 * Base node — send "hello from mobile" and disconnect
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
		write_params.length = sizeof(hello_msg) - 1;

		int err = bt_gatt_write(conn, &write_params);
		if (err) {
			printk("Base write failed (err %d), disconnecting\n", err);
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
		return BT_GATT_ITER_STOP;
	}

	/* ------------------------------------------------------------------
	 * Sensor node — enqueue raw bytes, wait for encoded config, send back
	 * ------------------------------------------------------------------ */
	if (sensor_seen) {
		printk("Notification length: %d\n", length);

		struct SensorRx sensor_rx;
		sensor_rx.length = length;
		memcpy(sensor_rx.encoded, (const uint8_t *)data, length);

		int ret = k_msgq_put(&readings_writing_msgq, &sensor_rx, K_NO_WAIT);
		if (ret != 0) {
			printk("readings_writing_msgq put failed: %d\n", ret);
		}

		/* Wait for the config thread to produce an encoded reply */
		struct SensorTx sensor_tx;
		k_msgq_get(&sensor_config_send_msgq, &sensor_tx, K_FOREVER);

		if (nus_rx_handle == 0) {
			printk("Sensor RX handle not ready, disconnecting\n");
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			return BT_GATT_ITER_STOP;
		}

		write_params.func   = write_done;
		write_params.handle = nus_rx_handle;
		write_params.offset = 0;
		write_params.data   = sensor_tx.encoded;
		write_params.length = sensor_tx.length;

		ret = bt_gatt_write(conn, &write_params);
		if (ret) {
			printk("Config write failed (err %d), disconnecting\n", ret);
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}

		sensor_seen = false;
	}

	return BT_GATT_ITER_STOP;
}

/* ------------------------------------------------------------------
 * CCC discovery
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
		printk("Subscribed to notifications\n");
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
	printk("NUS TX handle: %u — discovering CCC...\n", nus_tx_handle);

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
 * RX characteristic discovery
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
 * MTU exchange
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

	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	struct net_buf_simple_state state;
	net_buf_simple_save(buf, &state);
	sensor_seen = false;
	base_seen   = false;

	while (buf->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(buf);
		if (!len || len > buf->len) break;
		uint8_t ad_type = net_buf_simple_pull_u8(buf);
		if (ad_type == BT_DATA_NAME_COMPLETE ||
		    ad_type == BT_DATA_NAME_SHORTENED) {
			int data_len = len - 1;
			if (data_len == (int)strlen(SENSOR_NAME) &&
			    memcmp(buf->data, SENSOR_NAME, data_len) == 0) {
				sensor_seen = true;
				printk("Sensor node detected\n");
			}
			if (data_len == (int)strlen(BASE_NAME) &&
			    memcmp(buf->data, BASE_NAME, data_len) == 0) {
				base_seen = true;
				printk("Base node detected\n");
			}
		}
		net_buf_simple_pull(buf, len - 1);
	}

	net_buf_simple_restore(buf, &state);

	if (!sensor_seen && !base_seen) {
		return;
	}

	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Connecting to [%s] RSSI=%d...\n", addr_str, rssi);

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
		printk("Scanning for SensorNode / BaseNode...\n");
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

	nus_tx_handle = 0;
	nus_rx_handle = 0;

	exchange_params.func = exchange_func;
	int mtu_err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (mtu_err) {
		printk("MTU exchange failed (err %d), discovering anyway\n", mtu_err);
		discover_nus_rx(conn);
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

	err = mobile_lfs_init(mountpoint);
	if (err < 0) {
		printk("Failed to mount LittleFS\n");
		return err;
	}

	err = mobile_lfs_file_init(mountpoint, &sensor_configs_file,
				   sensor_configs, sensor_configs_path);
	if (err < 0) {
		printk("Failed to init %s\n", sensor_configs);
		return err;
	}

	err = mobile_lfs_file_init(mountpoint, &sensor_nodes_file,
				   sensor_nodes, sensor_nodes_path);
	if (err < 0) {
		printk("Failed to init %s\n", sensor_nodes);
		return err;
	}

	printk("Mobile node started\n");

	start_scan();

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
