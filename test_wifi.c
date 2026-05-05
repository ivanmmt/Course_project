#include "drivers/net/wifi.h"
#include "drivers/vga/vga_text.h"

int main() {
    vga_printf("=== WiFi Test ===\n");
    
    /* Инициализируем WiFi */
    if (!wifi_init()) {
        vga_printf("WiFi init failed\n");
        return 1;
    }
    
    vga_printf("WiFi initialized. Status: %d\n", wifi_get_status());
    
    /* Сканируем */
    vga_printf("Starting scan...\n");
    if (!wifi_scan()) {
        vga_printf("WiFi scan failed\n");
        return 1;
    }
    
    vga_printf("Scan complete.\n");
    
    /* Получаем сети */
    wifi_network_t nets[16];
    int count = 16;
    wifi_get_networks(nets, &count);
    
    vga_printf("Found %d networks:\n", count);
    for (int i = 0; i < count; i++) {
        vga_printf("  [%d] %s (security:%d, rssi:%d)\n",
                   i, nets[i].ssid, nets[i].security, nets[i].rssi);
    }
    
    return 0;
}
