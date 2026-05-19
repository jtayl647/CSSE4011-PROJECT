#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "nanopb_types.h"
#include <sensor_config.pb.h>
#include <sensor_info.pb.h>
#include "sensor_lfs.h"

static int make_file(struct fs_mount_t *mountpoint, struct fs_file_t *readings, char* file_name, char* full_path);
static int littlefs_flash_erase(unsigned int id);
static int littlefs_mount(struct fs_mount_t *mp);
static void add_reading(SensorNode *sensor, int32_t temp, int32_t humidity,
			int32_t pressure, int32_t moisture, int32_t meas_time);
static int make_file(struct fs_mount_t *mountpoint, struct fs_file_t *readings, char* file_name, char *full_path);

LOG_MODULE_REGISTER(main);

/***************************************************************************************
 * Initialises the file system for the Sensor Node
 * 
 * @param mp The mountpoint of the LittleFS
 * @param file The file where past sensor readings are to be stored
 * @param file_name The name of the file where past readings will be stored
 * @param full_path The complete path location of the readings file for the Sensor Node
 * 
 * @return 0 if successful, -1 if unsuccessful
 ****************************************************************************************/

int sensor_lfs_init(void* mp, void* file, char* file_name, char *full_path) {
    
    int rc;

	struct fs_mount_t *mountpoint = mp;
	struct fs_file_t *readings = file;

    //Mount the file
    rc = littlefs_mount(mountpoint);
    if (rc < 0) {
        LOG_ERR("Failed to mount Sensor Node FS\n");
		return -1;
    }

	//initialise the file
    fs_file_t_init(readings);

	//Buffer for storing the full filepath of the file storing past readings
	char path[MAX_PATH_LEN];

	//Creat the full filepath to the file containing past readings
	snprintf(path, sizeof(path), "%s/readings.txt", mountpoint->mnt_point);
	
	// LOG_PRINTK("mountpoint path: %s\n", path);

	//Construct the file which will hold all past sensor readings
	make_file(mountpoint, readings, file_name, full_path);

	return 0;
}

/***************************************************************************************
 * Constructs a new file for the LittleFS architecture on the Sensor Node
 * 
 * @param mountpoint The LittleFS mountpoint
 * @param readings The file where past Sensor Node readings shall be stored
 * @param file_name The name of the file where past Sensor Node readings shall be stored
 * @param full_path The full path to the file storing past Sensor Node readings
 * 
 * @return 0 if successful, -1 if unsuccessful
 *****************************************************************************************/

static int make_file(struct fs_mount_t *mountpoint, struct fs_file_t *readings,
	 char* file_name, char *full_path) {

    char path_name[MAX_PATH_LEN];
	int rc;
    //create the new file name
    snprintf(path_name, sizeof(path_name), "%s/readings.txt", mountpoint->mnt_point);

    //initialise the file
    fs_file_t_init(readings);

     //open the file
    rc = fs_open(readings, path_name, (FS_O_RDWR | FS_O_CREATE));
    if (rc < 0) {
        LOG_ERR("Error opening sensor readings file during initialisation: %d\n", rc);
		return -1;
    }

    //close the file immediately after creating it
    rc = fs_close(readings);

    if (rc < 0) {
        LOG_ERR("Error closing sensor readings file during initialisation: %d\n", rc);
		return -1;
    }

	// read the resulting file name back to the full_path defined outside of this file
	snprintf(full_path, MAX_PATH_LEN, "%s", path_name);

	return 0;
}


/***************************************************************************
 * Erases the flash block that the LittleFS architecture is located on.
 * LittleFS stays on MCU through boot cycles, so if wanting to start fresh,
 * this function must be called
 * 
 * @param id The identification number of a given LittleFS architecture
 * 
 * @return 0 if successful, <0 if unsuccessful
 ***************************************************************************/

static int littlefs_flash_erase(unsigned int id)
{
	const struct flash_area *pfa;
	int rc;

	rc = flash_area_open(id, &pfa);
	if (rc < 0) {
		LOG_ERR("FAIL: unable to find flash area %u: %d\n",
			id, rc);
		return rc;
	}

	// LOG_PRINTK("Area %u at 0x%x on %s for %u bytes\n",
	// 	   id, (unsigned int)pfa->fa_off, pfa->fa_dev->name,
	// 	   (unsigned int)pfa->fa_size);

	/* Optional wipe flash contents */
	if (IS_ENABLED(CONFIG_APP_WIPE_STORAGE)) {
		rc = flash_area_flatten(pfa, 0, pfa->fa_size);
		LOG_ERR("Erasing flash area ... %d", rc);
	}

	flash_area_close(pfa);
	return rc;
}

/**********************************************************
 * Mounts a LittleFS architecture on the MCU
 * 
 * @param mp The mount point of the LittleFS architecture
 * 
 * @return 0 if successful, -1 if unsuccessful
 ***********************************************************/

static int littlefs_mount(struct fs_mount_t *mp)
{
	int rc;

	rc = littlefs_flash_erase((uintptr_t)mp->storage_dev);
	if (rc < 0) {
		return rc;
	}
    
	rc = fs_mount(mp);
	if (rc < 0) {
		// LOG_PRINTK("FAIL: mount id %" PRIuPTR " at %s: %d\n",
		//        (uintptr_t)mp->storage_dev, mp->mnt_point, rc);
		return rc;
	}
	// LOG_PRINTK("%s mount: %d\n", mp->mnt_point, rc);

	return 0;
}

