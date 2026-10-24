#pragma once
#include <m5os/types.h>
#define HEALTH_MAX 8u
typedef struct { uint32_t handle,owner,timeout_ms; uint64_t last_us; char name[16]; } health_info_t;
/* Explicit progress leases: sleeping/idle tasks need not subscribe. Handle 0 is invalid. */
uint32_t health_register(const char *name,uint32_t owner,uint32_t timeout_ms);
bool health_renew(uint32_t handle);
void health_release_owner(uint32_t owner); /* only after successful driver teardown */
uint32_t health_expired(void); /* first stale handle, or 0 */
unsigned health_snapshot(health_info_t *out,unsigned max);
void health_start(void);
int health_command(int argc,char **argv);
/* Bare-metal HAL, no FreeRTOS watchdog APIs. Start is explicitly build-opt-in. */
bool hal_watchdog_start(uint32_t timeout_ms);
void hal_watchdog_feed(void);
