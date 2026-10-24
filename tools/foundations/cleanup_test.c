#include <assert.h>
#include <stdio.h>
#include <m5os/app_cleanup.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
static unsigned calls; static bool fail=true,locked;
bool mutex_trylock(mutex_t *m) { (void)m; if(locked)return false;locked=true;return true; }
void mutex_unlock(mutex_t *m) { (void)m; assert(locked);locked=false; }
static int first(uint32_t id) { assert(id==42);calls=calls*10+1;return fail?ERR_IO:ERR_OK; }
static int second(uint32_t id) { assert(id==42);calls=calls*10+2; assert(app_cleanup_run(42)==ERR_BUSY);return ERR_OK; }
int main(void) {
    assert(app_cleanup_run(42)==ERR_INVAL);
    assert(app_cleanup_register("",first)==ERR_INVAL);
    assert(app_cleanup_register("first",NULL)==ERR_INVAL);
    assert(app_cleanup_register("first",first)==ERR_OK);
    assert(app_cleanup_register("duplicate",first)==ERR_EXIST);
    assert(app_cleanup_register("second",second)==ERR_OK);
    app_cleanup_seal();assert(app_cleanup_register("late",first)==ERR_BUSY);
    assert(app_cleanup_run(0)==ERR_INVAL);
    assert(app_cleanup_run(42)==ERR_IO && calls==12 && !locked);
    calls=0;fail=false;assert(app_cleanup_run(42)==ERR_OK && calls==12 && !locked);
    puts("cleanup: ordered hooks, failure propagation, complete stop attempts, retry and reentry PASS");
}
