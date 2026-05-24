#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/printk.h>
#include <string.h>

// #define MAX_LEN 64
// #define NUM_READINGS 12 // kconfig sets this usually

/* --- Structs --- */
// struct data_readings {
//     int32_t temp;
//     int32_t humidity;
//     int32_t pressure;
//     int32_t moisture;
//     int32_t meas_time;
// };

struct mule_visit {
    // const char *sensor_name;
    int mobile_sees_sensor_time;
    int mobile_sees_base_time;
    int base_sees_mobile_time;
    int send_time;
    // struct data_readings readings[12];
    // size_t readings_count;              /* size_t required by OBJ_ARRAY */
};

// static const struct json_obj_descr data_reading[] = {
//     JSON_OBJ_DESCR_PRIM(struct data_readings, temp,      JSON_TOK_NUMBER),
//     JSON_OBJ_DESCR_PRIM(struct data_readings, humidity,  JSON_TOK_NUMBER),
//     JSON_OBJ_DESCR_PRIM(struct data_readings, pressure,  JSON_TOK_NUMBER),
//     JSON_OBJ_DESCR_PRIM(struct data_readings, moisture,  JSON_TOK_NUMBER),
//     JSON_OBJ_DESCR_PRIM(struct data_readings, meas_time, JSON_TOK_NUMBER),
// };

static const struct json_obj_descr mule_visit_d[] = {
	// JSON_OBJ_DESCR_PRIM(struct mule_visit, sensor_name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct mule_visit, mobile_sees_sensor_time,  JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct mule_visit, mobile_sees_base_time,    JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct mule_visit, base_sees_mobile_time,    JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct mule_visit, send_time,    			 JSON_TOK_NUMBER),
	// JSON_OBJ_DESCR_PRIM(struct mule_visit, readings_count, JSON_TOK_NUMBER),
	// JSON_OBJ_DESCR_OBJ_ARRAY(struct mule_visit, readings, NUM_READINGS, readings_count,
    //                           data_reading, ARRAY_SIZE(data_reading)),
};

int main(void)
{
	printk("in main\n");
	struct mule_visit visit = {
        // .sensor_name             = "garden",
        .mobile_sees_sensor_time = 1000,
        .mobile_sees_base_time   = 2000,
        .base_sees_mobile_time   = 3000,
        .send_time               = 4000,
        // .readings_count          = 4,
    };
	printk("after stuct\n");


    /* Fill 4 readings in a loop */
    // for (int i = 0; i < 4; i++) {
    //     visit.readings[i].temp     = 2300 + i * 10;
    //     visit.readings[i].humidity = 50   + i;
    //     visit.readings[i].pressure = 101325 + i * 100;
    //     visit.readings[i].moisture = 60   + i * 5;
    //     visit.readings[i].meas_time = 1000 * (i + 1);
    // }
	// printk("after filling readings\n");


	char buf[124];
    int ret = json_obj_encode_buf(mule_visit_d, ARRAY_SIZE(mule_visit_d),
                                  &visit, buf, sizeof(buf));
	printk("after encoding\n");

	// if (ret == 0) {
	// 	printk("%s\n", buf);
	// } else {
	// 	printk("encode failed: %d\n", ret);
	// }
	

	return 0;
}
