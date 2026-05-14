#ifndef MOBILE_PB_H
#define MOBILE_PB_H

#define SENSOR_CONFIG 1
#define NODES 2

int mobile_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message);
bool encode_sensor_config(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message);
bool encode_nodes(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message);

#endif