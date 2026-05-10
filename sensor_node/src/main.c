#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(sensor_node, LOG_LEVEL_INF);

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
 *
 * Called when the mobile node sends data to the sensor.
 * Used to receive config commands: threshold, pump mode, and data ACK.
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

	char data[] = "moisture=45,humidity=62,temp=24,pressure=1013";
	int err = bt_nus_send(NULL, data, strlen(data));
	if (err) {
		printk("Send failed (err %d)\n", err);
	} else {
		printk("Sent sensor data\n");
	}
}

static struct bt_nus_cb nus_cb = {
	.received     = nus_received,
	.notif_enabled = nus_notif_enabled,
};

/* ==========================================================================
 * Advertising restart work - deferred so BLE stack finishes cleanup first
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

	/* Defer restart so the BLE stack fully releases the connection first */
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

	/* Everything is event driven - just sleep */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
