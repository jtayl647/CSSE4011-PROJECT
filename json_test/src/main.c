#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <zephyr/shell/shell.h>

/* ==========================================================================
 * Mirrors base_json.c structs and descriptors exactly
 * ========================================================================== */

#define NUM_READINGS     4   /* match CONFIG_NUM_READINGS from base-node */
#define JSON_STACK_SIZE  (4 * 4096)
#define JSON_THREAD_PRIO 6

struct sensor_reading {
    int32_t temp;
    int32_t humidity;
    int32_t pressure;
    int32_t moisture;
    int32_t sensor_meas_time;
};

struct sensor_node {
    char   *name;
    int32_t mobile_sees_sensor_time;
    int32_t mobile_sees_base_time;
    int32_t sensor_sees_mobile;
    struct sensor_reading readings[NUM_READINGS];
    size_t readings_len;
};

static const struct json_obj_descr reading_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, temp,             JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, humidity,         JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, pressure,         JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, moisture,         JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, sensor_meas_time, JSON_TOK_NUMBER),
};

static const struct json_obj_descr packet_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct sensor_node, name,                    JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct sensor_node, mobile_sees_sensor_time, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_node, mobile_sees_base_time,   JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_node, sensor_sees_mobile,      JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_OBJ_ARRAY(struct sensor_node, readings, NUM_READINGS, readings_len,
                              reading_descr, ARRAY_SIZE(reading_descr)),
};

/* ==========================================================================
 * Fake data queue — mimics nodes_json_msgq from base-node
 * ========================================================================== */
struct fake_visit {
    char    name[32];
    int32_t mobile_sees_sensor_time;
    int32_t mobile_sees_base_time;
    int32_t sensor_sees_mobile;
    struct sensor_reading readings[NUM_READINGS];
    size_t  readings_len;
};

K_MSGQ_DEFINE(json_test_msgq, sizeof(struct fake_visit), 4, 4);

/* ==========================================================================
 * JSON thread — same stack size as base-node json_nodes_thread
 * ========================================================================== */
static void json_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    static struct fake_visit  visit;
    static struct sensor_node json_sensor;
    static char               json_buffer[512];

    while (1) {
        printk("JSON thread: waiting for visit...\n");
        k_msgq_get(&json_test_msgq, &visit, K_FOREVER);
        printk("JSON thread: visit received for '%s'\n", visit.name);

        memset(&json_sensor, 0, sizeof(json_sensor));
        json_sensor.name                    = visit.name;
        json_sensor.mobile_sees_sensor_time = visit.mobile_sees_sensor_time;
        json_sensor.mobile_sees_base_time   = visit.mobile_sees_base_time;
        json_sensor.sensor_sees_mobile      = visit.sensor_sees_mobile;
        json_sensor.readings_len            = visit.readings_len;

        for (int i = 0; i < (int)visit.readings_len; i++) {
            json_sensor.readings[i] = visit.readings[i];
        }

        int ret = json_obj_encode_buf(packet_descr, ARRAY_SIZE(packet_descr),
                                      &json_sensor, json_buffer, sizeof(json_buffer));
        if (ret == 0) {
            printk("%s\n", json_buffer);
        } else {
            printk("JSON encode failed: %d\n", ret);
        }

        k_sleep(K_MSEC(1));
    }
}

K_THREAD_DEFINE(json_thread, JSON_STACK_SIZE,
                json_thread_fn, NULL, NULL, NULL,
                JSON_THREAD_PRIO, 0, 0);

/* ==========================================================================
 * Main — push fake visits into the queue
 * ========================================================================== */
int main(void)
{
    printk("=== JSON test starting ===\n");
    k_sleep(K_MSEC(500));

    /* Visit 1: garden, 2 readings */
    static struct fake_visit v1 = {
        .name                    = "garden",
        .mobile_sees_sensor_time = 200000,
        .mobile_sees_base_time   = 300000,
        .sensor_sees_mobile      = 105000,
        .readings_len            = 2,
        .readings = {
            { .temp = 2312, .humidity = 52, .pressure = 101567, .moisture = 80, .sensor_meas_time = 2000 },
            { .temp = 2318, .humidity = 54, .pressure = 101590, .moisture = 78, .sensor_meas_time = 4000 },
        },
    };

    /* Visit 2: backyard, 4 readings */
    static struct fake_visit v2 = {
        .name                    = "backyard",
        .mobile_sees_sensor_time = 350000,
        .mobile_sees_base_time   = 430000,
        .sensor_sees_mobile      =  60000,
        .readings_len            = 4,
        .readings = {
            { .temp = 2400, .humidity = 60, .pressure = 101200, .moisture = 45, .sensor_meas_time = 1000 },
            { .temp = 2405, .humidity = 61, .pressure = 101210, .moisture = 44, .sensor_meas_time = 2000 },
            { .temp = 2410, .humidity = 62, .pressure = 101220, .moisture = 43, .sensor_meas_time = 3000 },
            { .temp = 2415, .humidity = 63, .pressure = 101230, .moisture = 42, .sensor_meas_time = 4000 },
        },
    };

    while(1){
        printk("mess with it...\n");
        k_msgq_put(&json_test_msgq, &v1, K_NO_WAIT);

        k_sleep(K_MSEC(2000));

        printk("mess with it...\n");
        k_msgq_put(&json_test_msgq, &v2, K_NO_WAIT);

         k_sleep(K_MSEC(2000));
    }

    k_sleep(K_FOREVER);
    return 0;
}
