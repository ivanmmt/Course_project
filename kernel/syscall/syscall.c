/* =============================================================================
 * kernel/syscall/syscall.c
 * Реализация системных вызовов
 * ============================================================================= */

#include "syscall.h"
#include "../../kernel/idt.h"
#include "../../kernel/kernel.h"
#include "../../drivers/vga/vga_text.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/timer/pit.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../gui/gui.h"
#include "../../filesystem/vfs.h"
#include "../../memory/heap.h"
#include "../../memory/pmm.h"
#include "../../lib/string.h"

/* Текущий PID (упрощённо — один процесс) */
static uint32_t current_pid = 1;

/* Флаг — программа запросила выход */
static volatile bool process_exit_requested = false;
static volatile int  process_exit_code      = 0;

bool syscall_exit_requested(void) { return process_exit_requested; }
int  syscall_exit_code(void)      { return process_exit_code; }
void syscall_clear_exit(void)     { process_exit_requested = false; }

/* =============================================================================
 * syscall_handler_c — главный диспетчер системных вызовов
 * Вызывается из syscall_entry.asm с аргументами из регистров
 * ============================================================================= */
void syscall_handler_c(uint32_t num,
                        uint32_t arg1, uint32_t arg2, uint32_t arg3,
                        uint32_t* ret) {
    int32_t result = 0;

    switch (num) {

    /* ── SYS_EXIT: завершить процесс ── */
    case SYS_EXIT:
        process_exit_requested = true;
        process_exit_code      = (int)arg1;
        result = 0;
        break;

    /* ── SYS_WRITE: write(fd, buf, count) ── */
    case SYS_WRITE: {
        int32_t  fd    = (int32_t)arg1;
        const char* buf = (const char*)arg2;
        uint32_t count = arg3;

        if (!buf) { result = SYSCALL_EINVAL; break; }

        if (fd == STDOUT_FD || fd == STDERR_FD) {
            if (gui_terminal_capture_write(buf, count)) {
                result = (int32_t)count;
                break;
            }
            /* Пишем в VGA или framebuffer */
            if (fb_ready()) {
                /* В графическом режиме — TODO: вывод через GUI консоль */
                /* Пока пишем через VGA */
                for (uint32_t i = 0; i < count; i++)
                    vga_put_char(buf[i]);
            } else {
                for (uint32_t i = 0; i < count; i++)
                    vga_put_char(buf[i]);
            }
            result = (int32_t)count;
        } else {
            result = vfs_write(fd, buf, count);
        }
        break;
    }

    /* ── SYS_READ: read(fd, buf, count) ── */
    case SYS_READ: {
        int32_t  fd    = (int32_t)arg1;
        char*    buf   = (char*)arg2;
        uint32_t count = arg3;

        if (!buf) { result = SYSCALL_EINVAL; break; }

        if (fd == STDIN_FD) {
            /* Читаем с клавиатуры */
            uint32_t i = 0;
            while (i < count - 1) {
                char c = keyboard_getchar();
                if (c == '\n' || c == '\r') {
                    buf[i++] = '\n';
                    vga_put_char('\n');
                    break;
                }
                if (c == '\b') {
                    if (i > 0) { i--; vga_put_char('\b'); }
                    continue;
                }
                if (c >= 32 && c < 127) {
                    buf[i++] = c;
                    vga_put_char(c);
                }
            }
            buf[i] = '\0';
            result = (int32_t)i;
        } else {
            result = vfs_read(fd, buf, count);
        }
        break;
    }

    /* ── SYS_OPEN: open(path, flags) ── */
    case SYS_OPEN: {
        const char* path  = (const char*)arg1;
        uint32_t    flags = arg2;
        if (!path) { result = SYSCALL_EINVAL; break; }

        /* Конвертируем Linux флаги в наши VFS флаги */
        uint32_t vfs_flags = 0;
        if ((flags & 3) == O_RDONLY) vfs_flags |= VFS_O_READ;
        if ((flags & 3) == O_WRONLY) vfs_flags |= VFS_O_WRITE;
        if ((flags & 3) == O_RDWR)   vfs_flags |= VFS_O_READ | VFS_O_WRITE;
        if (flags & O_CREAT)         vfs_flags |= VFS_O_CREATE;
        if (flags & O_TRUNC)         vfs_flags |= VFS_O_TRUNC;
        if (flags & O_APPEND)        vfs_flags |= VFS_O_APPEND;

        result = vfs_open(path, vfs_flags);
        if (result < 0) result = SYSCALL_ENOENT;
        break;
    }

    /* ── SYS_CLOSE: close(fd) ── */
    case SYS_CLOSE:
        vfs_close((int32_t)arg1);
        result = 0;
        break;

    /* ── SYS_LSEEK: lseek(fd, offset, whence) ── */
    case SYS_LSEEK:
        result = vfs_seek((int32_t)arg1, (int32_t)arg2, (int)arg3);
        break;

    /* ── SYS_GETPID ── */
    case SYS_GETPID:
        result = (int32_t)current_pid;
        break;

    /* ── SYS_SBRK: sbrk(size) — выделить память ── */
    case SYS_SBRK: {
        void* ptr = kmalloc(arg1 ? arg1 : 4096);
        result = ptr ? (int32_t)(uint32_t)ptr : SYSCALL_ENOMEM;
        break;
    }

    /* ── SYS_SLEEP: sleep(ms) ── */
    case SYS_SLEEP:
        pit_sleep(arg1);
        result = 0;
        break;

    /* ── SYS_UPTIME: uptime() ── */
    case SYS_UPTIME:
        result = (int32_t)pit_get_ticks();
        break;

    /* ── SYS_PUTCHAR: быстрый вывод символа ── */
    case SYS_PUTCHAR:
        vga_put_char((char)arg1);
        result = 0;
        break;

    /* ── SYS_GETCHAR: читать символ ── */
    case SYS_GETCHAR:
        result = (int32_t)keyboard_getchar();
        break;

    /* ── SYS_FRAMEBUF: получить параметры фреймбуфера ── */
    case SYS_FRAMEBUF: {
        syscall_fb_info_t* info = (syscall_fb_info_t*)arg1;
        if (info && fb_ready()) {
            info->addr   = (uint32_t)fb_get_backbuf();
            info->width  = fb_width();
            info->height = fb_height();
            info->pitch  = fb_width() * 4;
            info->bpp    = 32;
            result = 0;
        } else {
            result = SYSCALL_EINVAL;
        }
        break;
    }

    /* ── SYS_MEMINFO ── */
    case SYS_MEMINFO: {
        uint32_t* buf = (uint32_t*)arg1;
        if (buf) {
            buf[0] = (uint32_t)pmm_get_free_pages() * 4096;
            buf[1] = (uint32_t)pmm_get_total_pages() * 4096;
            result = 0;
        } else result = SYSCALL_EINVAL;
        break;
    }

    default:
        result = SYSCALL_EINVAL;
        break;
    }

    if (ret) *ret = (uint32_t)result;
}

/* =============================================================================
 * syscall_init — зарегистрировать обработчик INT 0x80
 * ============================================================================= */

/* Объявлен в syscall_entry.asm */
extern void syscall_entry(void);

void syscall_init(void) {
    /* INT 0x80 — традиционный Linux syscall вектор */
    /* IDT_USER_INT позволяет вызывать из ring 3 (DPL=3) */
    idt_set_gate(0x80, (uint32_t)syscall_entry, 0x08, 0xEF);
    /* 0xEF = 1110_1111 = Present | DPL=3 | Interrupt Gate */
}
