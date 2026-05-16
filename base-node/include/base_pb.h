#ifndef BASE_PB_H
#define BASE_PB_H

int base_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message);
int base_decode(uint8_t *buffer, size_t message_length, uint8_t type, void* message);

#endif