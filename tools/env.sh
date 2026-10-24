# source this file:  . tools/env.sh        (uses M5OS_BOARD to pick the ESP-IDF; default cores3)
#   cores3 / cardputer (ESP32-S3, Xtensa)  -> ESP-IDF v4.4.3
#   tab5               (ESP32-P4, RISC-V)  -> ESP-IDF v5.4.4   (v4.4.3 knows nothing about the P4)
if [ "$M5OS_BOARD" = "tab5" ]; then
    if [ -n "${M5OS_IDF_P4:-}" ]; then
        export IDF_PATH=$M5OS_IDF_P4
    elif [ -d "$HOME/esp32/esp-idf-v5.4.4" ]; then
        export IDF_PATH=$HOME/esp32/esp-idf-v5.4.4
    else
        export IDF_PATH=${M5OS_IDF_P4:-${IDF_PATH:-$HOME/esp/esp-idf}}
    fi
    unset IDF_PYTHON_ENV_PATH
else
    export IDF_PATH=${M5OS_IDF_S3:-$HOME/esp32/esp-idf}
    export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf4.4_py3.9_env
    export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH
fi
. $IDF_PATH/export.sh >/dev/null 2>&1
