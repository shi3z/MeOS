#include <m5os/device.h>
#include <m5os/board_config.h>

void uart_dev_register(void);
void display_dev_register(void);
#if M5OS_HAS_TOUCH
void buttons_dev_register(void);
#endif

/* Registration order is also the order they appear in device_list(). */
void devices_init(void)
{
    uart_dev_register();
    display_dev_register();
#if M5OS_HAS_TOUCH
    buttons_dev_register();      /* starts the touch polling task: needs sched_init() first */
#endif
    /* The keyboard is not a device node: keyboard_start() feeds the console input. */
}
