#include <assert.h>
#include <stdio.h>
#include <m5os/peripheral.h>
#include <m5os/arch.h>
static unsigned depth;
uint32_t arch_irq_disable(void) { return depth++; }
void arch_irq_restore(uint32_t old) { assert(depth==old+1); depth=old; }
int main(void) {
    static char output, input, other;
    const peripheral_t tx[]={PERIPHERAL_I2S0,PERIPHERAL_AHB_DMA_TX2};
    const peripheral_t rx[]={PERIPHERAL_AHB_DMA_RX2,PERIPHERAL_I2S0};
    const peripheral_t invalid[]={PERIPHERAL_AHB_DMA_RX2,PERIPHERAL_COUNT};
    const peripheral_t duplicate[]={PERIPHERAL_I2S0,PERIPHERAL_I2S0};
    const peripheral_t negative[]={ (peripheral_t)-1 };
    assert(!peripheral_claim(NULL,1,&input)); assert(!peripheral_claim(tx,0,&input));
    assert(!peripheral_claim(tx,PERIPHERAL_COUNT+1,&input));
    assert(!peripheral_claim(tx,2,NULL));
    assert(!peripheral_claim(invalid,2,&input) && !peripheral_is_owned(PERIPHERAL_AHB_DMA_RX2,&input));
    assert(!peripheral_claim(duplicate,2,&input)); assert(!peripheral_claim(negative,1,&input));
    assert(peripheral_claim(tx,2,&output)); assert(peripheral_claim(tx,2,&output));
    assert(!peripheral_claim(rx,2,&input)); /* Conflict in second ID must not reserve first. */
    assert(!peripheral_is_owned(PERIPHERAL_AHB_DMA_RX2,&input));
    assert(peripheral_claim(rx,1,&other));
    peripheral_release_owner(&input); peripheral_release_owner(NULL);
    assert(peripheral_is_owned(PERIPHERAL_I2S0,&output));
    peripheral_release_owner(&output);
    assert(!peripheral_claim(rx,2,&input)); /* Independent RX owner survives output release. */
    peripheral_release_owner(&other);
    uint32_t irq=arch_irq_disable();
    assert(peripheral_claim(rx,2,&input) && depth==1); /* Nested caller's IRQ mask survives. */
    arch_irq_restore(irq);
    assert(!peripheral_is_owned(PERIPHERAL_COUNT,&input));
    assert(!peripheral_is_owned(PERIPHERAL_I2S0,NULL));
    peripheral_release_owner(&input); peripheral_release_owner(&input);
    assert(peripheral_claim(tx,2,&output)); peripheral_release_owner(&output);
    assert(depth==0);
    puts("peripheral: atomic claims, conflict rollback, owner isolation, release/reopen and IRQ nesting PASS");
}
