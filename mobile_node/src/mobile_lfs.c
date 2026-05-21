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
#include "mobile_lfs.h"

static int littlefs_flash_erase(unsigned int id);
static int littlefs_mount(struct fs_mount_t *mp);
static void copy_mac(unsigned char *block_mac, unsigned char *new_mac);
static uint8_t mac_compare(unsigned char *block_mac, unsigned char *line_mac);
static void add_to_sensor_node(SensorNode* sensor, unsigned char *line_mac,
     int32_t ble_time, int32_t temp,
      int32_t humidity, int32_t pressure,
       int32_t moisture, int32_t meas_time);


LOG_MODULE_REGISTER(main);

/**
 * ============================================================
 * Initialises the LittleFS architecture for the mobile node
 * 
 * @param mp Pointer to a mountpoint struct for the mobile node
 * 
 * @return 0 if successful, -1 if unsuccessful
 =============================================================*/

int mobile_lfs_init(void* mp) {

    //Cast the mountpoint to the correct type
    struct fs_mount_t *mountpoint = mp;

    int rc;

    //mount the file system
    rc = littlefs_mount(mountpoint);
    if (rc < 0) {
        LOG_ERR("Failed to mount Mobile Node LittleFS mountpoint\n");
        return -1;
    }

    return 0;
}

/**
 * ==========================================================================================
 * Initialises a any file that is to be made in the mobile LittleFS
 * 
 * @param mp Pointer to the mobile node's mountpoint struct
 * @param file Pointer to the desired file struct to be initialised
 * @param filename buffer containing name for the initialised file
 * @param filepath Buffer to which the entire path for the newly initialised file will be sent
 * 
 * @return 0 if successful, -1 if unsuccessful
 ============================================================================================*/

int mobile_lfs_file_init(void* mp, void* file, char* filename, char* filepath) {
    
    int rc;

    //cast the mountpoint to the correct type
    struct fs_mount_t *mountpoint = mp;

    //cast the file to the correct type
    struct fs_file_t *doc = file;

    //Buffer for the full path
    char path[MAX_PATH_LEN];

    //Write to path the full path name of this file
    snprintf(path, sizeof(path), "%s/%s", mountpoint->mnt_point, filename);

    //initialise the file
    fs_file_t_init(doc);

    //open the file
    rc = fs_open(doc, path, (FS_O_RDWR | FS_O_CREATE));

    if (rc < 0) {
        LOG_ERR("Error opening a file during initiation for Mobile Node\n");
        return -1;
    }

    //put the path name created into the pointer for the path name in outside world
    snprintf(filepath, MAX_PATH_LEN, "%s", path);

    rc = fs_close(doc);

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

	LOG_PRINTK("Area %u at 0x%x on %s for %u bytes\n",
		   id, (unsigned int)pfa->fa_off, pfa->fa_dev->name,
		   (unsigned int)pfa->fa_size);

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
		LOG_PRINTK("FAIL: mount id %" PRIuPTR " at %s: %d\n",
		       (uintptr_t)mp->storage_dev, mp->mnt_point, rc);
		return rc;
	}
	LOG_PRINTK("%s mount: %d\n", mp->mnt_point, rc);

	return 0;
}

/**
 * ========================================================================
 * Writes information from an AllConfigs struct received from
 * the base node to a file
 * 
 * @param cf Pointer to the config file's struct
 * @param path Buffer containing the config file's full filepath
 * @param all_cnfgs The AllConfigs struct whose information will be written
 * to the conig file
 * 
 * @return 0 if successful, -1 if unsuccessful
 ==========================================================================*/

int mobile_lfs_config_write(void* cf, char* path, void* all_cnfgs) {
    
    int rc;
    //case the confige file to to true form
    struct fs_file_t *config_file = cf;
    //cast the AllConfigs struct to true form
    AllConfigs *all_configs = all_cnfgs;

    //open the file
    rc = fs_open(config_file, path, FS_O_WRITE);
    if (rc < 0) {
        LOG_ERR("Failed to open %s while writing\n", path);
        return -1;
    }

    //truncate the file, to get rid of old configs
    rc = fs_truncate(config_file, 0);
	if (rc < 0) {
		LOG_ERR("failed to truncate %s while writing\n", path);
		return -1;
	}

    //retrieve the number of items in the struct currently
    int num_configs = (int) all_configs->configs_count;
    if (num_configs == 0) {
        LOG_ERR("AllCOnfigs struct empty while writing to %s\n", path);
        return -1;
    }

    //loop through the number of configs within the AllConfigs Struct
    for (int i = 0; i < num_configs; i++) {
        //make a buffer to write into
        char config_buffer[CONFIGS_CSV_LINE_LENGTH];
        
        //grab the current SensorConfig Struct
        SensorConfig conf = all_configs->configs[i];
        //send the elements in each struct to the above buffer
        snprintf(config_buffer,
            sizeof(config_buffer),
            "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX,%d,%0*d,%0*d\n",
            conf.mac_address[0],
            conf.mac_address[1],
            conf.mac_address[2],
            conf.mac_address[3],
            conf.mac_address[4],
            conf.mac_address[5],
            conf.automate,
            INT_32_MAX_WIDTH, conf.water_period,
            INT_32_MAX_WIDTH, conf.water_trigger);
        
        //write the buffer to the file
        if (fs_write(config_file, (void *)config_buffer, strlen(config_buffer)) < 0) {
            LOG_ERR("Error when writing data to config file\n");
            return -1;
        }
    
    }

    //close the file
    fs_close(config_file);

    return 0;
}


