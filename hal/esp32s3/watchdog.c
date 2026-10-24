#include <m5os/health.h>
/* The P4 watchdog clock/timing implementation is not assumed valid on S3. */
bool hal_watchdog_start(uint32_t timeout) { (void)timeout;return false; }
void hal_watchdog_feed(void) {}
