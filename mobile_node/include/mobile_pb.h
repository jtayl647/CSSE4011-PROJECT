#ifndef MOBILE_PB_H
#define MOBILE_PB_H

#define SENSOR_CONFIG 1
#define NODES 2
#define SENSOR_NODE 3
#define ALL_CONFIGS 4

int mobile_encode(uint8_t *buffer, size_t buffer_size, size_t *message_length, uint8_t type, void* message);
bool encode_sensor_config(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message);
bool encode_nodes(uint8_t *buffer, size_t buffer_size, size_t *message_length, void *message);
int mobile_decode(uint8_t *buffer, size_t *message_length, uint8_t type, void* message);
bool decode_sensor_node(uint8_t *buffer, size_t *message_length, void* message);
bool decode_all_configs(uint8_t *buffer, size_t *message_length, void* message);

#endif