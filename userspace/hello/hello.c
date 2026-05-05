/* =============================================================================
 * userspace/hello/hello.c
 * Первая программа для MyOS — "Hello, World!"
 *
 * Компилируется ОТДЕЛЬНО от ядра:
 *   i686-linux-gnu-gcc -m32 -ffreestanding -fno-pic -fno-pie -no-pie \
 *     -nostdlib -O2 -o hello.elf hello.c
 *   strip hello.elf  (уменьшаем размер)
 *
 * Запускается из шелла:
 *   exec /mnt/disk0/hello.elf
 *   (или если положить в ramfs)
 * ============================================================================= */

#include "../libc/myos_libc.h"

/* Точка входа — как main() в обычной программе */
int main(int argc, char** argv) {
    printf("Hello from MyOS userspace!\n");
    printf("  PID:    %d\n", getpid());
    printf("  Uptime: %u ticks\n", uptime());
    printf("  argc:   %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }

    /* Тест работы с файлами через syscall */
    int fd = open("/etc/motd", O_RDONLY);
    if (fd >= 0) {
        char buf[128];
        int n = (int)read(fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            printf("\n/etc/motd: %s", buf);
        }
        close(fd);
    }

    printf("\nHello program finished. Returning to kernel.\n");
    return 0;
}
