#include <m5os/display.h>
#include <m5os/arch.h>

static const void *s_owner;
bool display_try_claim(const void *owner)
{
    if (!owner) return false;
    uint32_t irq = arch_irq_disable();
    bool ok = !s_owner || s_owner == owner;
    if (ok) s_owner = owner;
    arch_irq_restore(irq);
    return ok;
}
void display_release_claim(const void *owner)
{
    uint32_t irq = arch_irq_disable();
    if (s_owner == owner) s_owner = NULL;
    arch_irq_restore(irq);
}
