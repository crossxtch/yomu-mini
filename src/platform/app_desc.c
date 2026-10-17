// The prebuilt libesp_app_format.a defines a weak esp_app_desc that still
// says crossjp. This strong copy is the one the image boots with.
// The eFuse range and MMU page size match that prebuilt descriptor.
#include "esp_app_desc.h"

#ifndef YOMU_VERSION
#define YOMU_VERSION "dev"
#endif

const __attribute__((section(".rodata_desc"))) esp_app_desc_t esp_app_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
    .secure_version = 0,
    .version = YOMU_VERSION,
    .project_name = "yomu",
    .idf_ver = "5.5.2",
    .time = __TIME__,
    .date = __DATE__,
    .min_efuse_blk_rev_full = 0,
    .max_efuse_blk_rev_full = 199,
    .mmu_page_size = 16,
};
