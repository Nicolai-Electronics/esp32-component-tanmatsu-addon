#pragma once

#include <stdint.h>
#include "esp_err.h"

// Add-on descriptor

typedef enum {
    ADDON_LOCATION_INTERNAL = 0x00,  // Internal add-on
    ADDON_LOCATION_CATT     = 0x01,  // CATT addon
    ADDON_LOCATION_SAO      = 0x02,  // SAO addon
} addon_location_t;

typedef enum {
    ADDON_TYPE_UNINITIALIZED = 0,  // Uninitialized EEPROM
    ADDON_TYPE_UNKNOWN       = 1,  // Unknown magic value
    ADDON_TYPE_BINARY_SAO    = 2,  // LIFE (https://badge.team/docs/standards/sao/binary_descriptor)
    ADDON_TYPE_JSON          = 3,  // JSON (https://github.com/urish/badge-addon-id)
    ADDON_TYPE_HEXPANSION    = 4,  // THEX (https://tildagon.badge.emfcamp.org/hexpansions/eeprom)
    ADDON_TYPE_CATT          = 5,  // CATT (Nicolai Electronics add-on descriptor)
} addon_descriptor_type_t;

typedef struct {
    char*    name;
    uint8_t  data_length;
    uint8_t* data;
} addon_binary_sao_driver_t;

typedef struct {
    addon_location_t        location;
    addon_descriptor_type_t descriptor_type;
    union {
        struct {
            // Binary SAO specific fields
            char*                      name;
            uint8_t                    amount_of_drivers;
            addon_binary_sao_driver_t* drivers;
        } binary_sao;
        struct {
            // JSON specific fields
            char* json_text;
        } json;
        struct {
            // CATT / Hexpansion specific fields
            char manifest_version[4];
            struct {
                uint16_t offset;
                uint16_t page_size;
                uint32_t total_size;
            } filesystem_info;
            uint16_t vendor_id;
            uint16_t product_id;
            uint16_t unique_id;
            char     name[15 + sizeof('\0')];
        } catt;
    };
} addon_descriptor_t;

// Functions

void addon_read_descriptor(addon_location_t location);

addon_descriptor_t* addon_get_descriptor(addon_location_t location);
esp_err_t           addon_set_descriptor(addon_location_t location, addon_descriptor_t* descriptor);
