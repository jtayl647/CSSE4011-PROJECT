#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/sys/slist.h>
#include <string.h>
#include <stdlib.h>
#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include "nanopb_types.h"

LOG_MODULE_REGISTER(base_node, LOG_LEVEL_INF);

/* ==========================================================================
 * Sensor Registry — Zephyr singly-linked list
 *
 * sensor_entry   : the actual sensor data (stored in a static pool)
 * sensor_container : wraps sensor_entry with a sys_snode_t for the list
 *
 * Shell commands:
 *   sensor add    <name> <addr_type 0|1> <b0> <b1> <b2> <b3> <b4> <b5>
 *   sensor remove <name>
 *   sensor config <name> automate <0|1>
 *   sensor config <name> period   <seconds>
 *   sensor config <name> trigger  <0-100>
 *   sensor list
 * ========================================================================== */
#define MAX_SENSORS CONFIG_SENSOR_NUM

struct sensor_entry {
	char         name[32];
	bt_addr_le_t addr;
	SensorConfig config;   /* automate, water_period, water_trigger */
};

struct sensor_container {
	sys_snode_t        node;
	struct sensor_entry *sensor;
};

/* Static pool — same pattern as mini project beacon_info/node_container arrays */
static struct sensor_entry    sensor_pool[MAX_SENSORS];
static struct sensor_container containers[MAX_SENSORS];
static bool                   slot_used[MAX_SENSORS];

/* The linked list and its mutex */
static sys_slist_t sensor_ll;
K_MUTEX_DEFINE(sensor_ll_mutex);

/* ==========================================================================
 * Internal helpers
 * ========================================================================== */

/* Find a sensor entry in the list by name */
static struct sensor_container *find_container_by_name(const char *name)
{
	struct sensor_container *c;
	SYS_SLIST_FOR_EACH_CONTAINER(&sensor_ll, c, node) {
		if (strcmp(c->sensor->name, name) == 0) {
			return c;
		}
	}
	return NULL;
}

/* Add a sensor to the linked list by pool index */
static void sensor_ll_add(int index)
{
	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
	sys_slist_append(&sensor_ll, &containers[index].node);
	k_mutex_unlock(&sensor_ll_mutex);
}

/* Remove a sensor from the linked list by pool index */
static void sensor_ll_remove(int index)
{
	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
	sys_slist_find_and_remove(&sensor_ll, &containers[index].node);
	k_mutex_unlock(&sensor_ll_mutex);
}

/* ==========================================================================
 * Shell Commands
 * ========================================================================== */

/* sensor add <name> <type 0|1> <b0>..<b5>  (hex bytes LSB first)
 * e.g.  sensor add garden 1 B9 F3 1A 0D 82 F4  */
static int cmd_add(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 9) {
		shell_error(sh, "Usage: sensor add <name> <type 0|1> <b0> <b1> <b2> <b3> <b4> <b5>");
		shell_error(sh, "  e.g. sensor add garden 1 B9 F3 1A 0D 82 F4");
		return -EINVAL;
	}

	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
	bool exists = (find_container_by_name(argv[1]) != NULL);
	k_mutex_unlock(&sensor_ll_mutex);

	if (exists) {
		shell_error(sh, "Sensor '%s' already exists", argv[1]);
		return -EEXIST;
	}

	/* Find a free slot in the pool */
	int slot = -1;
	for (int i = 0; i < MAX_SENSORS; i++) {
		if (!slot_used[i]) { slot = i; break; }
	}
	if (slot < 0) {
		shell_error(sh, "Registry full (%d sensors max)", MAX_SENSORS);
		return -ENOMEM;
	}

	/* Fill the pool entry */
	struct sensor_entry *e = &sensor_pool[slot];
	strncpy(e->name, argv[1], sizeof(e->name) - 1);
	e->addr.type = (uint8_t)strtoul(argv[2], NULL, 10);
	for (int i = 0; i < 6; i++) {
		e->addr.a.val[i] = (uint8_t)strtoul(argv[3 + i], NULL, 16);
	}
	/* Default config: automate=off, period=10s, trigger=50% */
	e->config.automate      = false;
	e->config.water_period  = 10;
	e->config.water_trigger = 50;

	/* Link the container to this pool slot and append to list */
	containers[slot].sensor = e;
	slot_used[slot] = true;
	sensor_ll_add(slot);

	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&e->addr, addr_str, sizeof(addr_str));
	shell_print(sh, "Added '%s'  addr=%s  automate=off  period=10  trigger=50",
		    e->name, addr_str);
	return 0;
}

