# 注意：main 是 ESP-IDF 的特例组件 —— 不声明 REQUIRES/PRIV_REQUIRES 时，
# 它【自动依赖构建中的所有组件】（包括 EXTRA_COMPONENT_DIRS 注册的 protocol）。
# 一旦显式声明依赖，这个自动行为会被取消，反而导致 driver/uart.h、
# mqtt_client.h、esp_wifi.h 等全部找不到。所以这里保持不声明。
idf_component_register(SRCS "app_main.c" "uart_link.c" "wifi_app.c" "ntp_sync.c" "mqtt_app.c"
                       INCLUDE_DIRS ".")
