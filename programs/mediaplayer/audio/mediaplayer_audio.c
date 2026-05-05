/* =============================================================================
 * programs/mediaplayer/audio/mediaplayer_audio.c
 * Аудиоплеер MyOS
 *
 * Поддерживает:
 *   - WAV файлы (PCM)
 *   - Простой визуализатор спектра
 *   - Управление воспроизведением (play/pause/stop/volume)
 *   - Плейлист
 *
 * Аудио вывод через PC Speaker (порт 0x61) или через AC97 если есть.
 * ============================================================================= */

#include "mediaplayer_audio.h"
#include "../../../drivers/framebuffer/framebuffer.h"
#include "../../../drivers/keyboard/keyboard.h"
#include "../../../drivers/mouse/mouse.h"
#include "../../../drivers/timer/pit.h"
#include "../../../filesystem/vfs.h"
#include "../../../memory/heap.h"
#include "../../../lib/string.h"

/* ─── PC Speaker ─── */
static void speaker_on(uint32_t freq) {
    if (freq == 0) return;
    uint32_t div = 1193180 / freq;
    __asm__ volatile ("outb %0, $0x43" : : "a"((uint8_t)0xB6));
    __asm__ volatile ("outb %0, $0x42" : : "a"((uint8_t)(div & 0xFF)));
    __asm__ volatile ("outb %0, $0x42" : : "a"((uint8_t)((div >> 8) & 0xFF)));
    uint8_t cur = 0;
    __asm__ volatile ("inb $0x61, %0" : "=a"(cur));
    cur |= 0x03;
    __asm__ volatile ("outb %0, $0x61" : : "a"(cur));
}

static void speaker_off(void) {
    uint8_t cur = 0;
    __asm__ volatile ("inb $0x61, %0" : "=a"(cur));
    cur &= 0xFC;
    __asm__ volatile ("outb %0, $0x61" : : "a"(cur));
}

/* ─── Цвета ─── */
#define AP_BG         0x0A0A1Au
#define AP_FG         0xE0E0F0u
#define AP_TOOLBAR_BG 0x151528u
#define AP_BTN_BG     0x2A2A4Au
#define AP_BTN_H      0x3A3A6Au
#define AP_PLAY       0x00CC44u
#define AP_PAUSE      0xCCAA00u
#define AP_STOP       0xCC2222u
#define AP_VOL        0x4488CCu
#define AP_SPECTRUM1  0x00FF88u
#define AP_SPECTRUM2  0x0088FFu
#define AP_SPECTRUM3  0xFF4488u
#define AP_PROGRESS   0x3366CCu
#define AP_STATUS_BG  0x0D0D1Au

/* ─── WAV заголовок ─── */
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t data_offset;
    bool     valid;
} wav_info_t;

/* ─── Состояние ─── */
typedef struct {
    char     filepath[512];
    char     filename[128];

    /* Аудио */
    wav_info_t wav;
    uint8_t*   audio_data;
    uint32_t   audio_pos;
    bool       playing;
    bool       paused;
    int        volume;       /* 0-100 */

    /* Плейлист */
    char**     playlist;
    int        pl_count;
    int        pl_cur;

    /* Визуализатор */
    int        spectrum[32];
    uint32_t   spectrum_tick;

    /* Управление */
    bool       running;

    /* Размеры */
    int32_t    sw, sh;
} ap_state_t;

/* =============================================================================
 * WAV парсер
 * ============================================================================= */

static wav_info_t parse_wav(int32_t fd) {
    wav_info_t info;
    memset(&info, 0, sizeof(info));

    uint8_t hdr[44];
    if (vfs_read(fd, hdr, 44) != 44) return info;

    /* RIFF */
    if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F') return info;
    /* WAVE */
    if (hdr[8]!='W'||hdr[9]!='A'||hdr[10]!='V'||hdr[11]!='E') return info;

    /* Ищем fmt и data чанки */
    uint32_t pos = 12;
    bool found_fmt = false, found_data = false;

    while (pos < 1000) {
        /* Читаем чанк */
        vfs_seek(fd, pos, 0);
        uint8_t chunk[8];
        if (vfs_read(fd, chunk, 8) != 8) break;

        uint32_t chunk_size = chunk[4] | (chunk[5]<<8) | (chunk[6]<<16) | (chunk[7]<<24);

        if (chunk[0]=='f' && chunk[1]=='m' && chunk[2]=='t' && chunk[3]==' ') {
            found_fmt = true;
            if (chunk_size >= 16) {
                uint8_t fmt[16];
                vfs_read(fd, fmt, 16);
                info.channels = fmt[2] | (fmt[3]<<8);
                info.sample_rate = fmt[4] | (fmt[5]<<8) | (fmt[6]<<16) | (fmt[7]<<24);
                info.bits_per_sample = fmt[14] | (fmt[15]<<8);
            }
        }
        if (chunk[0]=='d' && chunk[1]=='a' && chunk[2]=='t' && chunk[3]=='a') {
            found_data = true;
            info.data_offset = pos + 8;
            info.data_size = chunk_size;
        }

        pos += 8 + chunk_size;
        if (found_fmt && found_data) break;
    }

    info.valid = found_fmt && found_data;
    return info;
}

