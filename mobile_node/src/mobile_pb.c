#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "mobile_pb.h"
#include <zephyr/kernel.h>

bool encode_sensor_config(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message) {

    bool status;

    //Grab the correct type of struct
    SensorConfig *config = message;

	/* Create a stream that will write to our buffer. */
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
	
	/* Now we are ready to encode the message! */
	status = pb_encode(&stream, SensorConfig_fields, config);
	*message_length = stream.bytes_written;
	printk("This is the number of bytes written: %d\n", (int) stream.bytes_written);

	if (!status) {
		printk("Encoding failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}

bool encode_nodes(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message) {

    bool status;

    //Grab the correct type of struct
    Nodes *nodes = message;

	/* Create a stream that will write to our buffer. */
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
	
	/* Now we are ready to encode the message! */
	status = pb_encode(&stream, Nodes_fields, nodes);
	*message_length = stream.bytes_written;
	printk("This is the number of bytes written: %d\n", (int) stream.bytes_written);

	if (!status) {
		printk("Encoding failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}

int mobile_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message) {
    if (type == SENSOR_CONFIG) {
        //Call the sensor config encode function
        if (encode_sensor_config(buffer, buffer_size, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else if (type == NODES) {
        //Call the Nodes encoder function
        if (encode_nodes(buffer, buffer_size, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else {
        //error, incorrect type passed, print errorr and return -1
        printk("Incorrect struct type given to mobile message encoding\n");
        return -1;
    }
}

