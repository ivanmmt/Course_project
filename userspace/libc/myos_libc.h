/* =============================================================================
 * userspace/libc/myos_libc.h
 * Минимальная стандартная библиотека для программ MyOS
 *
 * Программы включают этот заголовок и используют знакомые функции:
 *   printf(), malloc(), fopen(), exit() и т.д.
 *
 * Под капотом каждая функция делает INT 0x80 с нужным номером syscall.
 * ============================================================================= */

#ifndef MYOS_LIBC_H
#define MYOS_LIBC_H

/* Без системных заголовков — всё своё */
typedef unsigned int    uint32_t;
typedef unsigned short  uint16_t;
typedef unsigned char   uint8_t;
typedef int             int32_t;
typedef unsigned int    size_t;
typedef int             ssize_t;
typedef int             bool;
#define true  1
#define false 0
#define NULL  ((void*)0)

/* ─── Номера syscall (должны совпадать с kernel/syscall/syscall.h) ─── */
#define SYS_EXIT        1
#define SYS_WRITE       4
#define SYS_READ        3
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_LSEEK       19
#define SYS_GETPID      20
#define SYS_SBRK        45
#define SYS_SLEEP       162
#define SYS_UPTIME      200
#define SYS_PUTCHAR     201
#define SYS_GETCHAR     202
#define SYS_FRAMEBUF    203
#define SYS_MEMINFO     204

/* Флаги open() */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

/* Стандартные дескрипторы */
#define STDIN   0
#define STDOUT  1
#define STDERR  2

/* =============================================================================
 * Макрос системного вызова
 * Кладёт аргументы в регистры и вызывает INT 0x80
 * ============================================================================= */
static inline int32_t syscall0(uint32_t num) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline int32_t syscall1(uint32_t num, uint32_t a1) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1)
        : "memory"
    );
    return ret;
}

static inline int32_t syscall2(uint32_t num, uint32_t a1, uint32_t a2) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2)
        : "memory"
    );
    return ret;
}

static inline int32_t syscall3(uint32_t num,
                                uint32_t a1, uint32_t a2, uint32_t a3) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

/* =============================================================================
 * Стандартные функции — реализованы inline через syscall
 * ============================================================================= */

/* Завершить программу */
static inline void exit(int code) {
    syscall1(SYS_EXIT, (uint32_t)code);
    /* Если вернулись (не должны) — бесконечный цикл */
    while(1) {}
}

/* Получить PID */
static inline int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

/* Записать в дескриптор */
static inline ssize_t write(int fd, const void* buf, size_t n) {
    return (ssize_t)syscall3(SYS_WRITE, (uint32_t)fd,
                              (uint32_t)buf, (uint32_t)n);
}

/* Читать из дескриптора */
static inline ssize_t read(int fd, void* buf, size_t n) {
    return (ssize_t)syscall3(SYS_READ, (uint32_t)fd,
                              (uint32_t)buf, (uint32_t)n);
}

/* Открыть файл */
static inline int open(const char* path, int flags) {
    return (int)syscall2(SYS_OPEN, (uint32_t)path, (uint32_t)flags);
}

/* Закрыть файл */
static inline int close(int fd) {
    return (int)syscall1(SYS_CLOSE, (uint32_t)fd);
}

/* Задержка в миллисекундах */
static inline void sleep_ms(uint32_t ms) {
    syscall1(SYS_SLEEP, ms);
}

/* Аптайм в тиках (100 Hz) */
static inline uint32_t uptime(void) {
    return (uint32_t)syscall0(SYS_UPTIME);
}

/* Вывод одного символа */
static inline void putchar(char c) {
    syscall1(SYS_PUTCHAR, (uint32_t)(uint8_t)c);
}

/* Читать один символ с клавиатуры */
static inline int getchar(void) {
    return (int)syscall0(SYS_GETCHAR);
}

/* =============================================================================
 * Строковые функции (не требуют syscall)
 * ============================================================================= */

static inline size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void* memset(void* ptr, int val, size_t n) {
    uint8_t* p = (uint8_t*)ptr;
    while (n--) *p++ = (uint8_t)val;
    return ptr;
}

static inline int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

/* =============================================================================
 * puts — вывод строки с переводом строки
 * ============================================================================= */
static inline int puts(const char* s) {
    write(STDOUT, s, strlen(s));
    write(STDOUT, "\n", 1);
    return 0;
}

/* =============================================================================
 * Минимальный printf (поддерживает %s %d %u %x %c %%)
 * ============================================================================= */
static inline void _print_uint(uint32_t n, int base) {
    char buf[32];
    const char* digits = "0123456789abcdef";
    int i = 0;
    if (n == 0) { putchar('0'); return; }
    while (n > 0) { buf[i++] = digits[n % base]; n /= base; }
    while (i > 0) putchar(buf[--i]);
}

static inline int printf(const char* fmt, ...) {
    /* Доступ к variadic args через stack */
    uint32_t* args = (uint32_t*)((uint8_t*)&fmt + sizeof(fmt));
    int ai = 0;

    while (*fmt) {
        if (*fmt != '%') { putchar(*fmt++); continue; }
        fmt++;
        switch (*fmt) {
            case 's': {
                const char* s = (const char*)args[ai++];
                if (s) write(STDOUT, s, strlen(s));
                else write(STDOUT, "(null)", 6);
                break;
            }
            case 'd': {
                int32_t n = (int32_t)args[ai++];
                if (n < 0) { putchar('-'); n = -n; }
                _print_uint((uint32_t)n, 10);
                break;
            }
            case 'u': _print_uint(args[ai++], 10); break;
            case 'x': _print_uint(args[ai++], 16); break;
            case 'c': putchar((char)args[ai++]); break;
            case '%': putchar('%'); break;
            default:  putchar('%'); putchar(*fmt); break;
        }
        fmt++;
    }
    return 0;
}

/* =============================================================================
 * Простой malloc через sbrk
 * ============================================================================= */
static inline void* malloc(size_t size) {
    return (void*)(uint32_t)syscall1(SYS_SBRK, (uint32_t)size);
}

static inline void free(void* ptr) {
    (void)ptr; /* В нашей упрощённой реализации не освобождаем */
}

/* =============================================================================
 * Информация о фреймбуфере
 * ============================================================================= */
typedef struct {
    uint32_t addr, width, height, pitch;
    uint8_t  bpp;
} fb_info_t;

static inline int get_framebuffer(fb_info_t* info) {
    return (int)syscall1(SYS_FRAMEBUF, (uint32_t)info);
}

/* Быстрый доступ к пикселям */
static inline void put_pixel(fb_info_t* fb, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height)
        return;
    uint32_t* buf = (uint32_t*)fb->addr;
    buf[(uint32_t)y * fb->width + (uint32_t)x] = color;
}

#endif /* MYOS_LIBC_H */
