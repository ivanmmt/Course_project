/* =============================================================================
 * programs/shell/shell.c
 * Встроенная командная оболочка MyOS
 *
 * Поддерживает команды:
 *   ls [path]     - список файлов
 *   cat <file>    - вывод файла
 *   echo <text>   - вывод текста
 *   write <f> <t> - запись текста в файл
 *   mkdir <path>  - создать директорию
 *   rm <file>     - удалить файл
 *   cd <path>     - сменить директорию
 *   pwd           - текущая директория
 *   exec <file>   - запустить ELF файл
 *   elfinfo <f>   - показать ELF заголовки
 *   mem / heap    - статистика памяти
 *   help          - справка
 * ============================================================================= */

#include "shell.h"
#include "../../filesystem/vfs.h"
#include "../../filesystem/elf.h"
#include "../../filesystem/ramfs.h"
#include "../../filesystem/pe/pe.h"
#include "../../drivers/vga/vga_text.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../memory/pmm.h"
#include "../../memory/heap.h"
#include "../../drivers/timer/pit.h"
#include "../../kernel/process/process.h"
#include "../../programs/browser/browser.h"
#include "../../programs/wordproc/wordproc.h"
#include "../../programs/mediaplayer/video/mediaplayer_video.h"
#include "../../programs/mediaplayer/audio/mediaplayer_audio.h"
#include "../../programs/codeeditor/codeeditor.h"
#include "../../lib/string.h"
#include "../../net/net.h"
#include "../../drivers/net/wifi.h"

#define SHELL_MAX_ARGS  16
#define SHELL_LINE_MAX  256

/* ─── Вспомогательная: разбить строку на аргументы ─── */
static int split_args(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* p  = line;

    while (*p && argc < max_args) {
        /* Пропускаем пробелы */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = p;

        /* Ищем конец слова */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ─── Команды ─── */

static void cmd_ls(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/";

    vfs_node_t* dir = vfs_resolve(path);
    if (!dir) {
        vga_printf("  ls: no such directory: %s\n", path);
        return;
    }
    if (dir->type != VFS_DIRECTORY) {
        vga_printf("  ls: not a directory: %s\n", path);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_printf("  %s:\n", path);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    for (uint32_t i = 0; i < dir->child_count; i++) {
        vfs_node_t* child = dir->children[i];
        if (child->type == VFS_DIRECTORY) {
            vga_set_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            vga_printf("    %s/\n", child->name);
        } else {
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_printf("    %-24s  %u bytes\n", child->name, child->size);
        }
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_printf("  %u item(s)\n", dir->child_count);
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) { vga_put_string("  Usage: cat <file>\n"); return; }

    int32_t fd = vfs_open(argv[1], VFS_O_READ);
    if (fd < 0) {
        vga_printf("  cat: cannot open: %s\n", argv[1]);
        return;
    }

    uint8_t buf[256];
    int32_t n;
    vga_put_char('\n');
    while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        vga_put_string((char*)buf);
    }
    vga_put_char('\n');
    vfs_close(fd);
}

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        vga_put_string(argv[i]);
        if (i < argc - 1) vga_put_char(' ');
    }
    vga_put_char('\n');
}

static void cmd_write(int argc, char** argv) {
    if (argc < 3) { vga_put_string("  Usage: write <file> <text>\n"); return; }

    int32_t fd = vfs_open(argv[1], VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) {
        vga_printf("  write: cannot open: %s\n", argv[1]);
        return;
    }

    /* Склеиваем все аргументы после имени файла */
    for (int i = 2; i < argc; i++) {
        vfs_write(fd, argv[i], strlen(argv[i]));
        if (i < argc - 1) vfs_write(fd, " ", 1);
    }
    vfs_write(fd, "\n", 1);
    vfs_close(fd);
    vga_printf("  Written to %s\n", argv[1]);
}

static void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { vga_put_string("  Usage: mkdir <dir>\n"); return; }
    if (vfs_mkdir(argv[1]) == 0)
        vga_printf("  Created: %s\n", argv[1]);
    else
        vga_printf("  mkdir: failed: %s\n", argv[1]);
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) { vga_put_string("  Usage: rm <file>\n"); return; }
    if (vfs_unlink(argv[1]) == 0)
        vga_printf("  Deleted: %s\n", argv[1]);
    else
        vga_printf("  rm: not found: %s\n", argv[1]);
}

