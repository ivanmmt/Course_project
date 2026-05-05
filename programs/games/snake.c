/* =============================================================================
 * programs/games/snake.c
 * Игра Snake — классическая змейка
 *
 * Управление: стрелки, Q = выход
 * Особенности: скорость растёт с набором очков, стены убивают
 * ============================================================================= */

#include "games.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/timer/pit.h"
#include "../../lib/string.h"

#define SN_CELL     16          /* размер ячейки в пикселях */
#define SN_COLS     ((int32_t)(fb_width()  / SN_CELL))
#define SN_ROWS     ((int32_t)((fb_height() - 40) / SN_CELL))
#define SN_MAX_LEN  512

/* Цвета */
#define SN_BG       0x0A1A0Au
#define SN_GRID     0x0F2A0Fu
#define SN_HEAD     0x44FF44u
#define SN_BODY     0x228822u
#define SN_FOOD     0xFF4444u
#define SN_WALL     0x334433u
#define SN_TEXT     0xAAFFAAu
#define SN_UI_BG    0x051005u

typedef struct { int32_t x, y; } sn_pt_t;

typedef enum { DIR_RIGHT=0, DIR_DOWN=1, DIR_LEFT=2, DIR_UP=3 } sn_dir_t;

static sn_pt_t  body[SN_MAX_LEN];
static int32_t  body_len;
static sn_dir_t dir, next_dir;
static sn_pt_t  food;
static int32_t  score;
static bool     game_over;
static uint32_t speed_ticks; /* тиков между шагами */

/* Псевдослучайное число */
static uint32_t rng_state = 12345;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void place_food(void) {
    int32_t cols = SN_COLS, rows = SN_ROWS;
    do {
        food.x = (int32_t)(rng_next() % (uint32_t)cols);
        food.y = (int32_t)(rng_next() % (uint32_t)rows);
        bool ok = true;
        for (int32_t i = 0; i < body_len; i++)
            if (body[i].x == food.x && body[i].y == food.y) { ok=false; break; }
        if (ok) break;
    } while (1);
}

static void draw_cell(int32_t gx, int32_t gy, fb_color_t c) {
    int32_t px = gx * SN_CELL;
    int32_t py = gy * SN_CELL + 40; /* 40px UI сверху */
    fb_fill_rect(px+1, py+1, SN_CELL-2, SN_CELL-2, c);
}

static void draw_ui(void) {
    fb_fill_rect(0, 0, (int32_t)fb_width(), 40, SN_UI_BG);

    /* Заголовок */
    fb_draw_string(8, 4, "SNAKE", SN_TEXT, SN_UI_BG, true);
    fb_draw_string(8, 22, "Q=Quit  Arrows=Move", 0x668866u, SN_UI_BG, true);

    /* Очки */
    char sc_buf[32];
    sc_buf[0]='S'; sc_buf[1]='c'; sc_buf[2]='o'; sc_buf[3]='r'; sc_buf[4]='e'; sc_buf[5]=':'; sc_buf[6]=' ';
    int si=7, s=score; char tmp[8]; int ti=0;
    if(s==0){sc_buf[si++]='0';}
    else { while(s>0){tmp[ti++]='0'+s%10;s/=10;} while(ti>0)sc_buf[si++]=tmp[--ti]; }
    sc_buf[si]='\0';
    fb_draw_string((int32_t)fb_width()/2 - 50, 4, sc_buf, 0xFFFF44u, SN_UI_BG, true);

    /* Длина */
    char ln_buf[32];
    ln_buf[0]='L'; ln_buf[1]='e'; ln_buf[2]='n'; ln_buf[3]=':'; ln_buf[4]=' ';
    si=5; int l=body_len; ti=0;
    if(l==0){ln_buf[si++]='0';}
    else { while(l>0){tmp[ti++]='0'+l%10;l/=10;} while(ti>0)ln_buf[si++]=tmp[--ti]; }
    ln_buf[si]='\0';
    fb_draw_string((int32_t)fb_width()/2 + 20, 4, ln_buf, 0x88CCFF, SN_UI_BG, true);
}

static void draw_grid(void) {
    int32_t cols=SN_COLS, rows=SN_ROWS;
    fb_fill_rect(0, 40, (int32_t)fb_width(), (int32_t)fb_height()-40, SN_BG);

    /* Лёгкая сетка */
    for (int32_t x=0; x<=cols; x++)
        fb_draw_vline(x*SN_CELL, 40, rows*SN_CELL, SN_GRID);
    for (int32_t y=0; y<=rows; y++)
        fb_draw_hline(0, 40+y*SN_CELL, cols*SN_CELL, SN_GRID);
}

