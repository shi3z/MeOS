#include <m5os/board_config.h>
#if M5OS_QUAKE
#include <m5os/sdroot.h>
const char *quake_storage_base(void){return sdroot_active()?"/assets/quake":"/sd";}
#endif
