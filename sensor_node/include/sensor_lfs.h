#ifndef SENSOR_LFS_H
#define SENSOR_LFS_H

#define MAX_PATH_LEN 255
#define INT32_MAX_WIDTH 11
#define SENSOR_CSV_LINE_LENGTH 61

int sensor_lfs_init(void* mp, void* file, char* file_name, char *full_path);
int sensor_lfs_write_to_file(void* file, char* filename, void* sensor_readings);
int sensor_lfs_file_truncate(void* file, char *full_path);
int sensor_lfs_read_from_file(void* file, char* filename, void* sensor_node);

#endif