static void cmd_cd(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/";
    vfs_node_t* dir  = vfs_resolve(path);
    if (!dir || dir->type != VFS_DIRECTORY) {
        vga_printf("  cd: not a directory: %s\n", path);
        return;
    }
    vfs_set_cwd(dir);
}

static void cmd_pwd(void) {
    /* Строим путь от cwd до корня */
    vfs_node_t* cwd = vfs_get_cwd();
    if (!cwd) { vga_put_string("  /\n"); return; }

    char path[VFS_PATH_MAX];
    char tmp[VFS_PATH_MAX];
    path[0] = '\0';

    vfs_node_t* n = cwd;
    while (n && n->parent) {
        /* Добавляем "/" + имя в начало */
        strncpy(tmp, "/", VFS_PATH_MAX - 1);
        strcat(tmp, n->name);
        strcat(tmp, path);
        strncpy(path, tmp, VFS_PATH_MAX - 1);
        n = n->parent;
    }
    if (path[0] == '\0') strncpy(path, "/", VFS_PATH_MAX - 1);
    vga_printf("  %s\n", path);
}

static void cmd_exec(int argc, char** argv) {
    if (argc < 2) {
        vga_put_string("  Usage: exec <file.elf|file.exe> [args...]\n");
        vga_put_string("  Auto-detects ELF32 and PE32 (.exe) formats\n");
        return;
    }

    const char* path = argv[1];

    /* Читаем первые 4 байта чтобы определить формат */
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) { vga_printf("  exec: not found: %s\n", path); return; }

    uint8_t magic[4] = {0};
    vfs_read(fd, magic, 4);
    vfs_close(fd);

    /* ELF: 0x7F 'E' 'L' 'F' */
    if (magic[0]==0x7F && magic[1]=='E' && magic[2]=='L' && magic[3]=='F') {
        vga_printf("  [ELF] Launching: %s\n", path);
        int ret = process_exec(path, argc-1, argv+1);
        vga_printf("  [ELF] Exit code: %d\n", ret);
        return;
    }

    /* PE/EXE: 'M' 'Z' */
    if (magic[0]=='M' && magic[1]=='Z') {
        vga_printf("  [PE] Launching: %s\n", path);
        pe_info_t pi = pe_load_file(path);
        if (!pi.valid) {
            vga_put_string("  [PE] Load failed (check format/relocs)\n");
            return;
        }
        int ret = pe_exec(&pi, argc-1, argv+1);
        vga_printf("  [PE] Exit code: %d\n", ret);
        return;
    }

    vga_printf("  exec: unknown format (magic: %02x %02x %02x %02x)\n",
               magic[0], magic[1], magic[2], magic[3]);
    vga_put_string("  Supported: ELF32 (0x7FELF) and PE32/EXE (MZ)\n");
}

static void cmd_elfinfo(int argc, char** argv) {
    if (argc < 2) { vga_put_string("  Usage: elfinfo <file>\n"); return; }

    int32_t fd = vfs_open(argv[1], VFS_O_READ);
    if (fd < 0) { vga_printf("  elfinfo: cannot open: %s\n", argv[1]); return; }

    vfs_stat_t st;
    vfs_stat(argv[1], &st);
    uint8_t* buf = (uint8_t*)kmalloc(st.size);
    if (!buf) { vfs_close(fd); return; }

    vfs_read(fd, buf, st.size);
    vfs_close(fd);
    elf_dump_info(buf, st.size);
    kfree(buf);
}

static void cmd_tree(const char* path, int depth) {
    vfs_node_t* dir = vfs_resolve(path);
    if (!dir) return;
    ramfs_dump(dir, depth);
}

static void cmd_help(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_put_string("\n  MyOS Shell v0.1  —  Commands:\n\n");

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Filesystem:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    ls [path]           list files\n"
        "    cat <file>          show file contents\n"
        "    echo <text>         print text\n"
        "    write <file> <txt>  write text to file\n"
        "    mkdir <dir>         create directory\n"
        "    rm <file>           delete file\n"
        "    cd <path>           change directory\n"
        "    pwd                 current directory\n"
        "    tree [path]         directory tree\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Programs (Step 8):\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    exec <elf> [args]   run ELF executable\n"
        "    elfinfo <file>      show ELF headers\n"
        "    ps                  list processes\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Storage (Step 7):\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    ata                 ATA disk info\n"
        "    mount               mount points\n"
        "    df                  disk/memory usage\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  System:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    mem                 memory stats\n"
        "    uptime              system uptime\n"
        "    clear               clear screen\n"
        "    layout [ua|us]      keyboard layout\n"
        "    help                this help\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Applications:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    fm                  file manager\n"
        "    browser [url]       web browser (HTTP/HTTPS)\n"
        "    word [file]         word processor\n"
        "    code [file]         code editor (C/C++/Py/JS)\n"
        "    video [file]        video/image viewer\n"
        "    audio [file]        audio player\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Network:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    netstatus           show network diagnostics\n"
        "    ping <host>         ping a host\n"
    );
    vga_put_char('\n');
}

