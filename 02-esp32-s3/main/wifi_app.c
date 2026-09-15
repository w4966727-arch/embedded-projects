#include "wifi_app.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define WIFI_MAX_RETRY     5        //重连上限，超过置fail位

static const char *TAG = "wifi_app";

static EventGroupHandle_t s_wifi_events = NULL;
static esp_netif_t *s_sta_netif = NULL;
static int s_retry = 0;
static bool s_connected = false;

//事件处理
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if(base == WIFI_EVENT)
    {
        switch(id)
        {
            //开始配置STA模式
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA启动,开始连接...");
                esp_wifi_connect();     //连接配置好的AP
                break;
            //WIFI连接成功
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "已关联AP,等待分配IP...");
                break;
            //WIFI未连接
            case WIFI_EVENT_STA_DISCONNECTED:
            {
                //data里带着断开原因码
                wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
                s_connected = false;
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
                ESP_LOGW(TAG, "断开连接,reason=%d,重试=%d", e->reason, s_retry);

                if(s_retry < WIFI_MAX_RETRY)
                {
                    s_retry++;
                    esp_wifi_connect();
                }
                else
                {
                    ESP_LOGW(TAG, "重试超限,放弃连接");
                    //广播WIFI连接失败
                    xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
                }
                break;
            }
                
            default:
                break;
        }
    }
    //获得IP后
    else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "拿到IP:" IPSTR "网络就绪", IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        //广播WIFI连接成功
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}
//静态IP配置
static void wifi_set_static_ip(void)
{
    #if CONFIG_WIFI_STATIC_IP
    esp_netif_ip_info_t ip = {0};

    //停掉DHCP客户端，防止静态IP被覆盖
    esp_netif_dhcpc_stop(s_sta_netif);

    //静态IP地址
    esp_netif_str_to_ip4(CONFIG_WIFI_IP_ADDR, &ip.ip);
    //子网掩码
    esp_netif_str_to_ip4(CONFIG_WIFI_NETMASK, &ip.netmask);
    //网关
    esp_netif_str_to_ip4(CONFIG_WIFI_GW, &ip.gw);

    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &ip));

    //静态IP下DNS无法自动下发,进行手动配置
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;

    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(CONFIG_WIFI_DNS_MAIN);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns));

    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(CONFIG_WIFI_DNS_BACKUP);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns));

    ESP_LOGI(TAG, "DNS: %s / %s", CONFIG_WIFI_DNS_MAIN, CONFIG_WIFI_DNS_BACKUP);

    ESP_LOGI(TAG, "静态IP:%s / %s / gw %s", CONFIG_WIFI_IP_ADDR, CONFIG_WIFI_NETMASK, CONFIG_WIFI_GW);

    #else
    ESP_LOGI(TAG, "使用DHCP自动获取");

    #endif
}
//初始化
esp_err_t wifi_app_init(void)
{
    //去flash里申请一块非易失性键值分区(nvs)
    esp_err_t ret = nvs_flash_init();
    //nvs分区页满 或 nvs库驱动版本过低无法处理该nvs硬件版本
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        //擦除整个nvs分区并尝试重新申请一个nvs页
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    //如果是别的无法重新擦拭的错误直接停止
    ESP_ERROR_CHECK(ret);

    //创建wifi的事件广播
    s_wifi_events = xEventGroupCreate();

    /*  
        初始化esp-netif组件本身   初始化lwip(创建lwIP内核任务,准备好内存池,TCP/UDP控制块等)
        注册事件处理框架(IP_EVENT_STA_GOT_IP等)     建立与事件循环的关联
    */
    ESP_ERROR_CHECK(esp_netif_init());

    //创建事件循环，提供事件循环的位置
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /*  分配esp_netif_t结构体(这个对象是后续所有状态的载体)
        创建底层lwIP netif(在lwIP协议栈里注册一个网络接口,lwIP靠它来收发IP包)
        绑定WIFI驱动,在后续esp_wifi_start()后,WIFI驱动收到的数据帧会交给lwIP netif,
        --反过来lwIP将要发的包交给WIFI驱动
        创建默认配置(接口名,接口描述,默认MAC地址(从WIFI驱动获取),默认MTU(最大传输单元),默认标志)
        注册IP事件,在接口上注册IP_EVENT_STA_GOT_IP
        默认启动DHCP客户端
        返回句柄
    */
    s_sta_netif = esp_netif_create_default_wifi_sta();

    //WIFI驱动所需要的资源列表
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    //WIFI驱动创建好并根据所需资源列表分配资源
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    //在事件循环上订阅所有WIFI_EVENT类型的事件，并将里面所有的id事件都交给写好的wifi事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    wifi_event_handler, NULL, NULL));
    //IP处理也交给该回调函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    wifi_event_handler, NULL, NULL));

    //连接AP的配置
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,      CONFIG_WIFI_SSID,       sizeof(wcfg.sta.ssid) - 1);
    //留出的最后一位用于c语言字符串中最后一位写0表示结束
    wcfg.sta.ssid[sizeof(wcfg.sta.ssid) - 1] = 0;
    strncpy((char *)wcfg.sta.password,  CONFIG_WIFI_PASSWORD,   sizeof(wcfg.sta.password) - 1);
    wcfg.sta.password[sizeof(wcfg.sta.password) - 1] = 0;
    
    //连接的认证模式门槛为WPA2
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    //将本设备作为客户端去连接外部AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    //把设置好的sta配置交给WIFI驱动保存，给后续连接时使用
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));

    //设置静态IP
    wifi_set_static_ip();

    //正式启动wifi连接
    ESP_ERROR_CHECK(esp_wifi_start());

    //关闭省电模式，将延迟减小
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "初始化完成,目标SSID:%s", CONFIG_WIFI_SSID);
    return ESP_OK;
}

//初始化后等待WiFi是否连接
bool wifi_app_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,                      //相关事件
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, //连接了或断开了
        pdFALSE,                            //不清0
        pdFALSE,                            //2者满足一个即可
        pdMS_TO_TICKS(timeout_ms)           //最大超时时间
    );
    //取出bits中的WIFI_CONNECTED_BIT位并判断是否为1
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
//随时查询WiFi是否连接函数
bool wifi_app_is_connected(void)
{
    return s_connected;
}

/*
    取当前连接ap的信号强度
    > -60：优秀
    -60 ~ -70：良好
    -70 ~ -80：可用
    < -80：偏差，考虑换位置或换天线
*/
int8_t wifi_app_get_rssi(void)
{
    wifi_ap_record_t ap;
    if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        return ap.rssi;
    }
    return 0;
}

//打印当前生效的DNS
void wifi_app_print_dns(void)
{
    esp_netif_dns_info_t d = {0};

    if(esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
    {
        ESP_LOGI(TAG, "主DNS:"IPSTR, IP2STR(&d.ip.u_addr.ip4));
    }
    if(esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &d) == ESP_OK)
    {
        ESP_LOGI(TAG, "备DNS:"IPSTR, IP2STR(&d.ip.u_addr.ip4));
    }
}