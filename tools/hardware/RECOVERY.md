# I2C startup recovery

Bus initialization releases SDA/SCL and, if SDA is low, emits nine SCL pulses
and a STOP. The existing bus mutex covers the complete sequence; startup before
the scheduler still works. Normal transaction timing is unchanged.

Tab5 retries each expander reset up to three times, separated by a 10ms wait
and bus initialization. Persistent NACK still aborts board initialization.
This does not recover an electrically stuck line or guarantee a responding
slave. Existing expander configuration writes are unchanged.

Run `sh tools/hardware/host_test.sh` for production-HAL GPIO/locking regressions
on all three boards and production Tab5 startup retry/failure regressions.
Repeated hardware reset and power-cycle testing remains required.
