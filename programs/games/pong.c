/* =============================================================================
 * programs/games/pong.c
 * Игра Pong — два игрока или vs CPU
 *
 * Управление:
 *   W/S   : Player 1 (левая ракетка)
 *   ↑/↓   : Player 2 или CPU
 *   Q     : выход
 * ============================================================================= */

#include "games.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/timer/pit.h"
#include "../../lib/string.h"

#define PG_W     ((int32_t)fb_width())
#define PG_H     ((int32_t)fb_height())
#define PAD_W    12
#define PAD_H    80
#define PAD_SPD  5
#define BALL_SZ  12
#define WIN_SCORE 7

#define PG_BG    0x050510u
#define PG_FG    0xFFFFFFu
#define PG_P1    0x44AAFFu
#define PG_P2    0xFF6644u
#define PG_BALL  0xFFFFAAu
#define PG_NET   0x333344u

typedef struct {
    int32_t x, y;      /* позиция ракетки */
    int32_t score;
    bool    up, down;  /* зажатые клавиши */
} paddle_t;

typedef struct {
    int32_t x, y;
    int32_t vx, vy;   /* скорость в пикселях/тик */
} ball_t;

static paddle_t p1, p2;
static ball_t   ball;
static bool     running;
static bool     cpu_mode;  /* true = правый игрок — CPU */

static uint32_t rng3 = 77777;
static int32_t rng3_sign(void) {
    /* 32-битный xorshift — никакого 64-битного умножения */
    rng3 ^= rng3 << 13;
    rng3 ^= rng3 >> 17;
    rng3 ^= rng3 << 5;
    return (rng3 & 1) ? 1 : -1;
}

static void reset_ball(int32_t dir) {
    ball.x  = PG_W / 2 - BALL_SZ/2;
    ball.y  = PG_H / 2 - BALL_SZ/2;
    ball.vx = 4 * dir;

    rng3 ^= pit_get_ticks();
    ball.vy = rng3_sign() * (2 + (int32_t)(rng3 % 3));
}

static void draw_int(int32_t x, int32_t y, int32_t n, fb_color_t c, fb_color_t bg) {
    char buf[12]; int i=0;
    if (n == 0) { buf[i++]='0'; }
    else { char t[8]; int ti=0;
           while(n>0){t[ti++]='0'+n%10;n/=10;}
           while(ti>0) buf[i++]=t[--ti]; }
    buf[i]='\0';
    fb_draw_string_scaled(x, y, buf, c, 2);
    (void)bg;
}

static void pong_render(void) {
    fb_fill_rect(0, 0, PG_W, PG_H, PG_BG);

    /* Центральная линия */
    for (int32_t y=0; y<PG_H; y+=20)
        fb_fill_rect(PG_W/2-1, y, 2, 10, PG_NET);

    /* Счёт */
    draw_int(PG_W/4-8, 20, p1.score, PG_P1, PG_BG);
    draw_int(3*PG_W/4-8, 20, p2.score, PG_P2, PG_BG);

    /* CPU метка */
    if (cpu_mode)
        fb_draw_string(3*PG_W/4-16, 60, "CPU", 0x886644u, PG_BG, true);

    /* Ракетки */
    fb_fill_round_rect(p1.x, p1.y, PAD_W, PAD_H, 4, PG_P1);
    fb_fill_round_rect(p2.x, p2.y, PAD_W, PAD_H, 4, PG_P2);

    /* Мяч с ореолом */
    fb_fill_circle(ball.x + BALL_SZ/2, ball.y + BALL_SZ/2,
                   BALL_SZ/2 + 2, fb_blend(PG_BG, PG_BALL, 60));
    fb_fill_circle(ball.x + BALL_SZ/2, ball.y + BALL_SZ/2,
                   BALL_SZ/2, PG_BALL);

    /* Подсказки */
    fb_draw_string(4, PG_H-18, "W/S: P1  |  Arrows: P2  |  Q: Quit",
                   0x444466u, PG_BG, true);

    fb_swap();
}

