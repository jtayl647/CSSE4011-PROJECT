#ifndef MOBILE_PB_H
#define MOBILE_PB_H

int mobile_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message);
int mobile_decode(uint8_t *buffer, size_t message_length, uint8_t type, void* message);

#endif