#include <m5os/app_cleanup.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
static app_cleanup_fn s_hooks[APP_CLEANUP_MAX];
static unsigned s_count;
static bool s_sealed;
static mutex_t s_lock=MUTEX_INIT;
int app_cleanup_register(const char *name,app_cleanup_fn fn)
{
    if(s_sealed) return ERR_BUSY;
    if(!name || !*name || !fn) return ERR_INVAL;
    for(unsigned i=0;i<s_count;i++) if(s_hooks[i]==fn) return ERR_EXIST;
    if(s_count==APP_CLEANUP_MAX) return ERR_NOSPC;
    s_hooks[s_count++]=fn; return ERR_OK;
}
void app_cleanup_seal(void) { s_sealed=true; }
int app_cleanup_run(uint32_t owner)
{
    if(!s_sealed || !owner) return ERR_INVAL;
    if(!mutex_trylock(&s_lock)) return ERR_BUSY;
    int result=ERR_OK;
    for(unsigned i=0;i<s_count;i++) {
        int rc=s_hooks[i](owner);
        if(result==ERR_OK && rc!=ERR_OK) result=rc;
    }
    mutex_unlock(&s_lock); return result;
}
