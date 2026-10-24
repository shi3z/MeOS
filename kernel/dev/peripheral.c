#include <m5os/peripheral.h>
#include <m5os/arch.h>
static const void *s_owners[PERIPHERAL_COUNT];

bool peripheral_claim(const peripheral_t *ids, size_t count, const void *owner)
{
    if (!owner || !ids || !count || count > PERIPHERAL_COUNT) return false;
    for (size_t i = 0; i < count; i++) {
        if ((unsigned)ids[i] >= PERIPHERAL_COUNT) return false;
        for (size_t j = 0; j < i; j++) if (ids[j] == ids[i]) return false;
    }
    uint32_t irq = arch_irq_disable();
    for (size_t i = 0; i < count; i++) {
        if (s_owners[ids[i]] && s_owners[ids[i]] != owner) {
            arch_irq_restore(irq); return false;
        }
    }
    for (size_t i = 0; i < count; i++) s_owners[ids[i]] = owner;
    arch_irq_restore(irq); return true;
}

void peripheral_release_owner(const void *owner)
{
    if (!owner) return;
    uint32_t irq = arch_irq_disable();
    for (unsigned i = 0; i < PERIPHERAL_COUNT; i++)
        if (s_owners[i] == owner) s_owners[i] = NULL;
    arch_irq_restore(irq);
}

bool peripheral_is_owned(peripheral_t id, const void *owner)
{
    if (!owner || (unsigned)id >= PERIPHERAL_COUNT) return false;
    uint32_t irq = arch_irq_disable();
    bool owned = s_owners[id] == owner;
    arch_irq_restore(irq); return owned;
}
