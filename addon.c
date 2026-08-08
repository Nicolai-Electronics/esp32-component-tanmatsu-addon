#include "addon.h"
#include "bsp/catt.h"
#include "bsp/i2c.h"
#include "bsp/sao.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "eeprom.h"
#include "esp_err.h"
#include "esp_log.h"
#include "include/addon.h"
#include "sdkconfig.h"

// Descriptor structures

typedef struct __attribute__((__packed__)) {
    uint8_t magic[4];
    uint8_t name_length;
    uint8_t driver_name_length;
    uint8_t driver_data_length;
    uint8_t number_of_extra_drivers;
} sao_binary_header_t;

typedef struct __attribute__((__packed__)) {
    uint8_t driver_name_length;
    uint8_t driver_data_length;
} sao_binary_extra_driver_t;

typedef struct __attribute__((__packed__)) {
    uint8_t magic[4];
    char    manifest_version[4];
    struct {
        uint8_t offset[2];
        uint8_t page_size[2];
        uint8_t total_size[4];
    } filesystem_info;
    uint8_t vendor_id[2];
    uint8_t product_id[2];
    uint8_t unique_id[2];
    char    name[9];
    uint8_t checksum;
} catt_header_t;

// Constants & variables

static const char TAG[] = "Add-on";

static addon_descriptor_t* internal_addon_descriptor = NULL;
static addon_descriptor_t* catt_addon_descriptor     = NULL;
static addon_descriptor_t* sao_addon_descriptor      = NULL;

// Helper functions

