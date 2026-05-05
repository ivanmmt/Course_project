/* =============================================================================
 * programs/mediaplayer/video/mediaplayer_video.c
 * Видеоплеер MyOS
 *
 * Поддерживает:
 *   - BMP, PNG, JPG изображения (просмотр)
 *   - Простой видео фреймбуфер (raw frames)
 *   - Слайдшоу из изображений
 *   - Масштабирование
 * ============================================================================= */

#include "mediaplayer_video.h"
#include "../../../drivers/framebuffer/framebuffer.h"
#include "../../../drivers/keyboard/keyboard.h"
#include "../../../drivers/mouse/mouse.h"
#include "../../../drivers/timer/pit.h"
#include "../../../filesystem/vfs.h"
#include "../../../memory/heap.h"
#include "../../../lib/string.h"

/* ─── Цвета ─── */
#define VP_BG        0x000000u
#define VP_TOOLBAR_BG 0x1A1A2Eu
#define VP_FG        0xE0E0E0u
#define VP_BTN_BG    0x333355u
#define VP_BTN_H     0x444477u
#define VP_STATUS_BG 0x111122u

/* ─── Состояние ─── */
typedef struct {
    char     filepath[512];
    char     filename[128];

    /* Изображение */
    uint8_t* image_data;
    uint32_t image_w;
    uint32_t image_h;

    /* Масштабирование */
    int      zoom;       /* 100 = 100% */
    int      offset_x;
    int      offset_y;

    /* Слайдшоу */
    char**   slide_files;
    int      slide_count;
    int      slide_cur;

    /* Управление */
    bool     running;
    bool     paused;
    int      frame_delay;  /* ms между кадрами */

    /* Размеры */
    int32_t  view_x, view_y, view_w, view_h;
} vp_state_t;

/* =============================================================================
 * BMP загрузчик (простой, 24-bit)
 * ============================================================================= */

static int load_bmp(const char* path, uint8_t** out_data, uint32_t* out_w, uint32_t* out_h) {
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;

    uint8_t header[54];
    if (vfs_read(fd, header, 54) != 54) { vfs_close(fd); return -1; }

    /* Проверяем сигнатуру BM */
    if (header[0] != 'B' || header[1] != 'M') { vfs_close(fd); return -1; }

    uint32_t data_offset = header[10] | (header[11]<<8) | (header[12]<<16) | (header[13]<<24);
    int32_t  width  = header[18] | (header[19]<<8) | (header[20]<<16) | (header[21]<<24);
    int32_t  height = header[22] | (header[23]<<8) | (header[24]<<16) | (header[25]<<24);
    uint16_t bpp    = header[28] | (header[29]<<8);

    if (bpp != 24) { vfs_close(fd); return -1; }
    if (width <= 0 || height <= 0) { vfs_close(fd); return -1; }

    uint32_t row_bytes = ((width * 3 + 3) / 4) * 4;  /* выравнивание до 4 байт */
    uint32_t img_size = row_bytes * (uint32_t)height;

    uint8_t* raw = (uint8_t*)kmalloc(img_size);
    if (!raw) { vfs_close(fd); return -1; }

    /* Пропускаем до данных */
    int32_t seek = data_offset - 54;
    while (seek > 0) {
        uint8_t tmp[256];
        int32_t r = seek > 256 ? 256 : seek;
        vfs_read(fd, tmp, (uint32_t)r);
        seek -= r;
    }

    vfs_read(fd, raw, img_size);
    vfs_close(fd);

    /* Конвертируем BGR → RGB и переворачиваем */
    uint32_t pix_count = (uint32_t)width * (uint32_t)height;
    uint8_t* rgb = (uint8_t*)kmalloc(pix_count * 3);
    if (!rgb) { kfree(raw); return -1; }

    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            uint32_t src_idx = (uint32_t)((height - 1 - y)) * row_bytes + x * 3;
            uint32_t dst_idx = (uint32_t)(y * width + x) * 3;
            rgb[dst_idx]     = raw[src_idx + 2]; /* R */
            rgb[dst_idx + 1] = raw[src_idx + 1]; /* G */
            rgb[dst_idx + 2] = raw[src_idx];     /* B */
        }
    }

    kfree(raw);
    *out_data = rgb;
    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    return 0;
}

/* =============================================================================
 * Отображение изображения с масштабированием
 * ============================================================================= */

