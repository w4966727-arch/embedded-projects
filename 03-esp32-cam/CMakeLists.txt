# ESP32-CAM 工程 (ESP32 芯片)
# 使用 tools/esp-idf 里的 ESP-IDF v5.5
# 注意：CAM 芯片是 ESP32(经典款)，构建时用 idf.py set-target esp32

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_cam)