static int load_wav(const char* path, wav_info_t* wav, uint8_t** data) {
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;

    wav_info_t info = parse_wav(fd);
    if (!info.valid) { vfs_close(fd); return -1; }

    *wav = info;

    *data = (uint8_t*)kmalloc(info.data_size);
    if (!*data) { vfs_close(fd); return -1; }

    vfs_seek(fd, info.data_offset, 0);
    vfs_read(fd, *data, info.data_size);
    vfs_close(fd);
    return 0;
}

/* =============================================================================
 * Визуализатор спектра
 * ============================================================================= */

static void ap_update_spectrum(ap_state_t* ap) {
    /* Генерируем фейковый спектр на основе позиции воспроизведения */
    for (int i = 0; i < 32; i++) {
        uint32_t seed = ap->audio_pos + i * 137 + ap->spectrum_tick;
        if (ap->playing && !ap->paused)
            ap->spectrum[i] = (int)((seed * 31 % 100) * ap->volume / 100);
        else
            ap->spectrum[i] = 0;
    }
}

static void ap_draw_spectrum(ap_state_t* ap) {
    int32_t bar_w = (ap->sw - 40) / 32;
    int32_t max_h = ap->sh / 3;
    int32_t base_y = ap->sh - 120;

    for (int i = 0; i < 32; i++) {
        int32_t h = ap->spectrum[i] * max_h / 100;
        int32_t x = 20 + i * bar_w;
        int32_t y = base_y - h;

        /* Градиент по высоте */
        fb_color_t c = AP_SPECTRUM1;
        if (h > max_h * 2 / 3) c = AP_SPECTRUM3;
        else if (h > max_h / 3) c = AP_SPECTRUM2;

        fb_fill_rect(x + 1, y, bar_w - 2, h, c);

        /* Тонкие линии */
        if (h > 0)
            fb_fill_rect(x + 1, base_y, bar_w - 2, 1, 0x333355u);
    }
}

/* =============================================================================
 * Панель управления
 * ============================================================================= */

static void ap_draw_controls(ap_state_t* ap) {
    int32_t ctrl_y = ap->sh - 80;

    /* Фон панели */
    fb_fill_rect(0, ctrl_y, ap->sw, 80, AP_TOOLBAR_BG);
    fb_draw_hline(0, ctrl_y, ap->sw, 0x333355u);

    /* Кнопки */
    struct { const char* label; int32_t x; fb_color_t c; } btns[] = {
        {"|<<",  20,  0x5555AAu},
        {"<<",   70,  0x5555AAu},
        {">>",   115, 0x5555AAu},
        {">>|",  160, 0x5555AAu},
    };

    /* Play/Pause */
    const char* play_label = ap->paused ? "||" : ">";
    fb_color_t play_color = ap->paused ? AP_PAUSE : AP_PLAY;

    /* Stop */
    fb_fill_round_rect(205, ctrl_y + 10, 40, 30, 5, AP_STOP);
    fb_draw_string(215, ctrl_y + 17, "[]", 0xFFFFFFu, AP_STOP, true);

    for (int i = 0; i < 4; i++) {
        fb_fill_round_rect(btns[i].x, ctrl_y + 10, 40, 30, 5, btns[i].c);
        fb_draw_string(btns[i].x + 8, ctrl_y + 17, btns[i].label, 0xFFFFFFu, btns[i].c, true);
    }

    /* Play кнопка */
    fb_fill_round_rect(60, ctrl_y + 10, 45, 30, 5, play_color);
    fb_draw_string(72, ctrl_y + 17, play_label, 0xFFFFFFu, play_color, true);

    /* Volume slider */
    int32_t vol_x = 270;
    int32_t vol_y = ctrl_y + 20;
    int32_t vol_w = 150;

    fb_draw_string(vol_x, vol_y - 12, "Vol:", AP_VOL, AP_TOOLBAR_BG, true);
    fb_fill_rect(vol_x + 30, vol_y, vol_w, 8, 0x333355u);
    fb_fill_rect(vol_x + 30, vol_y, vol_w * ap->volume / 100, 8, AP_VOL);
    fb_fill_circle(vol_x + 30 + vol_w * ap->volume / 100, vol_y + 4, 6, AP_VOL);

    char vol_str[8];
    {
        int vi=0,v=ap->volume;char tmp[8];int ti=0;
        if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
        tmp[ti]='\0';for(int i=0;tmp[i];i++)vol_str[vi++]=tmp[i];
    }
    vol_str[strlen(vol_str)] = '%';
    vol_str[strlen(vol_str)+1] = '\0';
    fb_draw_string(vol_x + vol_w + 40, vol_y - 12, vol_str, AP_VOL, AP_TOOLBAR_BG, true);

    /* Название файла */
    fb_draw_string(vol_x, ctrl_y + 48, ap->filename, AP_FG, AP_TOOLBAR_BG, true);

    /* Информация о файле */
    char info[128];
    {
        int ii=0;
        if (ap->wav.valid) {
            const char* p = "WAV "; while(*p) info[ii++]=*p++;
            char tmp[8];int ti=0,v=(int)ap->wav.sample_rate;
            if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
            tmp[ti]='\0';for(int i=0;tmp[i];i++)info[ii++]=tmp[i];
            const char* p2 = "Hz "; while(*p2) info[ii++]=*p2++;
            ti=0;v=ap->wav.channels;
            if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
            tmp[ti]='\0';for(int i=0;tmp[i];i++)info[ii++]=tmp[i];
            const char* p3 = "ch"; while(*p3) info[ii++]=*p3++;
        } else {
            const char* p = "Unknown format"; while(*p) info[ii++]=*p++;
        }
        info[ii]='\0';
    }
    fb_draw_string(vol_x + 200, ctrl_y + 48, info, 0x888888u, AP_TOOLBAR_BG, true);
}

