#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "base_pb.h"
#include "nanopb_types.h"
#include <zephyr/kernel.h>

static bool encode_all_configs(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message);
static bool decode_nodes(uint8_t *buffer, size_t message_length, void* message);

/************************************************
 *  Entry function for encoding of structs relevant to the base node
 * 
 * @param buffer a buffer which the encoded struct bytes will be written to
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param type the type of struct that should be encoded
 * @param message Pointer to the struct to encode
 *************************************************/

int base_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message) {
    if (type == ALL_CONFIGS) {
        //Call the all configs encode function
        if (encode_all_configs(buffer, buffer_size, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else {
        //error, incorrect type passed, print errorr and return -1
        printk("Incorrect struct type given to base message encoding\n");
        return -1;
    }
}

/************************************************
 *  Encoder function for struct of AllConfigs type
 * 
 * @param buffer a buffer which the encoded struct bytes will be read into
 * @param buffer_size Space available in the buffer to write to, overflows will trigger a streaming error
 * @param message_length the number of bytes read into the buffer during streaming process
 * @param message Pointer to the struct to encode
 *************************************************/

static bool encode_all_configs(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message) {
    bool status;

    //Grab the correct type of struct
    AllConfigs *configs = message;

	/* Create a stream that will write to our buffer. */
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
	
	/* Now we are ready to encode the message! */
	status = pb_encode(&stream, AllConfigs_fields, configs);
	*message_length = stream.bytes_written;
	printk("Number of AllConfigs bytes written: %d\n", (int) stream.bytes_written);

	if (!status) {
		printk("Encoding AllConfigs struct failed: %s\n", PB_GET_ERROR(&stream));
	} else {
        printk("Encoded AllConfigs struct\n");
    }

	return status;
}

/************************************************
 *  Entry function for decoding of structs relevant to the Base node
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param type the type of struct that should be decoded
 * @param message Pointer to the struct to write decoded information to
 *************************************************/
int base_decode(uint8_t *buffer, size_t message_length, uint8_t type, void* message) {
    if (type == NODES) {
        if (decode_nodes(buffer, message_length, message)) {
            return 0;
        } else {
            return -1;
        }
    } else {
        //incorrect type passed to the base decoding function
        printk("Incorrect struct type passed to the base decode\n");
        return -1;
    }
}

/************************************************
 *  Decoder function for Nodes type struct
 * 
 * @param buffer a buffer which the encoded struct bytes will be read from
 * @param message_length the number of bytes to read from the buffer
 * @param message Pointer to the struct to write decoded information to
 *************************************************/
static bool decode_nodes(uint8_t *buffer, size_t message_length, void* message) {
    
    bool status;
    //assume that we are being passed a struct that has been initialised properly
	Nodes *nodes = message;

	/* Create a stream that reads from the buffer. */
	pb_istream_t stream = pb_istream_from_buffer(buffer, message_length);

	/* Now we are ready to decode the message. */
	status = pb_decode(&stream, Nodes_fields, nodes);

	/* Check for errors... */
	if (status) {
		/* Print the data contained in the message. */
		printk("Successfully Decoded Nodes struct");
		printk("\n");
		pb_release(Nodes_fields, nodes);
	} else {
		printk("Decoding Nodes struct failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}