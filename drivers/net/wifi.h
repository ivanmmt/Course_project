/* =============================================================================
 * drivers/net/wifi.h
 * WiFi 802.11 Driver Interface
 *
 * Поддерживает:
 *   - WiFi scanning (ESSID discovery)
 *   - WPA2-PSK подключение
 *   - 2.4GHz и 5GHz bands
 *   - Auto-reconnection
 * ============================================================================= */

#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_SSID_MAX       32
#define WIFI_PASS_MAX       64
#define WIFI_MAC_LEN        6
#define WIFI_MAX_NETWORKS   16

/* WiFi безопасность */
typedef enum {
    WIFI_SECURITY_OPEN = 0,
    WIFI_SECURITY_WEP = 1,
    WIFI_SECURITY_WPA = 2,
    WIFI_SECURITY_WPA2 = 3,
} wifi_security_t;

/* Сканированная сеть */
typedef struct {
    char         ssid[WIFI_SSID_MAX];
    uint8_t      bssid[WIFI_MAC_LEN];
    int8_t       rssi;              /* сила сигнала (дБм) */
    uint32_t     frequency;         /* частота в МГц */
    wifi_security_t security;
    bool         valid;
} wifi_network_t;

/* Статус подключения */
typedef enum {
    WIFI_DISCONNECTED = 0,
    WIFI_SCANNING = 1,
    WIFI_CONNECTING = 2,
    WIFI_CONNECTED = 3,
    WIFI_FAILED = 4,
} wifi_status_t;

/* WiFi конфигурация подключения */
typedef struct {
    char         ssid[WIFI_SSID_MAX];
    char         password[WIFI_PASS_MAX];
    wifi_security_t security;
    bool         auto_connect;      /* переподключаться автоматически */
} wifi_config_t;

/* WiFi девайс */
typedef struct {
    bool         present;           /* девайс найден */
    wifi_status_t status;
    wifi_config_t current_config;
    
    /* Список отсканированных сетей */
    wifi_network_t networks[WIFI_MAX_NETWORKS];
    int            network_count;
} wifi_dev_t;

/* ─── API ─── */

/* Инициализировать WiFi (поиск девайса через PCI) */
bool wifi_init(void);

/* Проверить наличие WiFi адаптера */
bool wifi_present(void);

/* Получить статус подключения */
wifi_status_t wifi_get_status(void);

/* Сканировать доступные сети */
bool wifi_scan(void);

/* Получить список отсканированных сетей */
void wifi_get_networks(wifi_network_t* out_nets, int* count);

/* Подключиться к сети */
bool wifi_connect(const char* ssid, const char* password, wifi_security_t security);

/* Отключиться от текущей сети */
bool wifi_disconnect(void);

/* Получить IP адрес (через DHCP) */
uint32_t wifi_get_ip(void);

/* Вывести статус */
void wifi_dump_status(void);

#endif /* WIFI_H */
