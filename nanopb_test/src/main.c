/*
 * Copyright (c) 2011 Petteri Aimonen
 * Copyright (c) 2021 Basalte bv
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <pb_encode.h>
#include <pb_decode.h>
#include "src/test.pb.h"
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

void fill_buffer(Envelope *envelope, int letterNum, const char* msg) {
	

	printk("this is sizeof on the buffer: %d\n", sizeof(envelope->letter[letterNum].buffer));
	printk("this is sizeof on the msg: %d\n", sizeof(msg));
	strncpy(envelope->letter[letterNum].buffer,
        msg,
        sizeof(envelope->letter[letterNum].buffer) - 1);

	envelope->letter[letterNum].buffer[
        sizeof(envelope->letter[letterNum].buffer) - 1
    ] = '\0';
	
	// printk("this is the strlen of the string: %d\n", strlen(msg));
	// printk("this is the string: %s\n", msg);

	//envelope->letter[letterNum].buffer[2] = '\0';

	printk("First Item added: %c\n", envelope->letter[letterNum].buffer[0]);
	printk("Second Item added: %c\n", envelope->letter[letterNum].buffer[1]);
}

void write_letter(Envelope *envelope) {

	envelope->letter_count = 4;
	for (int i = 0; i < envelope->letter_count; i++) {
		envelope->letter[i].sender = i;
		envelope->letter[i].receiver = 1;	
		if (i < 1) {
			char msg[] = "L1";
			fill_buffer(envelope, i, msg);
		} else if (i < 2){
			char msg[] = "L2";
			fill_buffer(envelope, i, msg);
		} else if (i < 3) {
			char msg[] = "L3";	
			fill_buffer(envelope, i, msg);
		} else {
			char msg[] = "L4";
			fill_buffer(envelope, i, msg);
		}
	}
}

bool encode_message(uint8_t *buffer, size_t buffer_size, size_t *message_length)
{
	bool status;

	/* Allocate space on the stack to store the message data.
	 *
	 * Nanopb generates simple struct definitions for all the messages.
	 * - check out the contents of simple.pb.h!
	 * It is a good idea to always initialize your structures
	 * so that you do not have garbage data from RAM in there.
	 */
	Envelope envelope = Envelope_init_zero;
	/* Create a stream that will write to our buffer. */
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);

	write_letter(&envelope);
	
	// /* Fill in the lucky number */
	// message.sender = 13;
	// for (int i = 0; i < CONFIG_SAMPLE_BUFFER_SIZE; ++i) {
	// 	message.buffer[i] = (uint8_t)(i * 2);
	// }
	
	/* Now we are ready to encode the message! */
	status = pb_encode(&stream, Envelope_fields, &envelope);
	*message_length = stream.bytes_written;
	printk("This is the number of bytes written: %d\n", (int) stream.bytes_written);

	if (!status) {
		printk("Encoding failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}

bool decode_message(uint8_t *buffer, size_t message_length)
{
	bool status;

	/* Allocate space for the decoded message. */
	Envelope envelope = Envelope_init_zero;

	/* Create a stream that reads from the buffer. */
	pb_istream_t stream = pb_istream_from_buffer(buffer, message_length);

	/* Now we are ready to decode the message. */
	status = pb_decode(&stream, Envelope_fields, &envelope);

	/* Check for errors... */
	if (status) {
		for (int i = 0; i < 4; i++) {
			int sender = envelope.letter[i].sender;
			int receiver = envelope.letter[i].receiver;
			printk("Current Letter Sender: %d\n", sender);
			printk("Current Letter Receiver: %d\n", receiver);
			printk("The love letter contained: %s\n", envelope.letter[i].buffer);
		}
		/* Print the data contained in the message. */
		// printk("Your sender was %d!\n", (int)envelope.sender);
		// printk("Buffer contains: ");
		// for (int i = 0; i < CONFIG_SAMPLE_BUFFER_SIZE; ++i) {
		// 	printk("%s%d", ((i == 0) ? "" : ", "), (int) envelope.buffer[i]);
		// }
		printk("\n");
		pb_release(Envelope_fields, &envelope);
	} else {
		printk("Decoding failed: %s\n", PB_GET_ERROR(&stream));
	}

	return status;
}

int main(void)
{
	// LOG_INF("at the start of main");
	/* This is the buffer where we will store our message. */
	uint8_t buffer[Envelope_size];
	size_t message_length;

	// printk("hello");
	// /* Encode our message */
	if (!encode_message(buffer, sizeof(buffer), &message_length)) {
		printk("SOMETHING DIED");
		return 0;
	}

	// /* Now we could transmit the message over network, store it in a file or
	//  * wrap it to a pigeon's leg.
	//  */

	// /* But because we are lazy, we will just decode it immediately. */
	decode_message(buffer, message_length);
	return 0;
}

//Change the static shit