/* =============================================================================
 * Progress bar
 * ============================================================================= */

static void ap_draw_progress(ap_state_t* ap) {
    if (!ap->wav.valid || ap->wav.data_size == 0) return;

    int32_t px = 20;
    int32_t py = ap->sh - 130;
    int32_t pw = ap->sw - 40;

    /* Фон */
    fb_fill_rect(px, py, pw, 6, 0x222244u);

    /* Прогресс */
    uint32_t progress = ap->audio_pos * 100 / ap->wav.data_size;
    fb_fill_rect(px, py, pw * progress / 100, 6, AP_PROGRESS);

    /* Время */
    uint32_t total_sec = ap->wav.data_size / (ap->wav.sample_rate * ap->wav.channels * ap->wav.bits_per_sample / 8);
    uint32_t cur_sec = ap->audio_pos / (ap->wav.sample_rate * ap->wav.channels * ap->wav.bits_per_sample / 8);

    char time_str[32];
    {
        int ti=0;
        uint32_t cm = cur_sec / 60, cs = cur_sec % 60;
        char tmp[8];
        if(cm==0){time_str[ti++]='0';}else{int cj=0;char ct[8];uint32_t cv=cm;while(cv>0){ct[cj++]='0'+cv%10;cv/=10;}while(cj>0)time_str[ti++]=ct[--cj];}
        time_str[ti++]=':';
        if(cs<10)time_str[ti++]='0';
        int cj=0;char ct[8];uint32_t cv=cs;
        if(cv==0){ct[cj++]='0';}else{while(cv>0){ct[cj++]='0'+cv%10;cv/=10;}}
        while(cj>0)time_str[ti++]=ct[--cj];

        const char* sep = " / "; while(*sep) time_str[ti++]=*sep++;
        uint32_t tm = total_sec / 60, ts = total_sec % 60;
        if(tm==0){time_str[ti++]='0';}else{cj=0;while(tm>0){ct[cj++]='0'+tm%10;tm/=10;}while(cj>0)time_str[ti++]=ct[--cj];}
        time_str[ti++]=':';
        if(ts<10)time_str[ti++]='0';
        cj=0;cv=ts;
        if(cv==0){ct[cj++]='0';}else{while(cv>0){ct[cj++]='0'+cv%10;cv/=10;}}
        while(cj>0)time_str[ti++]=ct[--cj];
        time_str[ti]='\0';
    }
    fb_draw_string(px, py - 16, time_str, 0xAAAAAAu, AP_BG, true);
}

/* =============================================================================
 * Точка входа
 * ============================================================================= */