/**
 * ========================================================================
 * Reads data from the mobile node's config file into a
 * SensorConfig struct. The config information read to this struct
 * will match the config for a particular sensor node that has been
 * connected to via GATT BLE
 * 
 * @param cf Pointer to the config file's struct
 * @param path Buffer containing the config file's full filepath
 * @param mac_buf Buffer containing the mac address of the sensor
 * node that hass been connected to by the mobile node
 * @param dest_config Pointer to a SensorConfig struct to which information
 * from the file shall be written to
 * 
 * @return 0 if successful, -1 if unsuccessful
 ==========================================================================*/

int mobile_lfs_config_read(void* cf, char* path, void* mac_buf, void* dest_config) {
    
    int rc;
    //cast the config file to true type
    struct fs_file_t *config_file = cf;
    //cast the MAC address buffer to true type
    pb_byte_t *mac_address = mac_buf;
    //cast the SensorConfig struct to its true form
    SensorConfig *config = dest_config;

    //open the file for reading
    rc = fs_open(config_file, path, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("Failed to open %s for reading\n", path);
    }

    //buffer to read into each time a read is called
    char read_buf[CONFIGS_CSV_LINE_LENGTH];

    //read through the file
    while (1) {

        //make a read
        ssize_t len = fs_read(config_file, read_buf, sizeof(read_buf) - 1);

        //check to see if we are at the end of the file
        if (len <= 0) {
            //break out of the reading loop
            break;
        }

        //insert the null pointer at the end of the read buffer
        read_buf[len] = '\0';

        //split the read into lines
        char* line = strtok(read_buf, "\n");

        unsigned char mac[CONFIG_MAC_BYTES];
        int automate;
        int32_t water_period;
        int32_t trigger;

        //while loop on the line
        while (line != NULL) {
            int scanned = sscanf(line, "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX,%d,%d,%d",
                 &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5],
                  &automate, &water_period, &trigger);
            //check to see that the correct amount of things were scanned
            if (scanned == 9) {
                uint8_t mac_same = 1;
                //Check to see if the mac address is the same
                for (int i = 0; i < CONFIG_MAC_BYTES; i++) {
                    if (mac[i] != (uint8_t)mac_address[i]) {
                        mac_same = 0;
                    }
                } 

                //check to see if the mac address was equal
                if (mac_same) {
                    LOG_INF("MAC address match in config file: %d:%d:%d:%d:%d:%d\n",
                         mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
                    
                    //set the config values to be the ones we found
                    config->automate = (bool) automate;
                    config->water_period = water_period;
                    config->water_trigger = trigger;

                    //close the file
                    rc = fs_close(config_file);
                    if (rc < 0) {
                        LOG_ERR("Failed to close config file on successful MAC match\n");   
                        return rc;
                    }

                    //return out of the function, success
                    return 0;

                }
            }
            line = strtok(NULL, "\n");
        }

    }

    rc = fs_close(config_file);
    if (rc < 0) {
        LOG_ERR("Failed to close config file after unsuccessful MAC search\n");
        return rc;
    }
    //The function returns by default if it does not find a config for the node
    return -1;
}

/**
 * ======================================================================================
 * Writes information from a SensorNode struct received from a Sensor Node
 * into a file on the mobile node
 * 
 * @param rf Pointer to the mobile node's readings file
 * @param path Buffer containing readings file's full filepath
 * @param sensor_node Pointer to a SensorNode struct received from a connected Sensor Node
 * 
 * @return 0 if successful, -1 if unsuccessful
 ========================================================================================*/

