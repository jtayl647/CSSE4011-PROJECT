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

/*******************************************************
 * Mobile Node Encoding Functions
 *******************************************************/

/************************************************
 *  Entry function for encoding of structs relevant to the mobile node
 * 
 * @param buffer a buffer which the encoded struct bytes will be written to
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param type the type of struct that should be encoded
 * @param message Pointer to the struct to encode
 *************************************************/

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

/************************************************
 *  Encoder function for struct of SensorConfig type
 * 
 * @param buffer a buffer which the encoded struct bytes will be read into
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param message Pointer to the struct to encode
 *************************************************/

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

/************************************************
 *  Encoder function for struct of Nodes type
 * 
 * @param buffer a buffer which the encoded struct bytes will be read into
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param message Pointer to the struct to encode
 *************************************************/

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


/*******************************************************
 * Mobile Node Decoding Functions
 *******************************************************/


/************************************************
 *  Entry function for decoding of structs relevant to the mobile node
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param type the type of struct that should be decoded
 * @param message Pointer to the struct to write decoded information to
 *************************************************/

int mobile_decode(uint8_t *buffer, size_t *message_length, uint8_t type, void* message) {
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

bool decode_sensor_node(uint8_t *buffer, size_t *message_length, void* message) {
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
		printk("Successfully Decoded sensor node type message")
		printk("\n");
		pb_release(SensorNode_fields, sensor);
	} else {
		printk("Decoding sensor node message failed: %s\n", PB_GET_ERROR(&stream));
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

bool decode_all_configs(uint8_t *buffer, size_t *message_length, void* message) {
    bool status;

	//assume that we are being passed a struct that has been initialised properly
	AllConfigs *configs = message;

	/* Create a stream that reads from the buffer. */
	pb_istream_t stream = pb_istream_from_buffer(buffer, message_length);

	/* Now we are ready to decode the message. */
	status = pb_decode(&stream, AllCOnfigs_fields, configs);

	/* Check for errors... */
	if (status) {
		/* Print the data contained in the message. */
		printk("Successfully Decoded {AllConfigs} type message")
		printk("\n");
		pb_release(AllConfigs_fields, configs);
	} else {
		printk("Decoding sensor node message failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}

