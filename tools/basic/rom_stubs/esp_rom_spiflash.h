#pragma once
#include <stdint.h>
typedef int esp_rom_spiflash_result_t;
#define ESP_ROM_SPIFLASH_RESULT_OK 0
typedef struct {uint32_t chip_size;} esp_rom_spiflash_chip_t;
extern esp_rom_spiflash_chip_t g_rom_flashchip;
int esp_rom_spiflash_read(uint32_t,uint32_t *,int32_t);
int esp_rom_spiflash_write(uint32_t,const uint32_t *,int32_t);
int esp_rom_spiflash_erase_sector(uint32_t);
