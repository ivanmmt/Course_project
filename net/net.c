/* =============================================================================
 * net/net.c
 * Network Stack Initialization & Main Loop
 * ============================================================================= */

#include "net.h"
#include "../drivers/net/e1000.h"
#include "../drivers/timer/pit.h"
#include "../drivers/vga/vga_text.h"

static bool net_stack_initialized = false;

/* ─────────────────────────────────────────────────────────────────────────── */
void net_init(void) {
    if (net_stack_initialized)
        return;
    
    vga_printf("[NET] Initializing network stack...\n");
    
    /* Initialize each layer in order (bottom-up) */
    net_core_init();    /* Ethernet layer */
    net_arp_init();     /* ARP resolution */
    net_ip_init();      /* IPv4 routing */
    
    net_stack_initialized = true;
    vga_printf("[NET] Stack initialized, ready=%d\n", net_stack_initialized);
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_poll(void) {
    if (!net_stack_initialized)
        return;
    
    net_core_poll();
}

/* ─────────────────────────────────────────────────────────────────────────── */
bool net_ready(void) {
    return net_stack_initialized && e1000_present();
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Wrapper for backward compatibility */
bool net_ping(ip4_addr_t ip, uint32_t timeout_ms) {
    if (!net_stack_initialized)
        return false;
    return net_icmp_ping(ip, timeout_ms);
}
