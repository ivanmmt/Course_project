/* =============================================================================
 * programs/shell/shell.c
 * Встроенная командная оболочка MyOS
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

/* ─── Разбор аргументов ─── */
static int split_args(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* p = line;

    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = p;

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
    vfs_node_t* dir = vfs_resolve(path);
    if (!dir || dir->type != VFS_DIRECTORY) {
        vga_printf("  cd: not a directory: %s\n", path);
        return;
    }
    vfs_set_cwd(dir);
}

static void cmd_pwd(void) {
    vfs_node_t* cwd = vfs_get_cwd();
    if (!cwd) { vga_put_string("  /\n"); return; }

    char path[VFS_PATH_MAX] = {0};
    char tmp[VFS_PATH_MAX];

    vfs_node_t* n = cwd;
    while (n && n->parent) {
        strncpy(tmp, "/", sizeof(tmp)-1);
        strcat(tmp, n->name);
        strcat(tmp, path);
        strncpy(path, tmp, sizeof(path)-1);
        n = n->parent;
    }
    if (path[0] == '\0') strcpy(path, "/");
    vga_printf("  %s\n", path);
}

static void cmd_exec(int argc, char** argv) {
    if (argc < 2) {
        vga_put_string("  Usage: exec <file.elf|file.exe> [args...]\n");
        return;
    }

    const char* path = argv[1];
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        vga_printf("  exec: not found: %s\n", path);
        return;
    }

    uint8_t magic[4] = {0};
    vfs_read(fd, magic, 4);
    vfs_close(fd);

    if (magic[0]==0x7F && magic[1]=='E' && magic[2]=='L' && magic[3]=='F') {
        vga_printf("  [ELF] Launching: %s\n", path);
        int ret = process_exec(path, argc-1, argv+1);
        vga_printf("  [ELF] Exit code: %d\n", ret);
    }
    else if (magic[0]=='M' && magic[1]=='Z') {
        vga_printf("  [PE] Launching: %s\n", path);
        pe_info_t pi = pe_load_file(path);
        if (!pi.valid) {
            vga_put_string("  [PE] Load failed\n");
            return;
        }
        int ret = pe_exec(&pi, argc-1, argv+1);
        vga_printf("  [PE] Exit code: %d\n", ret);
    }
    else {
        vga_printf("  exec: unknown format (magic: %02x%02x%02x%02x)\n", 
                   magic[0], magic[1], magic[2], magic[3]);
    }
}

static void cmd_elfinfo(int argc, char** argv) {
    if (argc < 2) { vga_put_string("  Usage: elfinfo <file>\n"); return; }

    vfs_stat_t st;
    vfs_stat(argv[1], &st);
    if (st.size == 0) {
        vga_printf("  elfinfo: not found: %s\n", argv[1]);
        return;
    }

    uint8_t* buf = kmalloc(st.size);
    if (!buf) return;

    int32_t fd = vfs_open(argv[1], VFS_O_READ);
    if (fd >= 0) {
        vfs_read(fd, buf, st.size);
        vfs_close(fd);
        elf_dump_info(buf, st.size);
    }
    kfree(buf);
}

static void cmd_tree(const char* path, int depth) {
    vfs_node_t* dir = vfs_resolve(path);
    if (dir) ramfs_dump(dir, depth);
}

static void cmd_help(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_put_string("\n  MyOS Shell v0.3 — Commands:\n\n");

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Filesystem:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    ls [path]     list files\n"
        "    cat <file>    show file\n"
        "    echo <text>   print text\n"
        "    write <f> <t> write to file\n"
        "    mkdir <dir>   create dir\n"
        "    rm <file>     delete file\n"
        "    cd <path>     change dir\n"
        "    pwd           show current path\n"
        "    tree [path]   show tree\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  Programs:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    exec <file>     run program\n"
        "    elfinfo <file>  show ELF info\n"
        "    fm              file manager\n"
        "    browser [url]   open browser\n"
        "    word [file]     word processor\n"
        "    code [file]     code editor\n"
        "    video [file]    video player\n"
        "    audio [file]    audio player\n"
    );

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_put_string("  System:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string(
        "    ps            processes\n"
        "    mem / df      memory info\n"
        "    uptime        system uptime\n"
        "    clear         clear screen\n"
        "    help          this help\n"
    );
    vga_put_char('\n');
}

/* =============================================================================
 * shell_run — главный цикл
 * ============================================================================= */
void shell_run(void) {
    char line[SHELL_LINE_MAX];
    char* argv[SHELL_MAX_ARGS];
    int pos;

    vga_clear();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_put_string(
        "  ╔══════════════════════════════════════╗\n"
        "  ║          MyOS Shell v0.3             ║\n"
        "  ╚══════════════════════════════════════╝\n\n"
    );
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_put_string("  Type 'help' for list of commands.\n\n");

    while (1) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_put_string("myos");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_put_string(":/# ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        pos = 0;
        memset(line, 0, sizeof(line));

        while (1) {
            static uint32_t last_poll = 0;
            if (pit_get_ticks() - last_poll >= 8) {
                net_poll();
                last_poll = pit_get_ticks();
            }

            char c = keyboard_getchar();

            if (c == '\0') {
                asm volatile("hlt");
                continue;
            }

            if (c == '\n' || c == '\r') {
                vga_put_char('\n');
                break;
            }

            if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    line[pos] = '\0';
                    vga_put_char('\b');
                    vga_put_char(' ');
                    vga_put_char('\b');
                }
            }
            else if (pos < SHELL_LINE_MAX - 1 && c >= ' ') {
                line[pos++] = c;
                vga_put_char(c);
            }
        }

        if (pos == 0) continue;

        int argc = split_args(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

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
        else if (strcmp(argv[0], "clear")   == 0) vga_clear();
        else if (strcmp(argv[0], "mem") == 0 || strcmp(argv[0], "df") == 0) {
            pmm_dump_stats();
            heap_dump_stats();
        }
        else if (strcmp(argv[0], "uptime")  == 0) {
            uint32_t t = pit_get_ticks();
            vga_printf("  Uptime: %u.%02u sec\n", t/100, t%100);
        }
        else if (strcmp(argv[0], "ps")      == 0) process_list();

        else if (strcmp(argv[0], "fm") == 0 || strcmp(argv[0], "explorer") == 0) {
            extern void filemanager_run(const char* p);
            filemanager_run(argc > 1 ? argv[1] : "/");
        }
        else if (strcmp(argv[0], "browser") == 0) {
            extern gui_window_t* browser_open(const char* url);
            browser_open(argc > 1 ? argv[1] : "http://example.com");
        }
        else if (strcmp(argv[0], "word") == 0) {
            extern void wordproc_run(const char* f);
            wordproc_run(argc > 1 ? argv[1] : "/tmp/doc.txt");
        }
       /* else if (strcmp(argv[0], "video") == 0) {
            extern void mediaplayer_video_run(const char* f);
            mediaplayer_video_run(argc > 1 ? argv[1] : "/tmp/image.bmp");
        }
        else if (strcmp(argv[0], "audio") == 0) {
            extern void mediaplayer_audio_run(const char* f);
            mediaplayer_audio_run(argc > 1 ? argv[1] : "/tmp/audio.wav");
        }*/
        else if (strcmp(argv[0], "code") == 0) {
            extern void codeeditor_run(const char* f);
            codeeditor_run(argc > 1 ? argv[1] : "/tmp/file.c");
        }
        else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_printf("  Unknown command: %s\n", argv[0]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
    }
}