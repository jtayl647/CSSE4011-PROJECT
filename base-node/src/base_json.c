#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
//The inner struct for an embedded JSON message

struct sensor_reading {
    int32_t temp;
    int32_t humidity;
    int32_t pressure;
    int32_t moisture;
    int32_t sensor_meas_time;
};

//outer struct representing the SensorNode
struct sensor_node {
    char* name;
    int32_t mobile_sees_sensor_time;
    int32_t mobile_sees_base_time;
    int32_t sensor_sees_mobile;
    struct sensor_reading readings[CONFIG_NUM_READINGS];
    size_t readings_len;
};

//array of JSON packet primitives for the inner struct
static const struct json_obj_descr reading_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, temp, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, humidity, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, pressure, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, moisture, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_reading, sensor_meas_time, JSON_TOK_NUMBER),
};

//array of JSON packet primitives for the outside struct
static const struct json_obj_descr packet_descr[] = {

    JSON_OBJ_DESCR_PRIM(
        struct sensor_node,
        name,
        JSON_TOK_STRING
    ),

    JSON_OBJ_DESCR_PRIM(struct sensor_node, mobile_sees_sensor_time, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_node, mobile_sees_base_time, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sensor_node, sensor_sees_mobile, JSON_TOK_NUMBER),

    JSON_OBJ_DESCR_OBJ_ARRAY(
        struct sensor_node,
        readings,
        CONFIG_NUM_READINGS,
        readings_len,
        reading_descr,
        ARRAY_SIZE(reading_descr)
    ),
};

static int fill_outer_struct_times(SensorNode* decoded_sensor, char* name, struct sensor_node* json_sensor, int32_t mobile_sees_base);
static int fill_outer_struct_readings(SensorNode* decoded_sensor, struct sensor_node* json_sensor);


int create_sensor_json(void* sensor_node, char* sensor_name, char* json_buffer, int32_t mobile_sees_base) {
    
    //cast the SensorNode
    SensorNode* decoded_sensor = sensor_node;
    
    //initialise the outer struct
    struct sensor_node json_sensor = {0};

    //helper function to fill outer struct fields except for repeated inner structs
    fill_outer_struct_times(decoded_sensor, sensor_name, &json_sensor, mobile_sees_base);

    //fill the repeated structs
    fill_outer_struct_readings(decoded_sensor, &json_sensor);

    //now encode to the buffer
    int ret = json_obj_encode_buf(packet_descr, ARRAY_SIZE(packet_descr), &json_sensor, json_buffer, sizeof(json_buffer));
    if (ret < 0) {
        printk("Error encoding SensorNode info: %d\n", ret);
        return ret;
    }
    return 0;
}

static int fill_outer_struct_times(SensorNode* decoded_sensor, char* name, struct sensor_node* json_sensor, int32_t mobile_sees_base) {

    //set information carries in SensorNode struct decoded from mobile
    json_sensor->name = name;
    json_sensor->mobile_sees_sensor_time = decoded_sensor->mobile_sees_sensor;
    json_sensor->mobile_sees_base_time = mobile_sees_base;
    json_sensor->sensor_sees_mobile = decoded_sensor->sensor_sees_mobile;
    json_sensor->readings_len = decoded_sensor->readings_count;

    return 0;
}

static int fill_outer_struct_readings(SensorNode* decoded_sensor, struct sensor_node* json_sensor) {
    //loop through the number of readings
    for (int i = 0; i < CONFIG_NUM_READINGS; i++) {
        json_sensor->readings[i].temp = decoded_sensor->readings[i].temp;
        json_sensor->readings[i].humidity = decoded_sensor->readings[i].humidity;
        json_sensor->readings[i].pressure = decoded_sensor->readings[i].pressure;
        json_sensor->readings[i].moisture = decoded_sensor->readings[i].moisture;
        json_sensor->readings[i].sensor_meas_time = decoded_sensor->readings[i].meas_time;
    }
    return 0;
}