int mobile_lfs_write_sensor_readings(void* rf, char* path, void* sensor_node) {
    
    int rc;
    
    //cast the readings file to true form
    struct fs_file_t *readings_file = rf;
    //cast the SensorNode struct to its true form
    SensorNode *sensor = sensor_node;

    //open the file for writing
    rc = fs_open(readings_file, path, (FS_O_WRITE | FS_O_APPEND));
    if (rc < 0) {
        LOG_ERR("Failed to open %s while writing readings\n", path);
        return rc;
    }

    printk("number of readings to write: %d\n", sensor->readings_count);

    //loop through the dataReadings struct contained in this SensorNode
    for (int i = 0; i < (int)sensor->readings_count; i++) {
        printk("In the for loop for writing to readings file\n");
        //buffer for a single csv line of readings for this node
        char readings_buffer[READINGS_CSV_LINE_LENGTH];
        
        //grab the current DataReadings struct we are at
        DataReadings data = sensor->readings[i];

        //write to the buffer all info relevant to this node
        snprintf(readings_buffer, sizeof(readings_buffer),
            "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX,%0*d,%0*d,%0*d,%0*d,%0*d,%0*d\n",
            (uint8_t)sensor->mac_address[0],
            (uint8_t)sensor->mac_address[1],
            (uint8_t)sensor->mac_address[2],
            (uint8_t)sensor->mac_address[3],
            (uint8_t)sensor->mac_address[4],
            (uint8_t)sensor->mac_address[5],
            INT_32_MAX_WIDTH, sensor->ble_time,
            INT_32_MAX_WIDTH, data.temp,
            INT_32_MAX_WIDTH, data.humidity,
            INT_32_MAX_WIDTH, data.pressure,
            INT_32_MAX_WIDTH, data.moisture,
            INT_32_MAX_WIDTH, data.meas_time);

        if (fs_write(readings_file, (void *)readings_buffer, strlen(readings_buffer)) < 0) {
            printk("Error when writing data to Node Readings file\n");
		    return -1;
        }

        printk("Line written: %s\n", readings_buffer);
    }
    //close the file
    fs_close(readings_file);
    return 0;
}

/**
 * ==============================================================================
 * Reads information from the mobile node's readings file into a
 * Nodes struct which will be sent to the base node. 
 * 
 * @param rf Pointer to the mobile node's readings file struct
 * @param path The full filepath of the readings file
 * @param all_nodes Pointer to a Nodes struct that is to be sent to the base node
 * 
 * @return 0 if successful, -1 if unsuccessful
 ================================================================================*/

int mobile_lfs_read_sensor_readings(void* rf, char* path, void* all_nodes) {
    
    int rc;
    
    //case the readings file back to a file type
    struct fs_file_t *readings_file = rf;
    //cast the Nodes struct to type
    Nodes *nodes_collection = all_nodes;

    //open the file for reading
    rc = fs_open(readings_file, path, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("Failed to open %s for reading\n", path);
        return rc;
    }

    
    //Flag for first read
    uint8_t first_read = 1;

    //Buffer to read into
    char readings_buffer[READINGS_CSV_LINE_LENGTH];

    //struct to put sensor node data into
    SensorNode sensor = SensorNode_init_zero;

    //MAC address of current block of readings
    unsigned char block_mac[CONFIG_MAC_BYTES];

    //fields to read data from file to
	unsigned char line_mac[CONFIG_MAC_BYTES];
    int32_t ble_time = 0;
    int32_t temp = 0;
	int32_t humidity = 0;
	int32_t pressure = 0;
	int32_t moisture = 0;
	int32_t meas_time = 0;

    //now we need to start reading from the file
    while (1) {

        //get a reading from the file
		ssize_t len = fs_read(readings_file, readings_buffer, sizeof(readings_buffer) - 1);
        printk("After the read call\n");
        //check to see if we have reached the end of the file
        if (len <= 0) {

            if (len < 0) {
                printk("Reading error\n");
                return -1;
            }
            printk("In EOF condition\n");
            //add the last sensor node to the Nodes struct
            //put the last version of the SensorNode into the Nodes struct
            nodes_collection->nodes[nodes_collection->nodes_count] = sensor;
            printk("After the final nodes assignation\n");
            //Increase the number of nodes added to the overall Nodes struct
            nodes_collection->nodes_count++;
            printk("After the increment of number of nodes in the Nodes struct\n");
            break;
        }

        //insert the null terminator into the buffer
        readings_buffer[len] = '\0';

        //split the buffer into lines
        char* line = strtok(readings_buffer, "\n");

        while (line != NULL) {

            //scan the info from the buffer into elements
            int scanned = sscanf(line, "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX,%d,%d,%d,%d,%d,%d",
                &line_mac[0], &line_mac[1], &line_mac[2], &line_mac[3], &line_mac[4], &line_mac[5],
                &ble_time, &temp, &humidity, &pressure, &moisture, &meas_time
                );
            //check to see if we scanned the right number of things
            if (scanned == 12) {
                LOG_INF("Successfully scanned 12 elements to %s\n", path);
                printk("MAC: %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX, ble_time: %d, temp: %d, humidity: %d, pressure: %d, moisture: %d, meas_time: %d\n",
                line_mac[0], line_mac[1], line_mac[2], line_mac[3], line_mac[4], line_mac[5], ble_time,
                temp, humidity, pressure, moisture, meas_time);
                //check to see if it's the first time reading
                if (first_read) {
                    printk("In the first read block\n");
                    //turn off the flag
                    first_read = 0;
                    printk("Before the first copy_mac call in first read block\n");
                    //set the first repeated mac address to be the first seen
                    //memcpy(block_mac, line_mac, CONFIG_MAC_BYTES);
                    copy_mac(block_mac, line_mac);
                }
                printk("After first_read block\n");

                //check to see if the line mac and the block mac are the same
                if (mac_compare(block_mac, line_mac)) {
                    printk("Inside of the mac_compare return check\n");
                    add_to_sensor_node(&sensor, line_mac, ble_time,
                         temp, humidity, pressure, moisture, meas_time);
                    printk("After the first add to sensor node call\n");
                } else {
                    printk("Other mac address seen, in new_mac section\n");
                    //put the last version of the SensorNode into the Nodes struct
                    nodes_collection->nodes[nodes_collection->nodes_count] = sensor;
                    //Increase the number of nodes added to the overall Nodes struct
                    nodes_collection->nodes_count++;
                    //reinitialise the SensorNode struct for a new mac address
                    sensor = (SensorNode)SensorNode_init_zero;
                    //set the block address to be the new line mac seen
                    //memcpy(block_mac, line_mac, CONFIG_MAC_BYTES);
                    copy_mac(block_mac, line_mac);
                    //copy across data
                    add_to_sensor_node(&sensor, line_mac, ble_time,
                         temp, humidity, pressure, moisture, meas_time);
                }
                
            } else {
                LOG_ERR("Failed, scanned %d elements to %s\n", scanned, path);
                return -1;
            }
            printk("Before the NULL strtok call\n");
            line = strtok(NULL, "\n");
        }
    }

    rc = fs_close(readings_file);
    if (rc < 0) {
        LOG_ERR("Failed to close %s after reading\n", path);
    }

    return 0;
}