/*********************************************************************
 * Write information from a SensorReadings struct into a file
 * 
 * @param file The file where past readings are stored
 * @param filename The full filepath of the past readings file
 * @param sensor_readings The DataReadings struct full of newest data
 * 
 * @return 0 if successful, -1 if unsuccessful
 **********************************************************************/

int sensor_lfs_write_to_file(void* file, char* filename, void* sensor_readings) {
	
	//Cast file and DataReadings struct to correct forms
	struct fs_file_t *readings = file;
	DataReadings *data = sensor_readings;

	//open the file for writing, and begin writing from the end of the file
	fs_open(readings, filename, (FS_O_WRITE | FS_O_APPEND));

	//destination string
    char measurements[SENSOR_CSV_LINE_LENGTH];

	//send all the information from the struct to the buffer using zero padding
    snprintf(measurements,
         sizeof(measurements),
         "%0*d,%0*d,%0*d,%0*d,%0*d\n",
         INT32_MAX_WIDTH, data->temp,
         INT32_MAX_WIDTH, data->humidity,
         INT32_MAX_WIDTH, data->pressure,
         INT32_MAX_WIDTH, data->moisture,
         INT32_MAX_WIDTH, data->meas_time);
	
	// LOG_INF("Written to file: %s\n", measurements);

	//write the new entry to the file
    if (fs_write(readings, (void *)measurements, strlen(measurements)) < 0) {
        printk("Error when writing data to SensorNode file\n");
		return -1;
    }

    //close the file
    fs_close(readings);

	return 0;
}

/********************************************************************************************
 * Reads from the file containing past readings into a SensorNode
 * struct representing all past readings for a particular Sensor Node
 * 
 * @param file The file containing all past Sensor Node readings
 * @param filename The full path to the file containing past readings
 * @param sensor_node The SensorNode struct where info from past readings file will be read to
 * 
 * @return 0 if successful, -1 if unsuccessful
 *********************************************************************************************/

int sensor_lfs_read_from_file(void* file, char* filename, void* sensor_node) {

	struct fs_file_t *readings = file;
	SensorNode *sensor = sensor_node;

	//open the file for reading
	fs_open(readings, filename, FS_O_READ);

	//make a buffer to read into
	char buf[SENSOR_CSV_LINE_LENGTH];

	//loop through and read all the contents of the file
	while (1) {

		//fields to read data from file to
		int32_t temp = 0;
		int32_t humidity = 0;
		int32_t pressure = 0;
		int32_t moisture = 0;
		int32_t meas_time = 0;

		//read a line from the file
		ssize_t len = fs_read(readings, buf, sizeof(buf) - 1);

		if (len <= 0) {
			break;
		}

		//Insert the null pointer for string stuff
		buf[len] = '\0'; 

		//split into lines
		char *line = strtok(buf, "\n");

		while (line != NULL) {
			// LOG_INF("Line seen in file: %s\n", line);
			//scan the preformatted line into the buffer
			int scanned = sscanf(line, "%d,%d,%d,%d,%d", &temp, &humidity, &pressure, &moisture, &meas_time);
			//check to see that we scanned the right amount of things in
			// if (scanned == 5) {
			// 	printk("Successful DataReadings line read\n");
			// } else {
			// 	printk("Unsuccessful DataReadings line read\n");
			// }
			//populate the sensorNodes struct
			add_reading(sensor, temp, humidity, pressure, moisture, meas_time);
			line = strtok(NULL, "\n");
		}
	}

	fs_close(readings);

	return 0;
}

/*************************************************************************
 * Truncates the Sensor Node's readings file, deleting old values
 * 
 * @param file The file where past sensor readings are stored
 * @param full_path The full path name of the file storing past readings
 * 
 * @return 0 if successful, -1 if unsuccessful
 **************************************************************************/

int sensor_lfs_file_truncate(void* file, char *full_path) {
	
	struct fs_file_t *readings = file;
	int rc;

	//Open the file for reading and writing
	rc = fs_open(readings, full_path, FS_O_RDWR);
	if (rc < 0) {
		LOG_ERR("failed to open file while truncating\n");
		return -1;
	}

	//Truncate the file to 0 bytes
	rc = fs_truncate(readings, 0);
	if (rc < 0) {
		LOG_ERR("failed to truncate file while truncating\n");
		return -1;
	}

	//close the file
	fs_close(readings);

	return 0;
}

/******************************************************************
 * Adds a DataReadings struct to a SensorNode struct, 
 * deleting old data if the maximum number of readings is exceeded
 * 
 * @param sensor The SensorNode struct that is being read into
 * @param temp New temperature reading
 * @param humidity New humidity reading
 * @param pressure New pressure reading
 * @param moisture New moisture reading
 * @param meas_time Time at which readings above were taken
 *******************************************************************/

static void add_reading(SensorNode *sensor, int32_t temp, int32_t humidity,
			int32_t pressure, int32_t moisture, int32_t meas_time)
{
	pb_size_t max = (pb_size_t)ARRAY_SIZE(sensor->readings);

	if (sensor->readings_count >= max) {
		/* Full — shift left to drop oldest, safe because we own the mutex */
		memmove(&sensor->readings[0], &sensor->readings[1],
			(max - 1) * sizeof(DataReadings));
		sensor->readings_count = max - 1;
	}

	pb_size_t i = sensor->readings_count;
	sensor->readings[i].temp      = temp;
	sensor->readings[i].humidity  = humidity;
	sensor->readings[i].pressure  = pressure;
	sensor->readings[i].moisture  = moisture;
	sensor->readings[i].meas_time = meas_time;
	sensor->readings_count++;

	// printk("Reading added (%d/%d)\n", sensor->readings_count, max);
}