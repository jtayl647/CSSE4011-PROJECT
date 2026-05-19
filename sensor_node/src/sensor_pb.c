#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "sensor_pb.h"
#include <zephyr/kernel.h>
#include "nanopb_types.h"

static bool encode_sensor_node(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message);
static bool decode_sensor_config(uint8_t *buffer, size_t message_length, void* message);

//Testing only
static bool decode_sensor_node(uint8_t *buffer, size_t message_length, void* message);
static bool decode_all_configs(uint8_t *buffer, size_t message_length, void* message);


/************************************************
 *  Entry function for encoding of structs relevant to the Sensor node
 * 
 * @param buffer a buffer which the encoded struct bytes will be written to
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param type the type of struct that should be encoded
 * @param message Pointer to the struct to encode
 *************************************************/

int sensor_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message) {
    if (type == SENSOR_NODE) {
        //Call the sensor node encode function
        if (encode_sensor_node(buffer, buffer_size, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else {
        //error, incorrect type passed, print errorr and return -1
        printk("Incorrect struct type given to sensor message encoding\n");
        return -1;
    }
}

/************************************************
 *  Encoder function for struct of SensorNode type
 * 
 * @param buffer a buffer which the encoded struct bytes will be read into
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param message Pointer to the struct to encode
 *************************************************/

static bool encode_sensor_node(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message) {
    bool status;

    //Grab the correct type of struct
    SensorNode *sensor = message;

	/* Create a stream that will write to our buffer. */
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
	
	/* Now we are ready to encode the message! */
	status = pb_encode(&stream, SensorNode_fields, sensor);
	*message_length = stream.bytes_written;
	printk("Number of SensorNode bytes written: %d\n", (int) stream.bytes_written);

printk("\n");

	if (!status) {
		printk("Encoding SensorNode struct failed: %s\n", PB_GET_ERROR(&stream));
	} else {
        printk("SensorNode struct encoded\n");
        
    }

	return status;
}

/************************************************
 *  Entry function for decoding of structs relevant to the Sensor node
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param type the type of struct that should be decoded
 * @param message Pointer to the struct to write decoded information to
 *************************************************/

int sensor_decode_function(uint8_t *buffer, size_t message_length, uint8_t type, void* message) {
    if (type == SENSOR_CONFIG) {
        //do the sensor config decoding
        decode_sensor_config(buffer, message_length, message);
        return 0;
    } else {
        //error
        printk("Incorrect struct type given to sensor message decoding\n");
        return -1;
    }
}

/************************************************
 *  Decoder function for SensorConfig type struct
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param message Pointer to the struct to write decoded information to
 *************************************************/

static bool decode_sensor_config(uint8_t *buffer, size_t message_length, void* message) {
    bool status;
    //assume that we are being passed a struct that has been initialised properly
	SensorConfig *config = message;

	/* Create a stream that reads from the buffer. */
	pb_istream_t stream = pb_istream_from_buffer(buffer, message_length);

	/* Now we are ready to decode the message. */
	status = pb_decode(&stream, SensorConfig_fields, config);

	/* Check for errors... */
	if (status) {
		/* Print the data contained in the message. */
		printk("Successfully Decoded SensorConfig struct");
		printk("\n");
		pb_release(SensorConfig_fields, config);
	} else {
		printk("Decoding SensorConfig struct failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}






/////////// For testing only ///////////////////////////

/************************************************
 *  Entry function for decoding of structs relevant to the mobile node
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param type the type of struct that should be decoded
 * @param message Pointer to the struct to write decoded information to
 *************************************************/

int mobile_decode(uint8_t *buffer, size_t message_length, uint8_t type, void* message) {
    if (type == SENSOR_NODE) {
        //sensor node decode function
        if (decode_sensor_node(buffer, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else if (type == ALL_CONFIGS) {
        //all configs struct decoding functionality
        if (decode_all_configs(buffer, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else {
        //error, wrong struct type passed to the mobile decode function
        printk("Incorrect struct type given to mobile message decoding\n");
        return -1;
    }
}

/************************************************
 *  Decoder function for SensorNode type struct
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param message Pointer to the struct to write decoded information to
 *************************************************/

static bool decode_sensor_node(uint8_t *buffer, size_t message_length, void* message) {
    bool status;

	//assume that we are being passed a struct that has been initialised properly
	SensorNode *sensor = message;

	/* Create a stream that reads from the buffer. */
	pb_istream_t stream = pb_istream_from_buffer(buffer, message_length);

	/* Now we are ready to decode the message. */
	status = pb_decode(&stream, SensorNode_fields, sensor);

	/* Check for errors... */
	if (status) {
		/* Print the data contained in the message. */
		printk("Successfully Decoded SensorNode struct");
		printk("\n");
		pb_release(SensorNode_fields, sensor);
	} else {
		printk("Decoding SensorNode struct failed: %s\n", PB_GET_ERROR(&stream));
        printk("Bytes consumed while decoding SensorNode: %d\n", (int)stream.bytes_left);
        size_t position = message_length - stream.bytes_left;
        printk("Failure at byte index: %d\n", position);

        for (int i = position - 8; i < position + 8; i++) {
            if (i >= 0 && i < message_length) {
            printk("%02X ", buffer[i]);
            }
        }
        printk("\n");
	}

	return status;
}

/************************************************
 *  Decoder function for AllConfigs type struct
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param message Pointer to the struct to write decoded information to
 *************************************************/

static bool decode_all_configs(uint8_t *buffer, size_t message_length, void* message) {
    bool status;

	//assume that we are being passed a struct that has been initialised properly
	AllConfigs *configs = message;

	/* Create a stream that reads from the buffer. */
	pb_istream_t stream = pb_istream_from_buffer(buffer, message_length);

	/* Now we are ready to decode the message. */
	status = pb_decode(&stream, AllConfigs_fields, configs);

	/* Check for errors... */
	if (status) {
		/* Print the data contained in the message. */
		printk("Successfully Decoded AllConfigs struct\n");
		printk("\n");
		pb_release(AllConfigs_fields, configs);
	} else {
		printk("Decoding AllConfigs struct failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}