void snake_run(void) {
    rng_state = pit_get_ticks() ^ 0xABCD1234;

    /* Инициализация */
    int32_t cols=SN_COLS, rows=SN_ROWS;
    body_len  = 3;
    body[0].x = cols/2;   body[0].y = rows/2;
    body[1].x = cols/2-1; body[1].y = rows/2;
    body[2].x = cols/2-2; body[2].y = rows/2;
    dir = DIR_RIGHT; next_dir = DIR_RIGHT;
    score = 0; game_over = false;
    speed_ticks = 8;  /* начальная скорость (меньше = быстрее) */
    place_food();

    uint32_t last_move = pit_get_ticks();

    while (!game_over) {
        /* Ввод */
        while (keyboard_has_input()) {
            int c = keyboard_getkey();
            switch (c) {
                case KEY_ARROW_UP:    if (dir!=DIR_DOWN)  next_dir=DIR_UP;    break;
                case KEY_ARROW_DOWN:  if (dir!=DIR_UP)    next_dir=DIR_DOWN;  break;
                case KEY_ARROW_LEFT:  if (dir!=DIR_RIGHT) next_dir=DIR_LEFT;  break;
                case KEY_ARROW_RIGHT: if (dir!=DIR_LEFT)  next_dir=DIR_RIGHT; break;
                case 'q': case 'Q': game_over=true; break;
            }
        }

        /* Шаг змейки по таймеру */
        uint32_t now = pit_get_ticks();
        if (now - last_move >= speed_ticks) {
            last_move = now;
            dir = next_dir;

            /* Новая голова */
            sn_pt_t new_head = body[0];
            switch (dir) {
                case DIR_RIGHT: new_head.x++; break;
                case DIR_LEFT:  new_head.x--; break;
                case DIR_DOWN:  new_head.y++; break;
                case DIR_UP:    new_head.y--; break;
            }

            /* Столкновение со стеной */
            if (new_head.x < 0 || new_head.x >= cols ||
                new_head.y < 0 || new_head.y >= rows) {
                game_over = true; break;
            }

            /* Столкновение с телом */
            for (int32_t i = 0; i < body_len - 1; i++) {
                if (body[i].x == new_head.x && body[i].y == new_head.y) {
                    game_over = true; break;
                }
            }
            if (game_over) break;

            /* Съели еду? */
            bool ate = (new_head.x == food.x && new_head.y == food.y);
            if (ate) {
                score += 10;
                if (body_len < SN_MAX_LEN) body_len++;
                /* Ускоряем каждые 50 очков */
                if (score % 50 == 0 && speed_ticks > 2) speed_ticks--;
                place_food();
            }

            /* Двигаем тело */
            for (int32_t i = body_len-1; i > 0; i--)
                body[i] = body[i-1];
            body[0] = new_head;
        }

        /* Рисуем */
        draw_grid();
        draw_ui();

        /* Тело */
        for (int32_t i = body_len-1; i > 0; i--)
            draw_cell(body[i].x, body[i].y, SN_BODY);
        /* Голова (ярче) */
        draw_cell(body[0].x, body[0].y, SN_HEAD);
        /* Глаза на голове */
        int32_t hpx = body[0].x*SN_CELL, hpy = body[0].y*SN_CELL+40;
        fb_fill_rect(hpx+SN_CELL-5, hpy+3, 3, 3, FB_BLACK);
        fb_fill_rect(hpx+SN_CELL-5, hpy+SN_CELL-6, 3, 3, FB_BLACK);

        /* Еда (пульсирующая) */
        int32_t pulse = (int32_t)(pit_get_ticks() / 5 % 4);
        fb_fill_circle(food.x*SN_CELL + SN_CELL/2,
                       food.y*SN_CELL + SN_CELL/2 + 40,
                       SN_CELL/2 - 2 + pulse, SN_FOOD);

        fb_swap();
        __asm__ volatile ("hlt");
    }

    /* Game Over экран */
    if (!fb_ready()) return;
    fb_fill_rect(0, 0, (int32_t)fb_width(), (int32_t)fb_height(), 0x110011u);
    fb_draw_string_scaled((int32_t)fb_width()/2-64, (int32_t)fb_height()/2-40,
                          "GAME", 0xFF4444u, 3);
    fb_draw_string_scaled((int32_t)fb_width()/2-64, (int32_t)fb_height()/2,
                          "OVER", 0xFF4444u, 3);

    char sc2[32]; int s2=score, si2=0; char t2[8]; int ti2=0;
    const char* pref="Score: ";
    while(*pref) sc2[si2++]=*pref++;
    if(s2==0){sc2[si2++]='0';}
    else{while(s2>0){t2[ti2++]='0'+s2%10;s2/=10;}while(ti2>0)sc2[si2++]=t2[--ti2];}
    sc2[si2]='\0';
    fb_draw_string((int32_t)fb_width()/2 - 40,
                   (int32_t)fb_height()/2 + 48, sc2, 0xFFFF44u, 0x110011u, true);
    fb_draw_string((int32_t)fb_width()/2 - 60,
                   (int32_t)fb_height()/2 + 70,
                   "Press any key...", 0x888888u, 0x110011u, true);
    fb_swap();

    /* Ждём нажатия */
    while (!keyboard_has_input()) __asm__ volatile ("hlt");
    keyboard_getkey();
}