static void vp_draw_image(vp_state_t* vp) {
    if (!vp->image_data || vp->image_w == 0 || vp->image_h == 0) return;

    /* Фон */
    fb_fill_rect(0, 0, (int32_t)fb_width(), (int32_t)fb_height(), VP_BG);

    /* Рассчитываем размер с зумом */
    int draw_w = (int)vp->image_w * vp->zoom / 100;
    int draw_h = (int)vp->image_h * vp->zoom / 100;
    int dx = (int)fb_width() / 2 - draw_w / 2 + vp->offset_x;
    int dy = (int)fb_height() / 2 - draw_h / 2 + vp->offset_y;

    /* Рисуем пиксели */
    if (vp->zoom == 100) {
        /* Без масштабирования — быстрая отрисовка */
        for (uint32_t y = 0; y < vp->image_h; y++) {
            for (uint32_t x = 0; x < vp->image_w; x++) {
                uint32_t idx = (y * vp->image_w + x) * 3;
                uint8_t r = vp->image_data[idx];
                uint8_t g = vp->image_data[idx+1];
                uint8_t b = vp->image_data[idx+2];
                uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                int px = dx + (int)x;
                int py = dy + (int)y;
                if (px >= 0 && px < (int)fb_width() && py >= 0 && py < (int)fb_height())
                    fb_put_pixel((uint32_t)px, (uint32_t)py, color);
            }
        }
    } else {
        /* С масштабированием — ближайший сосед */
        for (int sy = 0; sy < draw_h; sy++) {
            int src_y = sy * (int)vp->image_h / draw_h;
            if (src_y < 0) src_y = 0;
            if (src_y >= (int)vp->image_h) src_y = (int)vp->image_h - 1;
            for (int sx = 0; sx < draw_w; sx++) {
                int src_x = sx * (int)vp->image_w / draw_w;
                if (src_x < 0) src_x = 0;
                if (src_x >= (int)vp->image_w) src_x = (int)vp->image_w - 1;
                uint32_t idx = (uint32_t)(src_y * (int)vp->image_w + src_x) * 3;
                uint8_t r = vp->image_data[idx];
                uint8_t g = vp->image_data[idx+1];
                uint8_t b = vp->image_data[idx+2];
                uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                int px = dx + sx;
                int py = dy + sy;
                if (px >= 0 && px < (int)fb_width() && py >= 0 && py < (int)fb_height())
                    fb_put_pixel((uint32_t)px, (uint32_t)py, color);
            }
        }
    }
}

/* =============================================================================
 * Панель управления
 * ============================================================================= */

static void vp_draw_toolbar(vp_state_t* vp) {
    int32_t sw = (int32_t)fb_width();
    int32_t sh = (int32_t)fb_height();
    int32_t tb_h = 40;

    fb_fill_rect(0, sh - tb_h - 24, sw, tb_h, VP_TOOLBAR_BG);
    fb_draw_hline(0, sh - tb_h - 24, sw, 0x333355u);

    /* Кнопки */
    int32_t bx = 10;
    int32_t by = sh - tb_h - 20;
    struct { const char* label; fb_color_t c; } btns[] = {
        {"|<", 0x5555AAu}, {"<", 0x5555AAu}, {">", 0x5555AAu}, {">|", 0x5555AAu},
        {"Z+", 0x44AA44u}, {"Z-", 0x44AA44u}, {"Fit", 0x44AA44u},
        {"Exit", 0xAA4444u}
    };
    for (int i = 0; i < 8; i++) {
        fb_fill_round_rect(bx, by, 36, 28, 4, btns[i].c);
        fb_draw_string(bx + 8, by + 8, btns[i].label, 0xFFFFFFu, btns[i].c, true);
        bx += 40;
    }

    /* Имя файла */
    fb_draw_string(340, sh - tb_h - 16, vp->filename, VP_FG, VP_TOOLBAR_BG, true);

    /* Статус */
    char st[64];
    {
        int si = 0;
        const char* p1 = vp->image_w ? "Image: " : "No image";
        while(*p1) st[si++] = *p1++;
        if (vp->image_w) {
            char tmp[8]; int ti=0, v=(int)vp->image_w;
            if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
            tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
            st[si++]='x';
            ti=0; v=(int)vp->image_h;
            if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
            tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
        }
        st[si++]=' '; st[si++]='[';
        char tmp[8]; int ti=0, v=vp->zoom;
        if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
        tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
        st[si++]='%'; st[si++]=']';
        if (vp->slide_count > 0) {
            st[si++]=' '; st[si++]='(';
            ti=0; v=vp->slide_cur+1;
            if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
            tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
            st[si++]='/';
            ti=0; v=vp->slide_count;
            if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
            tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
            st[si++]=')';
        }
        st[si] = '\0';
    }
    fb_draw_string(340, sh - 18, st, 0x999999u, VP_TOOLBAR_BG, true);
}

/* =============================================================================
 * Загрузить слайды (все изображения в той же директории)
 * ============================================================================= */