/* sensor remove <name> */
static int cmd_remove(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: sensor remove <name>");
		return -EINVAL;
	}

	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
	struct sensor_container *c = find_container_by_name(argv[1]);
	k_mutex_unlock(&sensor_ll_mutex);

	if (!c) {
		shell_error(sh, "Sensor '%s' not found", argv[1]);
		return -ENOENT;
	}

	/* Find the pool slot so we can mark it free */
	int slot = (int)(c - containers);
	sensor_ll_remove(slot);
	memset(&sensor_pool[slot], 0, sizeof(sensor_pool[slot]));
	slot_used[slot] = false;

	shell_print(sh, "Removed '%s'", argv[1]);
	return 0;
}

/* sensor config <name> automate <0|1>
   sensor config <name> period   <seconds>
   sensor config <name> trigger  <0-100>    */
static int cmd_config(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 4) {
		shell_error(sh, "Usage: sensor config <name> <automate|period|trigger> <value>");
		return -EINVAL;
	}

	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
	struct sensor_container *c = find_container_by_name(argv[1]);
	k_mutex_unlock(&sensor_ll_mutex);

	if (!c) {
		shell_error(sh, "Sensor '%s' not found", argv[1]);
		return -ENOENT;
	}

	struct sensor_entry *e = c->sensor;

	if (strcmp(argv[2], "automate") == 0) {
		e->config.automate = (bool)atoi(argv[3]);
		shell_print(sh, "%s: automate = %s", e->name,
			    e->config.automate ? "on" : "off");
	} else if (strcmp(argv[2], "period") == 0) {
		e->config.water_period = atoi(argv[3]);
		shell_print(sh, "%s: water_period = %d s", e->name, e->config.water_period);
	} else if (strcmp(argv[2], "trigger") == 0) {
		e->config.water_trigger = atoi(argv[3]);
		shell_print(sh, "%s: water_trigger = %d%%", e->name, e->config.water_trigger);
	} else {
		shell_error(sh, "Unknown field '%s' — use automate, period, or trigger", argv[2]);
		return -EINVAL;
	}
	return 0;
}

/* sensor list */
static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);

	int count = 0;
	struct sensor_container *c;
	SYS_SLIST_FOR_EACH_CONTAINER(&sensor_ll, c, node) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(&c->sensor->addr, addr_str, sizeof(addr_str));
		shell_print(sh, "[%d] %-16s  addr=%-30s  automate=%-3s  period=%ds  trigger=%d%%",
			    count, c->sensor->name, addr_str,
			    c->sensor->config.automate ? "on" : "off",
			    c->sensor->config.water_period,
			    c->sensor->config.water_trigger);
		count++;
	}

	k_mutex_unlock(&sensor_ll_mutex);

	if (count == 0) {
		shell_print(sh, "No sensors registered. Use: sensor add <name> <type> <b0..b5>");
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sensor,
	SHELL_CMD_ARG(add,    NULL,
		"Add:    sensor add <name> <type 0|1> <b0>..<b5>  (hex, LSB first)",
		cmd_add, 9, 0),
	SHELL_CMD_ARG(remove, NULL,
		"Remove: sensor remove <name>",
		cmd_remove, 2, 0),
	SHELL_CMD_ARG(config, NULL,
		"Config: sensor config <name> <automate|period|trigger> <value>",
		cmd_config, 4, 0),
	SHELL_CMD_ARG(list,   NULL,
		"List all registered sensors",
		cmd_list, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sensor, &sub_sensor, "Sensor node management", NULL);


/* ==========================================================================
 * NUS RX Callback
 * ========================================================================== */
static void nus_received(struct bt_conn *conn, const void *data, uint16_t len,
			 void *ctx)
{
	printk("got some shit.\n");
}

static void nus_notif_enabled(bool enabled, void *ctx)
{
	if (!enabled) {
		return;
	}

	printk("Mobile subscribed - sending sensor data\n");
	const char *test = "hello from base!\n";

	int err = bt_nus_send(NULL, test, strlen(test));
	if (err) {
		printk("Send failed (err %d)\n", err);
	} else {
		printk("Sent %d bytes\n", (int)strlen(test));
	}
}

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
	printk("=== Base Node Starting ===\n");

	sys_slist_init(&sensor_ll);

	printk("Ready — use 'sensor add' to register sensors\n");

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

	LOG_INF("Base node advertising, waiting for mobile...");

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
