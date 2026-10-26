#pragma once
#include <stddef.h>
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 2
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 4
int esp_cache_msync(void *,size_t,int);