void pong_run(void) {
    running  = true;
    cpu_mode = true;  /* правый игрок — CPU */

    p1.x=12;        p1.y=PG_H/2-PAD_H/2; p1.score=0;
    p2.x=PG_W-24;   p2.y=PG_H/2-PAD_H/2; p2.score=0;
    reset_ball(1);

    uint32_t last_update = pit_get_ticks();

    while (running) {
        /* Ввод */
        p1.up=false; p1.down=false; p2.up=false; p2.down=false;
        while (keyboard_has_input()) {
            int c = keyboard_getkey();
            switch(c) {
                case 'w': case 'W': p1.up   = true; break;
                case 's': case 'S': p1.down = true; break;
                case KEY_ARROW_UP:   p2.up   = true; cpu_mode=false; break;
                case KEY_ARROW_DOWN: p2.down = true; cpu_mode=false; break;
                case 'q': case 'Q': running=false; break;
            }
        }

        uint32_t now = pit_get_ticks();
        if (now - last_update < 1) {
            __asm__ volatile ("hlt");
            continue;
        }
        last_update = now;

        /* Движение ракеток */
        if (p1.up   && p1.y > 0)      p1.y -= PAD_SPD;
        if (p1.down && p1.y < PG_H-PAD_H) p1.y += PAD_SPD;

        if (!cpu_mode) {
            if (p2.up   && p2.y > 0)       p2.y -= PAD_SPD;
            if (p2.down && p2.y < PG_H-PAD_H) p2.y += PAD_SPD;
        } else {
            /* CPU: простое отслеживание мяча с задержкой */
            int32_t cpu_center = p2.y + PAD_H/2;
            int32_t ball_center= ball.y + BALL_SZ/2;
            if (cpu_center < ball_center - 4 && p2.y < PG_H-PAD_H)
                p2.y += 4;
            else if (cpu_center > ball_center + 4 && p2.y > 0)
                p2.y -= 4;
        }

        /* Движение мяча */
        ball.x += ball.vx;
        ball.y += ball.vy;

        /* Отражение от верхней/нижней стены */
        if (ball.y <= 0)           { ball.y=0; ball.vy=-ball.vy; }
        if (ball.y >= PG_H-BALL_SZ){ ball.y=PG_H-BALL_SZ; ball.vy=-ball.vy; }

        /* Столкновение с ракеткой P1 */
        if (ball.x <= p1.x+PAD_W && ball.x >= p1.x &&
            ball.y+BALL_SZ >= p1.y && ball.y <= p1.y+PAD_H) {
            ball.x = p1.x + PAD_W;
            ball.vx = -ball.vx;
            /* Угол зависит от точки попадания */
            int32_t rel = (ball.y+BALL_SZ/2) - (p1.y+PAD_H/2);
            ball.vy = rel / 8;
            if (ball.vy == 0) ball.vy = 1;
            /* Ускоряем со временем */
            if (ball.vx > 0 && ball.vx < 10) ball.vx++;
            if (ball.vx < 0 && ball.vx > -10) ball.vx--;
        }

        /* Столкновение с ракеткой P2 */
        if (ball.x+BALL_SZ >= p2.x && ball.x <= p2.x+PAD_W &&
            ball.y+BALL_SZ >= p2.y && ball.y <= p2.y+PAD_H) {
            ball.x = p2.x - BALL_SZ;
            ball.vx = -ball.vx;
            int32_t rel = (ball.y+BALL_SZ/2) - (p2.y+PAD_H/2);
            ball.vy = rel / 8;
            if (ball.vy == 0) ball.vy = -1;
            if (ball.vx > 0 && ball.vx < 10) ball.vx++;
            if (ball.vx < 0 && ball.vx > -10) ball.vx--;
        }

        /* Гол */
        if (ball.x < -BALL_SZ) {
            p2.score++;
            if (p2.score >= WIN_SCORE) break;
            reset_ball(1);
        }
        if (ball.x > PG_W) {
            p1.score++;
            if (p1.score >= WIN_SCORE) break;
            reset_ball(-1);
        }

        pong_render();
    }

    /* Победитель */
    fb_fill_rect(PG_W/2-120, PG_H/2-40, 240, 80, 0x101030u);
    fb_draw_rect(PG_W/2-120, PG_H/2-40, 240, 80, 0x4444AAu);
    if (p1.score >= WIN_SCORE)
        fb_draw_string(PG_W/2-80, PG_H/2-20, "PLAYER 1 WINS!", PG_P1, 0x101030u, true);
    else if (p2.score >= WIN_SCORE) {
        const char* winner = cpu_mode ? "CPU WINS!" : "PLAYER 2 WINS!";
        fb_draw_string(PG_W/2-80, PG_H/2-20, winner, PG_P2, 0x101030u, true);
    } else {
        fb_draw_string(PG_W/2-30, PG_H/2-20, "QUIT", PG_FG, 0x101030u, true);
    }
    fb_draw_string(PG_W/2-60, PG_H/2+10, "Press any key", 0x888888u, 0x101030u, true);
    fb_swap();
    while (!keyboard_has_input()) __asm__ volatile ("hlt");
    keyboard_getkey();
}
