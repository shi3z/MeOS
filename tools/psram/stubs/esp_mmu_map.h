#define MMU_TARGET_PSRAM0 0
#define MMU_VADDR_DATA 0
#define ESP_OK 0
int esp_mmu_paddr_to_vaddr(unsigned p, int target, int type, void **out);
