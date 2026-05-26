#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/sys/slist.h>
#include <string.h>
#include <stdlib.h>
#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include "nanopb_types.h"
#include "base_pb.h"
#include "base_json.h"

LOG_MODULE_REGISTER(base_node, LOG_LEVEL_INF);

static int match_name_to_mac(SensorNode* decoded_sensor, sys_slist_t* base_nodes, char* name);

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
#define SENSOR_DECOMP_STACK 2*4096
#define JSON_CONSTRUCT_STACK 4*4096
#define SENSOR_DECOMP_PRIO 6
static const char* MOBILE_NAME = "MobileNode";
static bool mobile_seen = false;


struct MobileRx {
	uint8_t encoded[255];
	uint16_t length;
};

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


K_MSGQ_DEFINE(sensors_decomp_msgq,
              sizeof(struct MobileRx),
              4,
              4);

K_MSGQ_DEFINE(nodes_json_msgq,
              Nodes_size,
              4,
              4);


static void sensors_decomp_thread_fn(void *a, void *b, void *c) {

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct MobileRx mobile_rx;
	int ret;

	while(1) {
		//wait on a queue on received data from the mobile node
		k_msgq_get(&sensors_decomp_msgq, &mobile_rx, K_FOREVER);
		printk("AllNodes struct buffer received!\n");
		//we now have information passed to us, we need to decode the Nodes struct
		Nodes nodes = Nodes_init_zero;
		ret = base_decode(mobile_rx.encoded, mobile_rx.length, NODES, &nodes);
		if (ret < 0) {
			printk("Error, issue with decoding Nodes struct coming from mobile node\n");
		}

		//now we loop through the nodes struct to see what we've got
		for (int i = 0; i < nodes.nodes_count; i++) {
			//grab the node we are at
			SensorNode current_node = nodes.nodes[i];
			printk("MAC: {%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX} BLE_time: {%d}\n", 
				current_node.mac_address[0], current_node.mac_address[1],
				current_node.mac_address[2], current_node.mac_address[3],
				current_node.mac_address[4], current_node.mac_address[5],
				current_node.sensor_sees_mobile);
			for (int j = 0; j < current_node.readings_count; j++) {
				//retrieve the readings 
				DataReadings reading = current_node.readings[j];
				printk("Temp: {%d} Humidity: {%d} Pressure: {%d} Moisture: {%d} Meas_time: {%d}\n", 
					reading.temp, reading.humidity, reading.pressure, reading.moisture,
					reading.meas_time);
			}
		}

		//put the Nodes struct into a queue that leads to the json thread
		ret = k_msgq_put(&nodes_json_msgq, &nodes, K_NO_WAIT);
		if (ret < 0) {
			printk("Error putting Nodes into json queue: %d\n", ret);
		}

		k_sleep(K_MSEC(1));
	}
}

K_THREAD_DEFINE(sensors_decomp_thread, SENSOR_DECOMP_STACK,
		sensors_decomp_thread_fn, NULL, NULL, NULL,
		SENSOR_DECOMP_PRIO, 0, 0);

static void json_nodes_thread_fn(void *a, void *b, void *c) {

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	int ret;
	Nodes nodes = Nodes_init_zero;

	while(1) {
		//wait on a queue on received data from the mobile node
		k_msgq_get(&nodes_json_msgq, &nodes, K_FOREVER);

		//now we need to loop through the number of SensorNodes received
		for (int i = 0; i < nodes.nodes_count; i++) {
			//set up a buffer to read name into
			char matched_name[32];

			char json_buffer[512];

			//lock up the linked list
			k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
			ret = match_name_to_mac(&nodes.nodes[i], &sensor_ll, matched_name);
			k_mutex_unlock(&sensor_ll_mutex);

			if (ret < 0) {
				printk("Error matching name to mac when making json\n");
			}

			//now we have a name, pass the name and the sensor node to json function
			ret = create_sensor_json(&nodes.nodes[i], matched_name, json_buffer, nodes.mobile_sees_base);
			if (ret < 0) {
				printk("Error in thread creating json: %d\n", ret);
			}

			//we have a functioning json buffer, print it
			printk("%s\n", json_buffer);

		}

		k_sleep(K_MSEC(1));
	}
}

