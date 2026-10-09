# ESP32-S3 工程 (hello)
# 使用 tools/esp-idf 里的 ESP-IDF v5.5

cmake_minimum_required(VERSION 3.16)

# 把仓库根目录下的 protocol/ 注册为一个额外组件（三端共用的协议代码）
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_hello)
