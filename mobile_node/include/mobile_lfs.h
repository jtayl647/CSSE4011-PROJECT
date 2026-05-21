#ifndef MOBILE_LFS_H
#define MOBILE_LFS_H

#define MAX_PATH_LEN 255
#define INT_32_MAX_WIDTH 11
#define CONFIGS_CSV_LINE_LENGTH 40   
#define READINGS_CSV_LINE_LENGTH 86

int mobile_lfs_init(void* mp);
int mobile_lfs_file_init(void* mp, void* file, char* filename, char* filepath);
int mobile_lfs_config_write(void* cf, char* path, void* all_cnfgs);
int mobile_lfs_config_read(void* cf, char* path, void* mac_buf, void* dest_config);
int mobile_lfs_write_sensor_readings(void* rf, char* path, void* sensor_node);
int mobile_lfs_read_sensor_readings(void* rf, char* path, void* all_nodes);

#endif