static esp_err_t addon_parse_binary_sao_descriptor(eeprom_configuration_t* eeprom, addon_descriptor_t* descriptor) {
    if (descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sao_binary_header_t header = {0};
    esp_err_t           res    = eeprom_read(eeprom, 0x00, (uint8_t*)&header, sizeof(sao_binary_header_t));
    if (res != ESP_OK) {
        return res;
    }

    uint8_t address = sizeof(sao_binary_header_t);

    descriptor->binary_sao.amount_of_drivers = 1 + header.number_of_extra_drivers;

    // Allocate memory
    descriptor->binary_sao.name = calloc(header.name_length + 1, sizeof(char));
    if (descriptor->binary_sao.name == NULL) {
        return ESP_ERR_NO_MEM;
    }
    descriptor->binary_sao.drivers =
        calloc(descriptor->binary_sao.amount_of_drivers, sizeof(addon_binary_sao_driver_t));
    if (descriptor->binary_sao.drivers == NULL) {
        free(descriptor->binary_sao.name);
        return ESP_ERR_NO_MEM;
    }

    // Read name
    res = eeprom_read(eeprom, address, (uint8_t*)descriptor->binary_sao.name, header.name_length);
    if (res != ESP_OK) {
        free(descriptor->binary_sao.name);
        free(descriptor->binary_sao.drivers);
        return res;
    }

    // Read primary driver data
    address                                += header.name_length;
    descriptor->binary_sao.drivers[0].name  = calloc(header.driver_name_length + 1, sizeof(char));
    if (descriptor->binary_sao.drivers[0].name == NULL) {
        free(descriptor->binary_sao.name);
        free(descriptor->binary_sao.drivers);
        return ESP_ERR_NO_MEM;
    }

    res = eeprom_read(eeprom, address, (uint8_t*)descriptor->binary_sao.drivers[0].name, header.driver_name_length);
    if (res != ESP_OK) {
        free(descriptor->binary_sao.name);
        free(descriptor->binary_sao.drivers[0].name);
        free(descriptor->binary_sao.drivers);
        return res;
    }

    address                                       += header.driver_name_length;
    descriptor->binary_sao.drivers[0].data_length  = header.driver_data_length;
    descriptor->binary_sao.drivers[0].data         = malloc(header.driver_data_length);
    if (descriptor->binary_sao.drivers[0].data == NULL) {
        free(descriptor->binary_sao.name);
        free(descriptor->binary_sao.drivers[0].name);
        free(descriptor->binary_sao.drivers);
        return ESP_ERR_NO_MEM;
    }

    res = eeprom_read(eeprom, address, descriptor->binary_sao.drivers[0].data, header.driver_data_length);
    if (res != ESP_OK) {
        free(descriptor->binary_sao.name);
        free(descriptor->binary_sao.drivers[0].name);
        free(descriptor->binary_sao.drivers[0].data);
        free(descriptor->binary_sao.drivers);
        return res;
    }

    address += header.driver_data_length;

    // Read extra drivers
    for (uint8_t i = 1; i < descriptor->binary_sao.amount_of_drivers; i++) {
        sao_binary_extra_driver_t extra_driver_header = {0};
        res = eeprom_read(eeprom, address, (uint8_t*)&extra_driver_header, sizeof(extra_driver_header));
        if (res != ESP_OK) {
            // Free previously allocated memory
            for (uint8_t j = 0; j <= i - 1; j++) {
                free(descriptor->binary_sao.drivers[j].name);
                free(descriptor->binary_sao.drivers[j].data);
            }
            free(descriptor->binary_sao.name);
            free(descriptor->binary_sao.drivers);
            return res;
        }
        address                                += sizeof(extra_driver_header);
        descriptor->binary_sao.drivers[i].name  = calloc(extra_driver_header.driver_name_length + 1, sizeof(char));
        if (descriptor->binary_sao.drivers[i].name == NULL) {
            // Free previously allocated memory
            for (uint8_t j = 0; j < i - 1; j++) {
                free(descriptor->binary_sao.drivers[j].name);
                free(descriptor->binary_sao.drivers[j].data);
            }
            free(descriptor->binary_sao.name);
            free(descriptor->binary_sao.drivers);
            return ESP_ERR_NO_MEM;
        }
        descriptor->binary_sao.drivers[i].data = malloc(extra_driver_header.driver_data_length);
        if (descriptor->binary_sao.drivers[i].data == NULL) {
            // Free previously allocated memory
            for (uint8_t j = 0; j < i - 1; j++) {
                free(descriptor->binary_sao.drivers[j].name);
                free(descriptor->binary_sao.drivers[j].data);
            }
            free(descriptor->binary_sao.drivers[i].name);
            free(descriptor->binary_sao.name);
            free(descriptor->binary_sao.drivers);
            return ESP_ERR_NO_MEM;
        }
        // Read extra driver name
        res      = eeprom_read(eeprom, address, (uint8_t*)descriptor->binary_sao.drivers[i].name,
                               extra_driver_header.driver_name_length);
        address += extra_driver_header.driver_name_length;
        if (res == ESP_OK) {
            // Read extra driver data
            descriptor->binary_sao.drivers[i].data_length = extra_driver_header.driver_data_length;
            res      = eeprom_read(eeprom, address, descriptor->binary_sao.drivers[i].data,
                                   extra_driver_header.driver_data_length);
            address += extra_driver_header.driver_data_length;
        }
        if (res != ESP_OK) {
            // Free previously allocated memory
            for (uint8_t j = 0; j <= i; j++) {
                free(descriptor->binary_sao.drivers[j].name);
                free(descriptor->binary_sao.drivers[j].data);
            }
            free(descriptor->binary_sao.name);
            free(descriptor->binary_sao.drivers);
            return res;
        }
    }

    descriptor->descriptor_type = ADDON_TYPE_BINARY_SAO;

    return ESP_OK;
}

static esp_err_t addon_parse_json_descriptor(eeprom_configuration_t* eeprom, addon_descriptor_t* descriptor) {
    if (descriptor == NULL || descriptor->descriptor_type != ADDON_TYPE_JSON) {
        return ESP_ERR_INVALID_ARG;
    }

    // Read the size of the JSON data
    uint8_t   json_size = 0;
    esp_err_t res       = eeprom_read(eeprom, 0x04, &json_size, sizeof(uint8_t));
    if (res != ESP_OK) {
        return res;
    }

    // Allocate memory for JSON data
    descriptor->json.json_text = calloc(json_size + 1, sizeof(char));  // +1 for null terminator
    if (descriptor->json.json_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Read JSON data
    res = eeprom_read(eeprom, 0x05, (uint8_t*)descriptor->json.json_text, json_size);
    if (res != ESP_OK) {
        free(descriptor->json.json_text);
        return res;
    }

    descriptor->descriptor_type = ADDON_TYPE_JSON;

    return ESP_OK;
}

static esp_err_t addon_parse_hexpansion_catt_descriptor(eeprom_configuration_t* eeprom,
                                                        addon_descriptor_t*     descriptor) {
    if (descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (descriptor->descriptor_type != ADDON_TYPE_HEXPANSION && descriptor->descriptor_type != ADDON_TYPE_CATT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Read manifest version
    catt_header_t header = {0};
    esp_err_t     res    = eeprom_read(eeprom, 0x00, (uint8_t*)&header, sizeof(catt_header_t));
    if (res != ESP_OK) return res;

    if (descriptor->descriptor_type == ADDON_TYPE_HEXPANSION && memcmp(header.manifest_version, "2024", 4) != 0) {
        ESP_LOGW(TAG, "THEX addon with unsupported manifest type '%c%c%c%c'", header.manifest_version[0],
                 header.manifest_version[1], header.manifest_version[2], header.manifest_version[3]);
        return ESP_ERR_NOT_SUPPORTED;
    } else if (descriptor->descriptor_type == ADDON_TYPE_CATT && memcmp(header.manifest_version, "0001", 4) != 0) {
        ESP_LOGW(TAG, "CATT addon with unsupported manifest type '%c%c%c%c'", header.manifest_version[0],
                 header.manifest_version[1], header.manifest_version[2], header.manifest_version[3]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t calculated_checksum = 0x55;
    for (size_t i = 1; i < sizeof(catt_header_t) - sizeof(uint8_t); i++) {
        calculated_checksum ^= ((uint8_t*)&header)[i];
    }
    if (calculated_checksum != header.checksum) {
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(descriptor->catt.manifest_version, header.manifest_version, 4);
    descriptor->catt.filesystem_info.offset =
        (header.filesystem_info.offset[0] << 8) | header.filesystem_info.offset[1];
    descriptor->catt.filesystem_info.page_size =
        (header.filesystem_info.page_size[0] << 8) | header.filesystem_info.page_size[1];
    descriptor->catt.filesystem_info.total_size =
        (header.filesystem_info.total_size[0] << 24) | (header.filesystem_info.total_size[1] << 16) |
        (header.filesystem_info.total_size[2] << 8) | header.filesystem_info.total_size[3];
    descriptor->catt.vendor_id  = (header.vendor_id[0] << 8) | header.vendor_id[1];
    descriptor->catt.product_id = (header.product_id[0] << 8) | header.product_id[1];
    descriptor->catt.unique_id  = (header.unique_id[0] << 8) | header.unique_id[1];
    memcpy(descriptor->catt.name, header.name, 9);

    return ESP_OK;
}

static void addon_free_if_allocated(addon_descriptor_t** descriptor) {
    if ((*descriptor) == NULL) {
        return;
    }
    if ((*descriptor)->descriptor_type == ADDON_TYPE_BINARY_SAO) {
        for (uint8_t i = 0; i < (*descriptor)->binary_sao.amount_of_drivers; i++) {
            free((*descriptor)->binary_sao.drivers[i].name);
            free((*descriptor)->binary_sao.drivers[i].data);
        }
        free((*descriptor)->binary_sao.drivers);
        free((*descriptor)->binary_sao.name);
    } else if ((*descriptor)->descriptor_type == ADDON_TYPE_JSON) {
        free((*descriptor)->json.json_text);
    }
    free((*descriptor));
    *descriptor = NULL;
}

static esp_err_t addon_detect(addon_location_t location, i2c_master_dev_handle_t device,
                              addon_descriptor_t** out_descriptor) {
    addon_free_if_allocated(out_descriptor);

    eeprom_configuration_t eeprom_config = {
        .device        = device,
        .page_size     = 16,
        .address_16bit = false,
        .timeout_ms    = 100,
    };

    char      magic[4] = "";
    esp_err_t res      = eeprom_read(&eeprom_config, 0, (uint8_t*)magic, sizeof(magic));
    if (res != ESP_OK) {
        return res;
    }

    *out_descriptor = calloc(1, sizeof(addon_descriptor_t));
    if (*out_descriptor == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (*out_descriptor)->location        = location;
    (*out_descriptor)->descriptor_type = ADDON_TYPE_UNKNOWN;

    if (memcmp(magic, "\0\0\0\0", sizeof(magic)) == 0 || memcmp(magic, "\xFF\xFF\xFF\xFF", sizeof(magic)) == 0) {
        (*out_descriptor)->descriptor_type = ADDON_TYPE_UNINITIALIZED;
    } else if (memcmp(magic, "LIFE", sizeof(magic)) == 0) {
        ESP_LOGI(TAG, "Found LIFE magic value in add-on EEPROM");
        (*out_descriptor)->descriptor_type = ADDON_TYPE_BINARY_SAO;
        addon_parse_binary_sao_descriptor(&eeprom_config, *out_descriptor);
    } else if (memcmp(magic, "JSON", sizeof(magic)) == 0) {
        ESP_LOGI(TAG, "Found JSON magic value in add-on EEPROM");
        (*out_descriptor)->descriptor_type = ADDON_TYPE_JSON;
        addon_parse_json_descriptor(&eeprom_config, *out_descriptor);
    } else if (memcmp(magic, "THEX", sizeof(magic)) == 0) {
        ESP_LOGI(TAG, "Found THEX magic value in add-on EEPROM");
        (*out_descriptor)->descriptor_type = ADDON_TYPE_HEXPANSION;
        addon_parse_hexpansion_catt_descriptor(&eeprom_config, *out_descriptor);
    } else if (memcmp(magic, "CATT", sizeof(magic)) == 0) {
        (*out_descriptor)->descriptor_type = ADDON_TYPE_CATT;
        ESP_LOGI(TAG, "Found CATT magic value in add-on EEPROM");
        addon_parse_hexpansion_catt_descriptor(&eeprom_config, *out_descriptor);
    } else {
        ESP_LOGI(TAG, "Unknown magic value in add-on EEPROM: '%c%c%c%c'", magic[0], magic[1], magic[2], magic[3]);
    }

    return ESP_OK;
}

// Public functions

esp_err_t addon_print_descriptor(addon_descriptor_t* descriptor) {
    if (descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char* location_str = descriptor->location == ADDON_LOCATION_INTERNAL ? "internal" : "external";
    const char* type_str     = "unknown";

    switch (descriptor->descriptor_type) {
        case ADDON_TYPE_UNINITIALIZED:
            type_str = "uninitialized";
            break;
        case ADDON_TYPE_BINARY_SAO:
            type_str = "binary SAO";
            break;
        case ADDON_TYPE_JSON:
            type_str = "JSON";
            break;
        case ADDON_TYPE_HEXPANSION:
            type_str = "Hexpansion";
            break;
        case ADDON_TYPE_CATT:
            type_str = "CATT";
            break;
        default:
            break;
    }

    ESP_LOGI(TAG, "%s %s add-on", location_str, type_str);

    if (descriptor->descriptor_type == ADDON_TYPE_BINARY_SAO) {
        ESP_LOGI(TAG, "  Name: %s", descriptor->binary_sao.name);
        ESP_LOGI(TAG, "  Number of drivers: %d", descriptor->binary_sao.amount_of_drivers);
        for (uint8_t i = 0; i < descriptor->binary_sao.amount_of_drivers; i++) {
            ESP_LOGI(TAG, "    Driver %d:", i + 1);
            ESP_LOGI(TAG, "      Name: %s", descriptor->binary_sao.drivers[i].name);
            ESP_LOGI(TAG, "      Data length: %d bytes", descriptor->binary_sao.drivers[i].data_length);
            ESP_LOGI(TAG, "      Data (hex):");
            printf("                ");
            for (uint8_t j = 0; j < descriptor->binary_sao.drivers[i].data_length; j++) {
                printf("%02X ", descriptor->binary_sao.drivers[i].data[j]);
            }
            printf("\r\n");
        }
    }

    if (descriptor->descriptor_type == ADDON_TYPE_JSON) {
        ESP_LOGI(TAG, "  JSON data: %s", descriptor->json.json_text);
    }

    if (descriptor->descriptor_type == ADDON_TYPE_HEXPANSION || descriptor->descriptor_type == ADDON_TYPE_CATT) {
        ESP_LOGI(TAG, "  Manifest version:      %.4s", descriptor->catt.manifest_version);
        ESP_LOGI(TAG, "  Name:                  %.9s", descriptor->catt.name);
        ESP_LOGI(TAG, "  Vendor ID:             0x%04X", descriptor->catt.vendor_id);
        ESP_LOGI(TAG, "  Product ID:            0x%04X", descriptor->catt.product_id);
        ESP_LOGI(TAG, "  Unique ID:             0x%04X", descriptor->catt.unique_id);
        ESP_LOGI(TAG, "  Filesystem offset:     0x%04X", descriptor->catt.filesystem_info.offset);
        ESP_LOGI(TAG, "  Filesystem page size:  %d bytes", descriptor->catt.filesystem_info.page_size);
        ESP_LOGI(TAG, "  Filesystem total size: %d bytes", descriptor->catt.filesystem_info.total_size);
    }

    return ESP_OK;
}

void addon_read_descriptor(addon_location_t location) {
    // Get and free add-on descriptor pointer
    addon_descriptor_t** descriptor = NULL;

    switch (location) {
        case ADDON_LOCATION_INTERNAL:
            descriptor = &internal_addon_descriptor;
            break;
        case ADDON_LOCATION_CATT:
            descriptor = &catt_addon_descriptor;
            break;
        case ADDON_LOCATION_SAO:
            descriptor = &sao_addon_descriptor;
            break;
        default:
            break;
    }

    if (descriptor == NULL) {
        return;
    }

    addon_free_if_allocated(descriptor);

    // Get I2C bus handle

    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    esp_err_t               res            = ESP_OK;

    switch (location) {
        case ADDON_LOCATION_INTERNAL: {
            res = bsp_i2c_primary_bus_get_handle(&i2c_bus_handle);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No internal add-on: I2C bus handle unavailable");
                return;
            }
            break;
        }
        case ADDON_LOCATION_CATT: {
            res = bsp_catt_set_i2c_enabled(true);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No CATT add-on: I2C bus unavailable");
                return;
            }
            res = bsp_catt_i2c_bus_get_handle(&i2c_bus_handle);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No CATT add-on: I2C bus handle unavailable");
                return;
            }
            break;
        }
        case ADDON_LOCATION_SAO: {
            res = bsp_sao_i2c_bus_get_handle(&i2c_bus_handle);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No SAO add-on: I2C bus handle unavailable");
                return;
            }
            i2c_master_bus_handle_t catt_bus_handle = NULL;
            bsp_catt_i2c_bus_get_handle(&catt_bus_handle);
            if (i2c_bus_handle == catt_bus_handle) {
                sao_addon_descriptor = catt_addon_descriptor;
                ESP_LOGI(TAG, "Skipping SAO add-on read, SAO port is CATT port on this device");
                return;
            }
            break;
        }
        default:
            break;
    }

    // Get I2C bus handle
    if (i2c_bus_handle == NULL) {
        return;
    }

    // Initialize EEPROM I2C device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x50,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t i2c_device_handle = NULL;
    res                                       = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &i2c_device_handle);
    if (res != ESP_OK || i2c_device_handle == NULL) {
        return;
    }

    res = addon_detect(location, i2c_device_handle, descriptor);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Add-on detected");
        addon_print_descriptor(*descriptor);
    } else {
        ESP_LOGW(TAG, "No add-on detected");
    }

    i2c_master_bus_rm_device(i2c_device_handle);
}

addon_descriptor_t* addon_get_descriptor(addon_location_t location) {
    if (location == ADDON_LOCATION_INTERNAL) {
        return internal_addon_descriptor;
    }
    if (location == ADDON_LOCATION_CATT) {
        return catt_addon_descriptor;
    }
    if (location == ADDON_LOCATION_SAO) {
        return sao_addon_descriptor;
    }
    return NULL;
}

esp_err_t addon_set_descriptor(addon_location_t location, addon_descriptor_t* descriptor) {
    if (descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get I2C bus handle

    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    esp_err_t               res            = ESP_OK;

    switch (location) {
        case ADDON_LOCATION_INTERNAL: {
            res = bsp_i2c_primary_bus_get_handle(&i2c_bus_handle);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No internal add-on: I2C bus handle unavailable");
                return ESP_FAIL;
            }
            break;
        }
        case ADDON_LOCATION_CATT: {
            res = bsp_catt_set_i2c_enabled(true);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No CATT add-on: I2C bus unavailable");
                return ESP_FAIL;
            }
            res = bsp_catt_i2c_bus_get_handle(&i2c_bus_handle);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No CATT add-on: I2C bus handle unavailable");
                return ESP_FAIL;
            }
            break;
        }
        case ADDON_LOCATION_SAO: {
            res = bsp_sao_i2c_bus_get_handle(&i2c_bus_handle);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "No SAO add-on: I2C bus handle unavailable");
                return ESP_FAIL;
            }
            i2c_master_bus_handle_t catt_bus_handle = NULL;
            bsp_catt_i2c_bus_get_handle(&catt_bus_handle);
            if (i2c_bus_handle == catt_bus_handle) {
                ESP_LOGI(TAG, "Writing to SAO add-on is not supported, SAO port is CATT port on this device");
                return ESP_ERR_NOT_SUPPORTED;
            }
            break;
        }
        default:
            break;
    }

    // Get I2C bus handle
    if (i2c_bus_handle == NULL) {
        return ESP_FAIL;
    }

    // Initialize EEPROM I2C device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x50,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t i2c_device_handle = NULL;
    res                                       = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &i2c_device_handle);
    if (res != ESP_OK || i2c_device_handle == NULL) {
        return ESP_FAIL;
    }

    eeprom_configuration_t eeprom_config = {
        .device        = i2c_device_handle,
        .page_size     = 16,
        .address_16bit = false,
        .timeout_ms    = 100,
    };

    switch (descriptor->descriptor_type) {
        case ADDON_TYPE_BINARY_SAO: {
            if (descriptor->binary_sao.amount_of_drivers == 0 || descriptor->binary_sao.name == NULL ||
                descriptor->binary_sao.drivers == NULL || descriptor->binary_sao.drivers[0].name == NULL) {
                return ESP_ERR_INVALID_ARG;
            }

            sao_binary_header_t header = {
                .magic                   = {'L', 'I', 'F', 'E'},
                .name_length             = (uint8_t)strlen(descriptor->binary_sao.name),
                .driver_name_length      = (uint8_t)strlen(descriptor->binary_sao.drivers[0].name),
                .driver_data_length      = descriptor->binary_sao.drivers[0].data_length,
                .number_of_extra_drivers = descriptor->binary_sao.amount_of_drivers - 1,
            };

            uint16_t address = 0;

            res = eeprom_write(&eeprom_config, address, (uint8_t*)&header, sizeof(header));
            if (res != ESP_OK) return res;
            address += sizeof(header);

            if (header.name_length > 0) {
                res = eeprom_write(&eeprom_config, address, (uint8_t*)descriptor->binary_sao.name, header.name_length);
                if (res != ESP_OK) return res;
            }
            address += header.name_length;

            if (header.driver_name_length > 0) {
                res = eeprom_write(&eeprom_config, address, (uint8_t*)descriptor->binary_sao.drivers[0].name,
                                   header.driver_name_length);
                if (res != ESP_OK) return res;
            }
            address += header.driver_name_length;

            if (header.driver_data_length > 0) {
                res = eeprom_write(&eeprom_config, address, descriptor->binary_sao.drivers[0].data,
                                   header.driver_data_length);
                if (res != ESP_OK) return res;
            }
            address += header.driver_data_length;

            for (uint8_t i = 1; i < descriptor->binary_sao.amount_of_drivers; i++) {
                if (descriptor->binary_sao.drivers[i].name == NULL) {
                    return ESP_ERR_INVALID_ARG;
                }
                sao_binary_extra_driver_t extra_header = {
                    .driver_name_length = (uint8_t)strlen(descriptor->binary_sao.drivers[i].name),
                    .driver_data_length = descriptor->binary_sao.drivers[i].data_length,
                };

                res = eeprom_write(&eeprom_config, address, (uint8_t*)&extra_header, sizeof(extra_header));
                if (res != ESP_OK) return res;
                address += sizeof(extra_header);

                if (extra_header.driver_name_length > 0) {
                    res = eeprom_write(&eeprom_config, address, (uint8_t*)descriptor->binary_sao.drivers[i].name,
                                       extra_header.driver_name_length);
                    if (res != ESP_OK) return res;
                }
                address += extra_header.driver_name_length;

                if (extra_header.driver_data_length > 0) {
                    res = eeprom_write(&eeprom_config, address, descriptor->binary_sao.drivers[i].data,
                                       extra_header.driver_data_length);
                    if (res != ESP_OK) return res;
                }
                address += extra_header.driver_data_length;
            }
            break;
        }

        case ADDON_TYPE_JSON: {
            if (descriptor->json.json_text == NULL) {
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t json_size = (uint8_t)strlen(descriptor->json.json_text);
            uint8_t magic[4]  = {'J', 'S', 'O', 'N'};

            res = eeprom_write(&eeprom_config, 0x00, magic, sizeof(magic));
            if (res != ESP_OK) return res;

            res = eeprom_write(&eeprom_config, 0x04, &json_size, sizeof(json_size));
            if (res != ESP_OK) return res;

            if (json_size > 0) {
                res = eeprom_write(&eeprom_config, 0x05, (uint8_t*)descriptor->json.json_text, json_size);
                if (res != ESP_OK) return res;
            }
            break;
        }

        case ADDON_TYPE_HEXPANSION:
        case ADDON_TYPE_CATT: {
            catt_header_t header = {0};

            if (descriptor->descriptor_type == ADDON_TYPE_HEXPANSION) {
                memcpy(header.magic, "THEX", 4);
                memcpy(header.manifest_version, "2024", 4);
            } else {
                memcpy(header.magic, "CATT", 4);
                memcpy(header.manifest_version, "0001", 4);
            }
            header.filesystem_info.offset[0]     = (descriptor->catt.filesystem_info.offset >> 8) & 0xFF;
            header.filesystem_info.offset[1]     = descriptor->catt.filesystem_info.offset & 0xFF;
            header.filesystem_info.page_size[0]  = (descriptor->catt.filesystem_info.page_size >> 8) & 0xFF;
            header.filesystem_info.page_size[1]  = descriptor->catt.filesystem_info.page_size & 0xFF;
            header.filesystem_info.total_size[0] = (descriptor->catt.filesystem_info.total_size >> 24) & 0xFF;
            header.filesystem_info.total_size[1] = (descriptor->catt.filesystem_info.total_size >> 16) & 0xFF;
            header.filesystem_info.total_size[2] = (descriptor->catt.filesystem_info.total_size >> 8) & 0xFF;
            header.filesystem_info.total_size[3] = descriptor->catt.filesystem_info.total_size & 0xFF;
            header.vendor_id[0]                  = (descriptor->catt.vendor_id >> 8) & 0xFF;
            header.vendor_id[1]                  = descriptor->catt.vendor_id & 0xFF;
            header.product_id[0]                 = (descriptor->catt.product_id >> 8) & 0xFF;
            header.product_id[1]                 = descriptor->catt.product_id & 0xFF;
            header.unique_id[0]                  = (descriptor->catt.unique_id >> 8) & 0xFF;
            header.unique_id[1]                  = descriptor->catt.unique_id & 0xFF;
            memcpy(header.name, descriptor->catt.name, sizeof(header.name));

            uint8_t checksum = 0x55;
            for (size_t i = 1; i < sizeof(catt_header_t) - 1; i++) {
                checksum ^= ((uint8_t*)&header)[i];
            }
            header.checksum = checksum;

            printf("Writing %u bytes\r\n", sizeof(catt_header_t));
            res = eeprom_write(&eeprom_config, 0x00, (uint8_t*)&header, sizeof(catt_header_t));
            break;
        }

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    return res;
}