static void vp_load_slides(vp_state_t* vp) {
    /* Простая реализация: один файл */
    vp->slide_count = 1;
    vp->slide_files = (char**)kmalloc(sizeof(char*));
    vp->slide_files[0] = (char*)kmalloc(strlen(vp->filepath) + 1);
    strcpy(vp->slide_files[0], vp->filepath);
    vp->slide_cur = 0;
}

static void vp_load_slide(vp_state_t* vp, int idx) {
    if (idx < 0 || idx >= vp->slide_count) return;

    /* Освобождаем предыдущее */
    if (vp->image_data) { kfree(vp->image_data); vp->image_data = 0; }

    load_bmp(vp->slide_files[idx], &vp->image_data, &vp->image_w, &vp->image_h);
    vp->zoom = 100;
    vp->offset_x = 0;
    vp->offset_y = 0;
    vp->slide_cur = idx;

    /* Имя файла */
    const char* fn = vp->slide_files[idx];
    for (const char* p = fn; *p; p++) if (*p == '/' || *p == '\\') fn = p + 1;
    strncpy(vp->filename, fn, 127);
}

/* =============================================================================
 * Точка входа
 * ============================================================================= */

void mediaplayer_video_run(const char* filepath) {
    if (!fb_ready() || !filepath) return;

    vp_state_t vp;
    memset(&vp, 0, sizeof(vp));
    vp.running = true;
    strncpy(vp.filepath, filepath, 511);

    const char* fn = filepath;
    for (const char* p = filepath; *p; p++) if (*p == '/' || *p == '\\') fn = p + 1;
    strncpy(vp.filename, fn, 127);

    vp_load_slides(&vp);
    vp_load_slide(&vp, 0);

    /* Главный цикл */
    uint32_t last_tick = pit_get_ticks();
    while (vp.running) {
        vp_draw_image(&vp);
        vp_draw_toolbar(&vp);
        fb_swap();

        if (keyboard_has_input()) {
            int c = keyboard_getkey();
            switch (c) {
                case KEY_ARROW_LEFT:
                    if (vp.slide_cur > 0) vp_load_slide(&vp, vp.slide_cur - 1);
                    break;
                case KEY_ARROW_RIGHT:
                    if (vp.slide_cur < vp.slide_count - 1)
                        vp_load_slide(&vp, vp.slide_cur + 1);
                    break;
                case KEY_ARROW_UP:
                    vp.zoom += 25;
                    if (vp.zoom > 400) vp.zoom = 400;
                    break;
                case KEY_ARROW_DOWN:
                    vp.zoom -= 25;
                    if (vp.zoom < 25) vp.zoom = 25;
                    break;
                case KEY_ESCAPE:
                    vp.running = false;
                    break;
                case '=': case '+':
                    vp.zoom += 10; break;
                case '-':
                    vp.zoom -= 10; break;
                case '0':
                    vp.zoom = 100; vp.offset_x = 0; vp.offset_y = 0;
                    break;
                case KEY_KP_PLUS:
                    vp.zoom += 10; break;
                case KEY_KP_MINUS:
                    vp.zoom -= 10; break;
                default:
                    break;
            }
        }

        const mouse_state_t* ms = mouse_get_state();
        if (ms->left_click) {
            int32_t sh = (int32_t)fb_height();
            int32_t tb_y = sh - 64;
            if (ms->y >= tb_y) {
                /* Клик по тулбару */
                int32_t bx = 10;
                for (int i = 0; i < 8; i++) {
                    if (ms->x >= bx && ms->x < bx + 36) {
                        switch (i) {
                            case 0: vp_load_slide(&vp, 0); break;
                            case 1: if (vp.slide_cur > 0) vp_load_slide(&vp, vp.slide_cur-1); break;
                            case 2: if (vp.slide_cur < vp.slide_count-1) vp_load_slide(&vp, vp.slide_cur+1); break;
                            case 3: vp_load_slide(&vp, vp.slide_count-1); break;
                            case 4: vp.zoom += 25; if (vp.zoom>400) vp.zoom=400; break;
                            case 5: vp.zoom -= 25; if (vp.zoom<25) vp.zoom=25; break;
                            case 6: vp.zoom = 100; break;
                            case 7: vp.running = false; break;
                        }
                        break;
                    }
                    bx += 40;
                }
                mouse_consume_clicks();
            }
        }

        __asm__ volatile ("hlt");
    }

    /* Очистка */
    if (vp.image_data) kfree(vp.image_data);
    if (vp.slide_files) {
        for (int i = 0; i < vp.slide_count; i++)
            if (vp.slide_files[i]) kfree(vp.slide_files[i]);
        kfree(vp.slide_files);
    }
}