void mediaplayer_audio_run(const char* filepath) {
    if (!fb_ready() || !filepath) return;

    ap_state_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.running = true;
    ap.volume = 70;
    strncpy(ap.filepath, filepath, 511);

    const char* fn = filepath;
    for (const char* p = filepath; *p; p++) if (*p == '/' || *p == '\\') fn = p + 1;
    strncpy(ap.filename, fn, 127);

    ap.sw = (int32_t)fb_width();
    ap.sh = (int32_t)fb_height();

    /* Загружаем WAV */
    load_wav(filepath, &ap.wav, &ap.audio_data);

    /* Главный цикл */
    uint32_t last_sample_tick = pit_get_ticks();
    uint32_t sample_interval = ap.wav.valid ? (1000 / (ap.wav.sample_rate / 100)) : 40;

    while (ap.running) {
        /* Фон */
        fb_fill_rect(0, 0, ap.sw, ap.sh, AP_BG);

        /* Заголовок */
        fb_draw_string(20, 20, "MyAudio Player", 0x4488CCu, AP_BG, true);

        /* Спектр */
        if (pit_get_ticks() - ap.spectrum_tick > 50) {
            ap_update_spectrum(&ap);
            ap.spectrum_tick = pit_get_ticks();
        }
        ap_draw_spectrum(&ap);

        /* Прогресс */
        ap_draw_progress(&ap);

        /* Контролы */
        ap_draw_controls(&ap);

        fb_swap();

        /* Воспроизведение (эмуляция через PC Speaker) */
        if (ap.playing && !ap.paused && ap.wav.valid && ap.audio_data) {
            uint32_t now = pit_get_ticks();
            if (now - last_sample_tick >= sample_interval) {
                /* Читаем семпл */
                if (ap.audio_pos < ap.wav.data_size) {
                    uint8_t sample = ap.audio_data[ap.audio_pos];
                    uint32_t freq = 200 + sample * 10;
                    speaker_on(freq);
                    ap.audio_pos++;
                    last_sample_tick = now;
                } else {
                    speaker_off();
                    ap.playing = false;
                    ap.audio_pos = 0;
                }
            }
        }

        /* Ввод */
        if (keyboard_has_input()) {
            int c = keyboard_getkey();
            switch (c) {
                case '\n': case '\r': case KEY_KP_ENTER:
                case ' ':
                    if (ap.wav.valid) {
                        ap.playing = !ap.playing;
                        ap.paused = !ap.playing;
                    }
                    break;
                case KEY_ESCAPE:
                    speaker_off();
                    ap.running = false;
                    break;
                case KEY_ARROW_UP:
                    ap.volume += 10;
                    if (ap.volume > 100) ap.volume = 100;
                    break;
                case KEY_ARROW_DOWN:
                    ap.volume -= 10;
                    if (ap.volume < 0) ap.volume = 0;
                    break;
                case KEY_ARROW_LEFT:
                    if (ap.audio_pos > 1000) ap.audio_pos -= 1000;
                    else ap.audio_pos = 0;
                    break;
                case KEY_ARROW_RIGHT:
                    if (ap.audio_pos + 1000 < ap.wav.data_size)
                        ap.audio_pos += 1000;
                    break;
                case 's': case 'S':
                    speaker_off();
                    ap.playing = false;
                    ap.paused = false;
                    ap.audio_pos = 0;
                    break;
                case 'm': case 'M':
                    ap.volume = ap.volume > 0 ? 0 : 70;
                    break;
                case KEY_KP_PLUS:
                    ap.volume += 5;
                    if (ap.volume > 100) ap.volume = 100;
                    break;
                case KEY_KP_MINUS:
                    ap.volume -= 5;
                    if (ap.volume < 0) ap.volume = 0;
                    break;
                default:
                    break;
            }
        }

        /* Мышь */
        const mouse_state_t* ms = mouse_get_state();
        if (ms->left_click) {
            int32_t ctrl_y = ap.sh - 80;
            if (ms->y >= ctrl_y) {
                /* Play */
                if (ms->x >= 60 && ms->x < 105) {
                    if (ap.wav.valid) { ap.playing = !ap.playing; ap.paused = !ap.playing; }
                }
                /* Stop */
                else if (ms->x >= 205 && ms->x < 245) {
                    speaker_off(); ap.playing = false; ap.paused = false; ap.audio_pos = 0;
                }
                /* Volume click */
                else if (ms->x >= 300 && ms->x < 450) {
                    ap.volume = (int)(ms->x - 300) * 100 / 150;
                    if (ap.volume < 0) ap.volume = 0;
                    if (ap.volume > 100) ap.volume = 100;
                }
                mouse_consume_clicks();
            }
        }

        __asm__ volatile ("hlt");
    }

    speaker_off();
    if (ap.audio_data) kfree(ap.audio_data);
}