/* =============================================================================
 * shell_run — главный цикл шелла
 * ============================================================================= */
void shell_run(void) {
    char line[SHELL_LINE_MAX];
    char* argv[SHELL_MAX_ARGS];
    int   pos;

    vga_clear();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_put_string(
        "  ╔══════════════════════════════════════╗\n"
        "  ║   MyOS Shell  v0.1  —  Steps 1-5    ║\n"
        "  ╚══════════════════════════════════════╝\n"
    );
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string("  Type 'help' for commands.\n\n");

    while (1) {
        /* Приглашение */
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_put_string("myos");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_put_string(":/# ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        /* Чтение строки */
        pos = 0;
        memset(line, 0, sizeof(line));

        while (1) {
            char c = keyboard_getchar();
            if (c == '\n' || c == '\r') {
                vga_put_char('\n');
                break;
            }
            if (c == '\b') {
                if (pos > 0) { pos--; line[pos] = '\0'; vga_put_char('\b'); }
            } else if (pos < SHELL_LINE_MAX - 1) {
                line[pos++] = c;
                vga_put_char(c);
            }
        }

        if (pos == 0) continue;

        /* Разбиваем на аргументы */
        int argc = split_args(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

        /* Диспетчер команд */
        if      (strcmp(argv[0], "help")    == 0) cmd_help();
        else if (strcmp(argv[0], "ls")      == 0) cmd_ls(argc, argv);
        else if (strcmp(argv[0], "cat")     == 0) cmd_cat(argc, argv);
        else if (strcmp(argv[0], "echo")    == 0) cmd_echo(argc, argv);
        else if (strcmp(argv[0], "write")   == 0) cmd_write(argc, argv);
        else if (strcmp(argv[0], "mkdir")   == 0) cmd_mkdir(argc, argv);
        else if (strcmp(argv[0], "rm")      == 0) cmd_rm(argc, argv);
        else if (strcmp(argv[0], "cd")      == 0) cmd_cd(argc, argv);
        else if (strcmp(argv[0], "pwd")     == 0) cmd_pwd();
        else if (strcmp(argv[0], "exec")    == 0) cmd_exec(argc, argv);
        else if (strcmp(argv[0], "elfinfo") == 0) cmd_elfinfo(argc, argv);
        else if (strcmp(argv[0], "tree")    == 0) cmd_tree(argc > 1 ? argv[1] : "/", 0);
        else if (strcmp(argv[0], "mount")   == 0) {
            /* Показываем точки монтирования */
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_put_string("  Mount points:\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_put_string("    /         ramfs    (RAM filesystem)\n");
            if (vfs_exists("/mnt/disk0"))
                vga_put_string("    /mnt/disk0  fat32    (ATA disk)\n");
        }
        else if (strcmp(argv[0], "df")      == 0) {
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_put_string("  Filesystem usage:\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            pmm_dump_stats();
            heap_dump_stats();
        }
        else if (strcmp(argv[0], "mem")     == 0) { pmm_dump_stats(); heap_dump_stats(); }
        else if (strcmp(argv[0], "uptime")  == 0) {
            uint32_t t = pit_get_ticks();
            vga_printf("  %u sec (%u ticks)\n", t/100, t);
        }
        else if (strcmp(argv[0], "ata")     == 0) {
            extern void ata_dump_info(void);
            ata_dump_info();
        }
        else if (strcmp(argv[0], "ps")      == 0) process_list();
        else if (strcmp(argv[0], "peinfo")  == 0) {
            /* Показать заголовки PE файла */
            if (argc < 2) { vga_put_string("  Usage: peinfo <file.exe>\n"); return; }
            vfs_stat_t st; vfs_stat(argv[1], &st);
            if (!st.size) { vga_printf("  peinfo: not found: %s\n",argv[1]); return; }
            uint8_t* buf = (uint8_t*)kmalloc(st.size);
            if (!buf) return;
            int32_t fd2 = vfs_open(argv[1], VFS_O_READ);
            if (fd2 >= 0) { vfs_read(fd2, buf, st.size); vfs_close(fd2);
                            pe_dump_info(buf, st.size); }
            kfree(buf);
        }
        else if (strcmp(argv[0], "fm")      == 0 ||
                 strcmp(argv[0], "explorer")== 0) {
            extern void filemanager_run(const char* path);
            const char* start = (argc > 1) ? argv[1] : "/";
            filemanager_run(start);
        }
        else if (strcmp(argv[0], "browser") == 0 ||
                 strcmp(argv[0], "http")    == 0) {
            extern gui_window_t* browser_open(const char* url);
            const char* url = (argc > 1) ? argv[1] : "http://example.com";
            browser_open(url);
        }
        else if (strcmp(argv[0], "word") == 0 ||
                 strcmp(argv[0], "wordproc") == 0) {
            extern void wordproc_run(const char* filepath);
            const char* fp = (argc > 1) ? argv[1] : "/tmp/document.txt";
            wordproc_run(fp);
        }
        else if (strcmp(argv[0], "video") == 0 ||
                 strcmp(argv[0], "mediaplayer") == 0) {
            extern void mediaplayer_video_run(const char* filepath);
            const char* fp = (argc > 1) ? argv[1] : "/tmp/image.bmp";
            mediaplayer_video_run(fp);
        }
        else if (strcmp(argv[0], "audio") == 0) {
            extern void mediaplayer_audio_run(const char* filepath);
            const char* fp = (argc > 1) ? argv[1] : "/tmp/audio.wav";
            mediaplayer_audio_run(fp);
        }
        else if (strcmp(argv[0], "code") == 0 ||
                 strcmp(argv[0], "codeedit") == 0) {
            extern void codeeditor_run(const char* filepath);
            const char* fp = (argc > 1) ? argv[1] : "/tmp/untitled.c";
            codeeditor_run(fp);
        }
        else if (strcmp(argv[0], "layout") == 0) {
            extern void keyboard_set_layout(int);
            extern int keyboard_get_layout(void);
            if (argc > 1) {
                if (strcmp(argv[1], "ua") == 0 || strcmp(argv[1], "uk") == 0)
                    keyboard_set_layout(1);
                else
                    keyboard_set_layout(0);
            }
            vga_printf("  Current layout: %s\n",
                keyboard_get_layout() == 1 ? "Ukrainian" : "US");
        }
        else if (strcmp(argv[0], "netstatus") == 0) {
            ip4_addr_t ip = net_get_ip();
            mac_addr_t mac = net_get_mac();
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_put_string("\n  === Network Status ===\n\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_printf("  Ready:    %s\n", net_ready() ? "YES" : "NO");
            vga_printf("  IP:       %d.%d.%d.%d\n",
                (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
            vga_printf("  MAC:      %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac.b[0],mac.b[1],mac.b[2],mac.b[3],mac.b[4],mac.b[5]);

            if (net_ready()) {
                vga_put_string("  Pinging gateway (10.0.2.2)... ");
                if (net_ping(IP4_GW, 3000)) {
                    vga_put_string("OK\n");
                } else {
                    vga_put_string("FAILED\n");
                }

                vga_put_string("  Resolving example.com... ");
                uint32_t dns_ip = 0;
                if (net_dns_resolve("example.com", &dns_ip, 5000)) {
                    vga_printf("OK (%d.%d.%d.%d)\n",
                        (dns_ip>>24)&0xFF,(dns_ip>>16)&0xFF,
                        (dns_ip>>8)&0xFF,dns_ip&0xFF);
                } else {
                    vga_put_string("FAILED\n");
                }
            }
            vga_put_string("\n");
        }
        else if (strcmp(argv[0], "ping")    == 0) {
            if (argc < 2) { vga_put_string("  Usage: ping <ip|host>\n"); }
            else {
                if (!net_ready()) {
                    vga_put_string("  Network not available\n");
                } else {
                    uint32_t ip = 0;
                    const char* h = argv[1];
                    /* Парсим dotted IP */
                    uint32_t a=0,b=0,c2=0,d=0;
                    bool is_ip = false;
                    const char* p=h; int dots=0;
                    while(*p){if(*p=='.')dots++;p++;}
                    if(dots==3) {
                        p=h; a=0;
                        while(*p&&*p!='.') { a=a*10+(*p++)-'0'; } if(*p)p++;
                        while(*p&&*p!='.') { b=b*10+(*p++)-'0'; } if(*p)p++;
                        while(*p&&*p!='.') { c2=c2*10+(*p++)-'0'; } if(*p)p++;
                        while(*p) { d=d*10+(*p++)-'0'; }
                        ip=(a<<24)|(b<<16)|(c2<<8)|d; is_ip=true;
                    }
                    if(!is_ip) {
                        vga_printf("  Resolving %s...\n", h);
                        if(!net_dns_resolve(h,&ip,5000)){
                            vga_printf("  DNS failed for %s\n",h);
                            goto ping_done;
                        }
                        vga_printf("  %s = %u.%u.%u.%u\n",h,
                            (ip>>24)&0xFF,(ip>>16)&0xFF,
                            (ip>>8)&0xFF,ip&0xFF);
                    }
                    vga_printf("  Pinging %u.%u.%u.%u...\n",
                        (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
                    bool ok = net_ping(ip, 3000);
                    vga_printf("  %s\n", ok ? "Reply received!" : "Timeout.");
                    ping_done:;
                }
            }
        }
        else if (strcmp(argv[0], "net")     == 0) {
            vga_printf("  Network: %s\n", net_ready()?"UP":"DOWN");
            if (net_ready()) e1000_dump_info();
            else vga_put_string("  Add -nic user,model=e1000 to QEMU\n");
        }
        else if (strcmp(argv[0], "pci")     == 0) {
            extern void pci_enumerate(void);
            pci_enumerate();
        }
        else if (strcmp(argv[0], "clear")   == 0) vga_clear();
        else if (strcmp(argv[0], "wifi")    == 0) {
            if (argc < 2) {
                vga_put_string("  WiFi Commands:\n");
                vga_put_string("    wifi scan              - scan for networks\n");
                vga_put_string("    wifi list              - list found networks\n");
                vga_put_string("    wifi status            - show connection status\n");
                vga_put_string("    wifi connect <ssid> <pw> - connect to network (or open)\n");
                vga_put_string("    wifi disconnect        - disconnect from network\n");
            }
            else if (strcmp(argv[1], "scan") == 0) {
                vga_put_string("  WiFi scanning for 3 seconds...\n");
                wifi_scan();
                vga_put_string("  Scan complete.\n");
            }
            else if (strcmp(argv[1], "list") == 0) {
                int count = 0;
                wifi_network_t networks[16];
                wifi_get_networks(networks, &count);
                if (count == 0) {
                    vga_put_string("  No networks found. Run 'wifi scan' first.\n");
                } else {
                    vga_printf("  Found %d networks:\n", count);
                    for (int i = 0; i < count; i++) {
                        const char* sec = "";
                        if (networks[i].security == WIFI_SECURITY_OPEN) sec = "OPEN";
                        else if (networks[i].security == WIFI_SECURITY_WPA2) sec = "WPA2";
                        vga_printf("    [%d] SSID: %s | RSSI: %d dBm | Security: %s | Freq: %u MHz\n",
                            i, networks[i].ssid, networks[i].rssi, sec, networks[i].frequency);
                    }
                }
            }
            else if (strcmp(argv[1], "status") == 0) {
                wifi_dump_status();
            }
            else if (strcmp(argv[1], "connect") == 0) {
                if (argc < 3) {
                    vga_put_string("  Usage: wifi connect <ssid> [password]\n");
                } else {
                    const char* ssid = argv[2];
                    const char* password = (argc > 3) ? argv[3] : "";
                    wifi_security_t security = (argc > 3) ? WIFI_SECURITY_WPA2 : WIFI_SECURITY_OPEN;
                    vga_printf("  Connecting to %s...\n", ssid);
                    if (wifi_connect(ssid, password, security)) {
                        vga_printf("  Connected! IP: 192.168.1.100\n");
                    } else {
                        vga_put_string("  Connection failed.\n");
                    }
                }
            }
            else if (strcmp(argv[1], "disconnect") == 0) {
                vga_put_string("  Disconnecting...\n");
                wifi_disconnect();
                vga_put_string("  Disconnected.\n");
            }
            else {
                vga_printf("  Unknown WiFi command: '%s'\n", argv[1]);
            }
        }
        else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_printf("  Unknown command: '%s'  (try 'help')\n", argv[0]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
    }
}
