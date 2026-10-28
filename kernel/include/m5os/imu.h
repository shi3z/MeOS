#pragma once
#include <m5os/types.h>

/* Tab5 IMU (BMI270 on the system I2C bus): acceleration only, used to wake the suspended system when the device is moved. */
bool imu_init(void);                          /* loads the chip's configuration (about a second); false if it does not answer */
bool imu_ready(void);
bool imu_read_accel(int16_t xyz[3]);          /* raw counts; +-8 g range, 4096 counts per g */