/**
 * =====================================
 * Fills a SensorNode struct
 =====================================*/

static void add_to_sensor_node(SensorNode* sensor, unsigned char *line_mac,
     int32_t ble_time, int32_t temp,
      int32_t humidity, int32_t pressure,
       int32_t moisture, int32_t meas_time) {

    printk("line MAC: %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX\n", 
    line_mac[0], line_mac[1], line_mac[2], line_mac[3],
    line_mac[4], line_mac[5]);
    //put the MAC address into the SensorNode
    copy_mac(sensor->mac_address, line_mac);
    //memcpy(sensor->mac_address, line_mac, CONFIG_MAC_BYTES);
    printk("MAC inside SensorNode: %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX\n",
    sensor->mac_address[0], sensor->mac_address[1],
    sensor->mac_address[2], sensor->mac_address[3],
    sensor->mac_address[4], sensor->mac_address[5]);
    //put the ble time into the SensorNode
    sensor->ble_time = ble_time;
    //put the information from this line into the SensorNode struct
    //make a DataReadings struct
    DataReadings line_data = DataReadings_init_zero;
    //put the line data into the above struct
    line_data.temp = temp;
    line_data.humidity = humidity;
    line_data.pressure = pressure;
    line_data.moisture = moisture;
    line_data.meas_time = meas_time;
    //put this struct into the SensorNode
    sensor->readings[sensor->readings_count] = line_data;
    //increase the count of readings in this SensorNode
    sensor->readings_count++;
}

/**
 * =====================================================================
 * Copies one MAC address to another MAC address
 * 
 * @param block_mac The MAC address for a block of readings in a file
 * @param new_mac A new MAC address seen in a line of readings in a file
 ====================================================================== */

static void copy_mac(unsigned char *block_mac, unsigned char *new_mac) {
    for (int i = 0; i < CONFIG_MAC_BYTES; i++) {
        block_mac[i] = new_mac[i];
    }
}

/**
 * ===============================================
 * Compares whether two MAC addresses are the same
 ================================================*/

static uint8_t mac_compare(unsigned char *block_mac, unsigned char *line_mac) {
    
    uint8_t result = 1;

    for (int i = 0; i < CONFIG_MAC_BYTES; i++) {
        if (block_mac[i] != line_mac[i]) {
            result = 0;
            break;
        }
    }

    return result;
}