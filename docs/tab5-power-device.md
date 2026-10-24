# Tab5 battery / USB switching qualification (2026-10-24)

The existing application was tested without flashing firmware, sending an explicit
reset, changing charger/INA226 configuration, or formatting the card. Serial
access used POSIX open/termios without DTR/RTS control-line operations. Firmware
source expected from the preceding verified installation is `d94c7d8`.

## USB disconnect and reconnect: passed for observed checks

The USB serial port disappeared for 63.007 seconds and reappeared. Uptime before
and after increased from 290.525 to 397.235 seconds. The bracketing host interval
was 111.680 seconds, including command batches; the post-reconnect uptime exceeds
that whole interval, so a restart during it was not detected. Command-batch host
timestamps do not support millisecond-accurate timing comparisons.

The persistent SD root remained active, on the same 30,867,456-sector card, with
the same epoch/bank/committed log length across the switch. All five existing
qualification files matched their baseline contents and hashes exactly after
reconnection. This includes the 2,058-byte interrupted-save recovery file.

The read-only power sample initially reported 7,930 mV (coarse voltage estimate
78%) and near-zero current. After reconnection it reported 7,910 mV (77%) and
-500 uA, also classified near zero/indeterminate. Charge permission was disabled
in both samples. This setting is not proof of actual charging or its absence.
No current/voltage sample was available during USB disconnection. Neither INA226
nor the raw CHG_STAT input independently identifies USB, external DC or battery
as the supply. Physical display observations are awaiting user confirmation.

## Normal power-off: prepared, awaiting operation

BASIC successfully committed `/home/pwr-20261024.bas` (16 bytes), containing
`10 PRINT 246810` followed by LF. Readback matched exactly and the SD root remained
active. The user has been asked to remove USB, use the device's normal power-off
operation, wait roughly ten seconds and start/reconnect it again. That operation
and the resulting persistence check are not yet qualified.

Actual interrupted-save power removal, low-battery shutdown, charging behavior
and a measured full loss of battery/SD power are not qualified by this test.

The sanitized receipt is `tools/power/device-results-20261024.json`. Raw serial
logs are retained privately under `/tmp/meos-power-device/`.
