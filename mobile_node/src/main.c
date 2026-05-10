#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(mobile_node, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* NUS service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (little-endian) */
static const uint8_t nus_uuid[16] = {
	0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
	0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
};

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

/* Config to send back to the sensor after receiving its data */
static const char config_payload[] = "threshold=30,pump=auto";

/* ------------------------------------------------------------------
 * Write callback — fires after sensor ACKs the config write.
 * Safe to disconnect here.
 * ------------------------------------------------------------------ */
static void write_done(struct bt_conn *conn, uint8_t err,
		       struct bt_gatt_write_params *params)
{
	if (err) {
		printk("Config write failed (err %d)\n", err);
	} else {
		printk("Config sent: %s\n", config_payload);
	}

	printk("Disconnecting...\n");
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/* ------------------------------------------------------------------
 * Notification callback — receives sensor data, then sends config.
 * ------------------------------------------------------------------ */
static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	if (!data) {
		printk("Unsubscribed\n");
		return BT_GATT_ITER_STOP;
	}

	printk("Sensor data: %.*s\n", length, (const char *)data);

	if (nus_rx_handle == 0) {
		printk("RX handle not ready yet, disconnecting anyway\n");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}

	/* Write config back to sensor; disconnect in write_done callback */
	write_params.func   = write_done;
	write_params.handle = nus_rx_handle;
	write_params.offset = 0;
	write_params.data   = config_payload;
	write_params.length = strlen(config_payload);

	int err = bt_gatt_write(conn, &write_params);
	if (err) {
		printk("Config write start failed (err %d), disconnecting\n", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	return BT_GATT_ITER_STOP; /* unsubscribe — we're done with this node */
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

	if (type != BT_GAP_ADV_TYPE_SCAN_RSP) {
		return;
	}

	struct net_buf_simple_state state;
	net_buf_simple_save(buf, &state);
	bool found = false;

	while (buf->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(buf);
		if (!len || len > buf->len) break;
		uint8_t ad_type = net_buf_simple_pull_u8(buf);
		if ((ad_type == BT_DATA_UUID128_ALL ||
		     ad_type == BT_DATA_UUID128_SOME) &&
		    len - 1 == 16 &&
		    memcmp(buf->data, nus_uuid, 16) == 0) {
			found = true;
			break;
		}
		net_buf_simple_pull(buf, len - 1);
	}

	net_buf_simple_restore(buf, &state);

	if (!found) {
		return;
	}

	printk("Found sensor node! RSSI=%d connecting...\n", rssi);

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
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
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

	printk("Mobile node started\n");

	start_scan();

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
