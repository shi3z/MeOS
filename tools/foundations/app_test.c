#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <m5os/app.h>
#include <m5os/app_cleanup.h>
#include <m5os/health.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/arch.h>
#include <m5os/errno.h>
extern app_t *app_reserve(const char *,uint32_t);
extern int app_launch(app_t *,void (*)(void *),void *,uint8_t,size_t);
static task_t current;
static void (*appd)(void *);
static unsigned depth,sleeps,freed[APP_MAX],stopped[APP_MAX],released[APP_MAX];
static bool fail=true,locked;
static jmp_buf finished;
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
task_t *task_current(void){return &current;}
const app_def_t *apps_def_table(int *n){*n=0;return NULL;}
int task_snapshot(task_info_t *out,int n){(void)out;(void)n;return 0;}
int task_kill(uint32_t id){(void)id;return ERR_OK;}
int task_kill_app(const struct app *app){assert(!freed[app->app_id]);return 0;}
int task_create_for_app(const char *name,void (*entry)(void *),void *arg,uint8_t p,size_t s,struct app *a){
    (void)arg;(void)p;(void)s;(void)a;if(!strcmp(name,"appd")){appd=entry;return 2;}return ERR_NOMEM;
}
void task_sleep(uint32_t n){(void)n;if(++sleeps==2)longjmp(finished,1);}
bool mutex_trylock(mutex_t *m){(void)m;if(locked)return false;locked=true;return true;}
void mutex_unlock(mutex_t *m){(void)m;assert(locked);locked=false;}
static int stop(uint32_t id){assert(!freed[id]);stopped[id]++;return fail?ERR_IO:ERR_OK;}
void app_cleanup_register_builtins(void){assert(app_cleanup_register("driver",stop)==ERR_OK);app_cleanup_seal();}
uint32_t health_register(const char *n,uint32_t owner,uint32_t timeout){(void)n;assert(!owner&&timeout==5000);return 1;}
bool health_renew(uint32_t id){assert(id==1);return true;}
void health_release_owner(uint32_t id){assert(stopped[id]&&!freed[id]);released[id]++;}
size_t kmem_reclaim_owner(uint32_t id){assert(stopped[id]&&released[id]&&!fail);freed[id]++;return 128;}
size_t kmem_owned_bytes(uint32_t id){return freed[id]?0:128;}
void kernel_panic(const char *s){fprintf(stderr,"unexpected panic %s\n",s);assert(0);__builtin_trap();}
static void entry(void *p){(void)p;}
int main(void){
    app_init();assert(appd);
    app_t *a=app_reserve("partial",APP_PERM_ALL);assert(a);
    app_abort(a);assert(a->state==APP_KILLED&&a->cleanup_pending&&a->cleanup_error==ERR_IO&&!freed[a->app_id]);
    app_t *b=app_reserve("launch-failure",APP_PERM_ALL);assert(b&&b!=a);
    assert(app_launch(b,entry,NULL,1,4096)==ERR_NOMEM);
    assert(b->state==APP_KILLED&&b->cleanup_pending&&!freed[b->app_id]);
    fail=false;
    if(!setjmp(finished))appd(NULL);
    assert(a->cleaned&&b->cleaned&&!a->cleanup_pending&&!b->cleanup_pending);
    assert(freed[a->app_id]==1&&freed[b->app_id]==1&&a->reclaimed_bytes==128);
    assert(!a->cleanup_error&&!b->cleanup_error);
    assert(!depth&&!locked);
    puts("app: partial/failed-launch cleanup quarantines memory until retry succeeds, then reclaims exactly once PASS");
}
