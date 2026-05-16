#ifndef SENSOR_PB_H
#define SENSOR_PB_H

int sensor_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message);
int sensor_decode(uint8_t *buffer, size_t message_length, uint8_t type, void* message);

#endif