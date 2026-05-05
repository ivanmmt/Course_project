/* =============================================================================
 * kernel/kernel.c  —  Steps 1-7: Memory + FS + GUI + ATA + FAT32 + WiFi
 * ============================================================================= */
#include "kernel.h"
#include "gdt.h"
#include "idt.h"
#include "syscall/syscall.h"
#include "process/process.h"
#include "../drivers/vga/vga_text.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/pit.h"
#include "../drivers/framebuffer/framebuffer.h"
#include "../drivers/mouse/mouse.h"
#include "../drivers/ata/ata.h"
#include "../drivers/net/wifi.h"
#include "../drivers/usb/usb.h"
#include "../drivers/usb/usbmass.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../memory/heap.h"
#include "../filesystem/vfs.h"
#include "../filesystem/ramfs.h"
#include "../filesystem/elf.h"
#include "../filesystem/fat32/fat32.h"
#include "../filesystem/pe/pe.h"
#include "../gui/gui.h"
#include "../programs/shell/shell.h"
#include "../lib/string.h"

extern uint32_t kernel_end;
extern void apps_init(void);

/* ─── Panic ─── */
void kernel_panic(const char* msg) {
    __asm__ volatile ("cli");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_put_string("\n\n *** KERNEL PANIC ***\n ");
    vga_put_string(msg);
    vga_put_string("\n\n System halted.\n");
    while (1) __asm__ volatile ("hlt");
}

