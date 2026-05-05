/* =============================================================================
 * kernel/syscall/syscall.h
 * Системные вызовы — интерфейс между пользовательскими программами и ядром
 *
 * КАК РАБОТАЮТ СИСТЕМНЫЕ ВЫЗОВЫ:
 *
 * Пользовательский код работает в ring 0 (пока нет ring 3, это упрощение).
 * Программа вызывает функцию из нашей libc, которая выполняет:
 *   1. Кладёт номер syscall в EAX
 *   2. Аргументы в EBX, ECX, EDX
 *   3. Выполняет INT 0x80
 *   4. IDT[0x80] → syscall_handler() в ядре
 *   5. Ядро выполняет операцию, результат в EAX
 *   6. IRET возвращает управление программе
 *
 * Это точно та же схема что использует Linux (legacy 32-bit syscall ABI).
 * Наши программы даже можно будет портировать на Linux с минимальными правками.
 * ============================================================================= */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── Номера системных вызовов ─── */
/* Совместимы с Linux x86 для удобства */
#define SYS_EXIT        1
#define SYS_WRITE       4
#define SYS_READ        3
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_LSEEK       19
#define SYS_GETPID      20
#define SYS_SBRK        45   /* расширить кучу процесса */
#define SYS_SLEEP       162  /* sleep(ms) */
#define SYS_UPTIME      200  /* MyOS extension: uptime в тиках */
#define SYS_PUTCHAR     201  /* быстрый вывод символа */
#define SYS_GETCHAR     202  /* чтение символа */
#define SYS_FRAMEBUF    203  /* получить адрес фреймбуфера */
#define SYS_MEMINFO     204  /* статистика памяти */

/* ─── Стандартные файловые дескрипторы ─── */
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

/* ─── Флаги open() ─── */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400

/* ─── Код возврата ─── */
#define SYSCALL_OK      0
#define SYSCALL_ENOENT  (-2)
#define SYSCALL_EBADF   (-9)
#define SYSCALL_ENOMEM  (-12)
#define SYSCALL_EINVAL  (-22)

/* ─── Структура фреймбуфера для userspace ─── */
typedef struct {
    uint32_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
} syscall_fb_info_t;

/* ─── API ядра ─── */
void syscall_init(void);

/* Обработчик INT 0x80 (вызывается из IDT) */
void syscall_handler_c(uint32_t num,
                        uint32_t arg1, uint32_t arg2, uint32_t arg3,
                        uint32_t* ret);

#endif /* SYSCALL_H */

/* Вспомогательные функции для process manager */
bool syscall_exit_requested(void);
int  syscall_exit_code(void);
void syscall_clear_exit(void);
