# ESP-IDF 组件描述（仅 ESP32 侧使用）
# PC 侧测试读 tests/CMakeLists.txt，STM32 侧读 01-test/Makefile，都不读这个文件。
idf_component_register(SRCS "frame_protocol.c"
                       INCLUDE_DIRS ".")