/* ─── Логирование ─── */
static void log_ok(const char* msg) {
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_put_string(" [OK] ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(msg); vga_put_char('\n');
}
static void log_step(int n, const char* msg) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_printf(" [%d] ", n);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(msg); vga_put_char('\n');
}
static void log_warn(const char* msg) {
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string(" [!] ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_put_string(msg); vga_put_char('\n');
}

/* ─── Framebuffer данные ─── */
static uint32_t g_fb_addr=0, g_fb_w=0, g_fb_h=0, g_fb_pitch=0;
static uint8_t  g_fb_bpp=0, g_fb_type=0;

static uint32_t parse_mbi(multiboot2_info_t* mbi) {
    uint32_t mem_upper = 0;
    uint8_t* ptr = (uint8_t*)mbi + 8;
    uint8_t* end = (uint8_t*)mbi + mbi->total_size;
    while (ptr < end) {
        multiboot2_tag_t* tag = (multiboot2_tag_t*)ptr;
        if (tag->type == 0) break;
        if (tag->type == 4) {
            multiboot2_tag_memory_t* m = (multiboot2_tag_memory_t*)tag;
            mem_upper = m->mem_upper;
            vga_printf("  RAM: ~%u MB\n", (m->mem_lower+m->mem_upper)/1024);
        }
        if (tag->type == 8) {
            multiboot2_tag_framebuffer_t* f=(multiboot2_tag_framebuffer_t*)tag;
            g_fb_addr=(uint32_t)f->framebuffer_addr;
            g_fb_w=f->framebuffer_width;   g_fb_h=f->framebuffer_height;
            g_fb_pitch=f->framebuffer_pitch; g_fb_bpp=f->framebuffer_bpp;
            g_fb_type=f->framebuffer_type;
            vga_printf("  FB: %ux%u @%ubpp addr=0x%x\n",
                g_fb_w,g_fb_h,g_fb_bpp,g_fb_addr);
        }
        ptr += ALIGN_UP(tag->size, 8);
    }
    return mem_upper;
}

static bool fb_is_usable(void) {
    return g_fb_addr && g_fb_bpp >= 24 && g_fb_w >= 320 &&
           (uint32_t)g_fb_addr != 0xB8000;
}

/* ─── VFS инициализация (RamFS + FAT32 если есть диск) ─── */
static void vfs_setup(void) {
    vfs_node_t* root = ramfs_init();
    if (!root) kernel_panic("ramfs_init failed");
    vfs_init();
    vfs_mount_root(root);

    /* Структура директорий */
    vfs_node_t* etc  = ramfs_create_dir(root, "etc");
    vfs_node_t* tmp  = ramfs_create_dir(root, "tmp");
    vfs_node_t* bin  = ramfs_create_dir(root, "bin");
    vfs_node_t* mnt  = ramfs_create_dir(root, "mnt");
    (void)bin;

    static const char readme[] =
        "=== MyOS v0.1 — Steps 1-7 ===\n\n"
        "Subsystems:\n"
        "  GDT + IDT (Protected Mode)\n"
        "  PMM + VMM + Heap (Memory)\n"
        "  Framebuffer + Mouse + GUI\n"
        "  VFS + RamFS + ELF loader\n"
        "  ATA/IDE + FAT32 (Step 7)\n\n"
        "Shell commands: help, ls, cat, exec, mount, df\n";

    static const char motd[] =
        "Welcome to MyOS! Type 'help' for commands.\n";

    ramfs_create_file(root, "readme.txt",
        (const uint8_t*)readme, strlen(readme));
    ramfs_create_file(etc, "motd",
        (const uint8_t*)motd, strlen(motd));
    ramfs_create_file(tmp, "scratch.txt",
        (const uint8_t*)"", 0);

    vga_printf("  RamFS: / created (%u dirs)\n", root->child_count);

    /* Пробуем монтировать FAT32 с первого ATA диска */
    ata_drive_t* disk = ata_get_drive(0);
    if (disk && disk->info.present) {
        vga_put_string("  Trying FAT32 mount on ATA0...\n");
        vfs_node_t* fat_root = fat32_mount(disk, 0);
        if (fat_root) {
            /* Монтируем в /mnt/disk0 */
            fat_root->parent = mnt;
            strncpy(fat_root->name, "disk0", VFS_NAME_MAX-1);
            if (mnt->child_count < 64)
                mnt->children[mnt->child_count++] = fat_root;
            log_ok("FAT32 mounted at /mnt/disk0");
        } else {
            log_warn("FAT32 not found on ATA0 (run 'mkdisk' to create)");
        }
    }
}

/* ─── Splash ─── */
static void draw_splash(void) {
    fb_clear(0x0A1628u);
    int32_t cx=(int32_t)fb_width()/2, cy=(int32_t)fb_height()/2;
    fb_fill_round_rect(cx-70,cy-90,140,140,20,0x1E5A8Au);
    fb_draw_string_scaled(cx-62,cy-52,"My",FB_WHITE,3);
    fb_draw_string_scaled(cx-26,cy- 8,"OS",0x4FC3F7u,3);
    fb_draw_string(cx-108,cy+64,
        "Educational x86 OS  —  Steps 1-7",0xCCDDEEu,0,true);
    fb_fill_rect(cx-110,cy+112,220,10,0x1A3A5Au);
    fb_draw_rect(cx-110,cy+112,220,10,0x2A5A7Au);
    for(int i=0;i<=220;i++){
        fb_fill_rect(cx-110,cy+113,i,8,0x4FC3F7u);
        if(i%10==0) fb_swap();
        uint32_t t=pit_get_ticks(); while(pit_get_ticks()-t<1);
    }
    fb_swap();
}

/* ─── kernel_main ─── */
void kernel_main(uint32_t magic, multiboot2_info_t* mbi) {
    vga_init(); vga_clear();
    if (magic != 0x36d76289) kernel_panic("Not Multiboot2!");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_put_string(" MyOS v0.1  |  Steps 1-7: ATA + FAT32\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    log_step(1,"GDT...");       gdt_init();      log_ok("GDT");
    log_step(2,"IDT+PIC...");   idt_init();      log_ok("IDT");
    log_step(3,"PIT 100Hz..."); pit_init(100);   log_ok("PIT");
    log_step(4,"Keyboard...");  keyboard_init(); log_ok("Keyboard");

    log_step(5,"Multiboot2...");
    uint32_t mem_upper = parse_mbi(mbi);
    if (!mem_upper) mem_upper = 128*1024;

    log_step(6,"PMM...");
    pmm_init(mem_upper,(uint32_t)&kernel_end);
    pmm_dump_stats(); log_ok("PMM");

    log_step(7,"VMM+Paging..."); vmm_init(); log_ok("Paging ON");
    log_step(8,"Heap...");       heap_init(); log_ok("Heap");

    __asm__ volatile ("sti");
    log_ok("Interrupts ON");

    /* ─── ATA диск ─── */
    log_step(9,"ATA/IDE disk scan...");
    ata_init();
    if (ata_get_count() > 0) {
        ata_dump_info();
        log_ok("ATA initialized");
    } else {
        log_warn("No ATA drives found (add -hda disk.img to QEMU)");
    }

    /* ─── VFS + RamFS + FAT32 ─── */
    log_step(10,"VFS + RamFS + FAT32...");
    vfs_setup();
    log_ok("VFS ready");

    /* ─── Framebuffer ─── */
    log_step(11,"Framebuffer...");
    bool fb_ok = false;
    if (fb_is_usable()) {
        fb_ok = fb_init(g_fb_addr,g_fb_w,g_fb_h,g_fb_pitch,g_fb_bpp);
        if (fb_ok) log_ok("Framebuffer");
        else log_warn("FB init failed");
    } else {
        if ((uint32_t)g_fb_addr == 0xB8000)
            log_warn("VGA text only — run: make run-fb");
        else
            log_warn("No pixel framebuffer");
    }

    log_step(12,"Mouse...");
    mouse_init(fb_ok?(int32_t)fb_width():800,
               fb_ok?(int32_t)fb_height():600);
    log_ok("Mouse");

    log_step(13,"Syscall INT 0x80...");
    syscall_init();
    log_ok("Syscalls ready (INT 0x80)");

    log_step(14,"Process manager...");
    process_init();
    log_ok("Process manager ready");

    log_step(15,"Network (PCI + e1000)...");
    {
        extern void pci_init(void);
        extern bool e1000_init(void);
        extern void net_init(void);
        pci_init();
        if (e1000_init()) {
            net_init();
            log_ok("Network ready");
        } else {
            log_warn("No NIC (add: -nic user,model=e1000)");
        }
    }

    log_step(16,"GUI...");
    gui_init(); log_ok("GUI ready");

    log_step(17,"WiFi...");
    if (wifi_init()) {
        log_ok("WiFi ready");
    } else {
        log_warn("WiFi not available");
    }

    log_step(18,"USB...");
    if (usb_init()) {
        usb_mass_storage_t* ms = usb_find_mass_storage();
        if (ms && usbmass_init(ms)) {
            /* Mount USB drive at /mnt/usb0 */
            extern bool fat32_mount_usb(const char* mount_point, usb_mass_storage_t* ms);
            if (fat32_mount_usb("/mnt/usb0", ms)) {
                log_ok("USB mass storage mounted at /mnt/usb0");
            } else {
                log_warn("USB mass storage mount failed");
            }
        } else {
            log_warn("No USB mass storage found");
        }
    } else {
        log_warn("USB initialization failed");
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_put_string("\n *** Steps 1-10 complete! ***\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* MOTD */
    int32_t motd_fd = vfs_open("/etc/motd", VFS_O_READ);
    if (motd_fd >= 0) {
        uint8_t buf[128]; int32_t n;
        while ((n=vfs_read(motd_fd,buf,sizeof(buf)-1))>0) {
            buf[n]='\0'; vga_put_string((char*)buf);
        }
        vfs_close(motd_fd);
    }

    if (fb_ok) {
        pit_sleep(300);
        draw_splash();
        apps_init();
        gui_run();
    }

    shell_run();
}
