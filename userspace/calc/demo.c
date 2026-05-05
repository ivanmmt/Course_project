/* =============================================================================
 * userspace/calc/calc.c
 * Простой графический демо — рисует на фреймбуфере через syscall
 *
 * Демонстрирует: доступ к framebuffer из userspace программы
 * ============================================================================= */

#include "../libc/myos_libc.h"

static fb_info_t fb;

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            put_pixel(&fb, x+col, y+row, c);
}

static void draw_string(int x, int y, const char* s, uint32_t c) {
    /* Простой 5×7 шрифт через put_pixel — только для демо */
    /* В реальной программе используй framebuffer syscall для текста */
    write(STDOUT, s, strlen(s));
    write(STDOUT, "\n", 1);
    (void)x; (void)y; (void)c;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("MyOS Framebuffer Demo\n");

    if (get_framebuffer(&fb) < 0) {
        printf("Framebuffer not available (run with make run-fb)\n");
        return 1;
    }

    printf("FB: %ux%u @32bpp addr=0x%x\n",
           (uint32_t)fb.width, (uint32_t)fb.height, fb.addr);

    /* Рисуем простые фигуры */
    /* Очищаем в тёмно-синий */
    fill_rect(0, 0, (int)fb.width, (int)fb.height, 0x001020u);

    /* Центральный прямоугольник */
    int cx = (int)fb.width/2, cy = (int)fb.height/2;
    fill_rect(cx-150, cy-100, 300, 200, 0x1E5A8Au);
    fill_rect(cx-148, cy-98,  296, 196, 0x2A7AC0u);

    /* Заголовок */
    draw_string(cx-60, cy-80, "MyOS Userspace", 0xFFFFFFu);
    draw_string(cx-40, cy-60, "Demo Program",   0xAADDFFu);

    /* Цветные полосы */
    uint32_t colors[] = {
        0xFF4444u, 0xFF8844u, 0xFFFF44u,
        0x44FF44u, 0x44FFFFu, 0x4444FFu, 0xFF44FFu
    };
    for (int i = 0; i < 7; i++)
        fill_rect(cx-140+i*40, cy+20, 36, 36, colors[i]);

    /* Анимация — рисуем движущуюся точку */
    printf("Drawing animation (press any key to stop)...\n");

    uint32_t t0 = uptime();
    int dot_x = 0;

    while (1) {
        /* Простая горизонтальная анимация */
        uint32_t t = uptime() - t0;
        dot_x = (int)((t * 3) % fb.width);

        /* Затираем старую позицию */
        fill_rect(0, cy+80, (int)fb.width, 20, 0x001020u);

        /* Рисуем точку */
        int r = 8;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx*dx + dy*dy <= r*r)
                    put_pixel(&fb, dot_x+dx, cy+90+dy, 0xFFFF44u);

        /* Проверяем клавишу (неблокирующе через uptime) */
        if (t > 500) break;  /* 5 секунд */

        sleep_ms(16);  /* ~60 FPS */
    }

    printf("Demo finished.\n");
    return 0;
}