K_THREAD_DEFINE(json_nodes_thread, JSON_CONSTRUCT_STACK,
		json_nodes_thread_fn, NULL, NULL, NULL,
		SENSOR_DECOMP_PRIO, 0, 0);

static int match_name_to_mac(SensorNode* decoded_sensor, sys_slist_t* base_nodes, char* name) {
	//iterate through the linked list
	struct sensor_container *c;
	SYS_SLIST_FOR_EACH_CONTAINER(base_nodes, c, node) {
		bool match = true;
		//compare each value in the ll address and the decoded sensor addresss
		for (int i = 0; i < CONFIG_MAC_BYTES; i++) {
			//Grab current entry of this list node's mac address
			uint8_t list_addr_piece = c->sensor->addr.a.val[i];
			//account for potential endianness switch up
			uint8_t decoded_addr_piece = decoded_sensor->mac_address[i];
			if (list_addr_piece != decoded_addr_piece) {
				match = false;
				break;
			}
		}
		if (match) {
			memcpy(name, c->sensor->name, 32);
			return 0;
		} else {
			match = true;
		}
	}

	//no match was ever found
	return -1;
	
}

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
	//grab the data and put it into a struct
	struct MobileRx mobile_rx;

	mobile_rx.length = len;

	//copy the data from data to mobile rx buffer
	memcpy(mobile_rx.encoded, (uint8_t *)data, len);

	//put the struct into the queue
	int ret = k_msgq_put(&sensors_decomp_msgq, &mobile_rx, K_NO_WAIT);
		if (ret != 0) {
			printk("queue failed: %d\n", ret);
		}

}

static void nus_notif_enabled(bool enabled, void *ctx)
{
	if (!enabled) {
		return;
	}
    int ret;

	printk("Mobile subscribed - sending sensor data\n");

	AllConfigs all_configs = AllConfigs_init_zero;

	k_mutex_lock(&sensor_ll_mutex, K_FOREVER);
	printk("before sys list for loop\n");
	int count = 0;
	struct sensor_container *c;
	SYS_SLIST_FOR_EACH_CONTAINER(&sensor_ll, c, node) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		memcpy(all_configs.configs[all_configs.configs_count].mac_address, c->sensor->addr.a.val, CONFIG_MAC_BYTES);
		all_configs.configs[all_configs.configs_count].automate = c->sensor->config.automate ? true : false;
		all_configs.configs[all_configs.configs_count].water_period = c->sensor->config.water_period;
		all_configs.configs[all_configs.configs_count].water_trigger = c->sensor->config.water_trigger;
		all_configs.configs_count++;
		count++;
	}
	printk("after sys list for loop\n");
	k_mutex_unlock(&sensor_ll_mutex);

    // encode this bitch
    uint8_t configs_buf[AllConfigs_size];
    size_t configs_buf_len = 0;
	printk("before encoding");
    //encode the AllConfigs
    ret = base_encode(configs_buf, sizeof(configs_buf), &configs_buf_len, ALL_CONFIGS, &all_configs);
    if (ret < 0) {
        printk("Error with nanoPB encoding of sensor config read from file\n");
    }
	printk("after encoding");
    printk("Length of NUS packet: %d\n", (int)configs_buf_len);

	if (configs_buf_len == 0) {
		printk("No sensors configured — sending empty sentinel\n");
		uint8_t sentinel = 0x00;
		bt_nus_send(NULL, &sentinel, 1);
		return;
	}

	int err = bt_nus_send(NULL, configs_buf, configs_buf_len);
	if (err) {
		printk("Send failed (err %d)\n", err);
	} else {
		printk("Sent %d bytes\n", (int)configs_buf_len);
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

	int err;

	err = bt_nus_cb_register(&nus_cb, NULL);
	if (err) {
		printk("Failed to register NUS callbacks: %d\n", err);
		return err;
	}


    err = bt_enable(NULL);
	if (err) {
		printk("Failed to enable bluetooth: %d\n", err);
		return err;
	}


	printk("=== Base Node Starting ===\n");

	sys_slist_init(&sensor_ll);

	printk("Ready — use 'sensor add' to register sensors\n");

	k_sleep(K_SECONDS(3));

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to start advertising: %d\n", err);
		return err;
	}

    // start_scan();

	printk("Base node advertising, waiting for mobile...\n");

    

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}