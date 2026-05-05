/* =============================================================================
 * programs/games/tetris.c
 * Игра Tetris — классический тетрис
 *
 * Управление:
 *   ← → : движение влево/вправо
 *   ↓   : ускоренное падение
 *   ↑   : поворот
 *   Пробел: мгновенное падение (hard drop)
 *   Q   : выход
 * ============================================================================= */

#include "games.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/timer/pit.h"
#include "../../lib/string.h"

#define TT_COLS     10
#define TT_ROWS     20
#define TT_CELL     28
#define TT_BOARD_X  ((int32_t)(fb_width()/2) - TT_COLS*TT_CELL/2)
#define TT_BOARD_Y  20

/* 7 цветов для 7 тетромино */
static const fb_color_t TT_COLORS[] = {
    0x000000u,  /* 0 = пусто */
    0x00EEEEu,  /* I - голубой */
    0xEEEE00u,  /* O - жёлтый */
    0xAA00EEu,  /* T - фиолетовый */
    0x00EE00u,  /* S - зелёный */
    0xEE0000u,  /* Z - красный */
    0x0000EEu,  /* J - синий */
    0xEE8800u,  /* L - оранжевый */
};

/* Формы тетромино: 7 штук × 4 поворота × 4×4 grid (0/1) */
/* I */ static const uint8_t T_I[4][4][4] = {
    {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
    {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}},
    {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}},
    {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}},
};
/* O */ static const uint8_t T_O[4][4][4] = {
    {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
};
/* T */ static const uint8_t T_T[4][4][4] = {
    {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
    {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}},
    {{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},
};
/* S */ static const uint8_t T_S[4][4][4] = {
    {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
    {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},
    {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
    {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},
};
/* Z */ static const uint8_t T_Z[4][4][4] = {
    {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}},
    {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}},
};
/* J */ static const uint8_t T_J[4][4][4] = {
    {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
    {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}},
    {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}},
};
/* L */ static const uint8_t T_L[4][4][4] = {
    {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
    {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}},
    {{0,0,0,0},{1,1,1,0},{1,0,0,0},{0,0,0,0}},
    {{1,1,0,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
};

/* Таблица всех фигур */
static const uint8_t (*PIECES[7])[4][4] = {
    T_I,T_O,T_T,T_S,T_Z,T_J,T_L
};

static uint8_t board[TT_ROWS][TT_COLS]; /* 0=пусто, 1-7=цвет */
static int32_t piece_type, piece_rot, piece_x, piece_y;
static int32_t next_type;
static int32_t score, lines_total, level;
static bool    running;

static uint32_t rng2 = 99991;
static int32_t rand_piece(void) {
    rng2 = rng2 * 1664525u + 1013904223u;
    return (int32_t)(rng2 % 7);
}

static bool can_place(int32_t type, int32_t rot, int32_t px, int32_t py) {
    const uint8_t (*sh)[4] = PIECES[type][rot];
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) {
        if (!sh[r][c]) continue;
        int nx=px+c, ny=py+r;
        if (nx<0||nx>=TT_COLS||ny>=TT_ROWS) return false;
        if (ny>=0 && board[ny][nx]) return false;
    }
    return true;
}

static void lock_piece(void) {
    const uint8_t (*sh)[4] = PIECES[piece_type][piece_rot];
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) {
        if (!sh[r][c]) continue;
        int ny=piece_y+r, nx=piece_x+c;
        if (ny>=0&&ny<TT_ROWS&&nx>=0&&nx<TT_COLS)
            board[ny][nx] = (uint8_t)(piece_type+1);
    }
}

static int32_t clear_lines(void) {
    int32_t cleared = 0;
    for (int r=TT_ROWS-1; r>=0; r--) {
        bool full=true;
        for (int c=0;c<TT_COLS;c++) if (!board[r][c]){full=false;break;}
        if (full) {
            for (int rr=r;rr>0;rr--)
                for (int c=0;c<TT_COLS;c++) board[rr][c]=board[rr-1][c];
            for (int c=0;c<TT_COLS;c++) board[0][c]=0;
            cleared++; r++; /* проверяем ту же строку снова */
        }
    }
    return cleared;
}

static void spawn_piece(void) {
    piece_type = next_type;
    piece_rot  = 0;
    piece_x    = TT_COLS/2 - 2;
    piece_y    = 0;
    next_type  = rand_piece();
    if (!can_place(piece_type,piece_rot,piece_x,piece_y))
        running = false;
}

static void draw_cell_tt(int32_t gx, int32_t gy, fb_color_t c) {
    int32_t px = TT_BOARD_X + gx*TT_CELL;
    int32_t py = TT_BOARD_Y + gy*TT_CELL;
    fb_fill_rect(px,py,TT_CELL,TT_CELL,c);
    /* Светлая грань сверху-слева */
    fb_draw_hline(px, py, TT_CELL, fb_blend(c, FB_WHITE, 80));
    fb_draw_vline(px, py, TT_CELL, fb_blend(c, FB_WHITE, 80));
    /* Тёмная грань снизу-справа */
    fb_draw_hline(px, py+TT_CELL-1, TT_CELL, fb_blend(c, FB_BLACK, 80));
    fb_draw_vline(px+TT_CELL-1, py, TT_CELL, fb_blend(c, FB_BLACK, 80));
}

static void tt_render(void) {
    fb_fill_rect(0,0,(int32_t)fb_width(),(int32_t)fb_height(),0x101018u);

    /* Фон поля */
    fb_fill_rect(TT_BOARD_X, TT_BOARD_Y, TT_COLS*TT_CELL, TT_ROWS*TT_CELL, 0x0A0A14u);

    /* Сетка */
    for (int r=0;r<=TT_ROWS;r++)
        fb_draw_hline(TT_BOARD_X, TT_BOARD_Y+r*TT_CELL, TT_COLS*TT_CELL, 0x1A1A28u);
    for (int c=0;c<=TT_COLS;c++)
        fb_draw_vline(TT_BOARD_X+c*TT_CELL, TT_BOARD_Y, TT_ROWS*TT_CELL, 0x1A1A28u);

    /* Рамка поля */
    fb_draw_rect(TT_BOARD_X-1, TT_BOARD_Y-1,
                 TT_COLS*TT_CELL+2, TT_ROWS*TT_CELL+2, 0x4444AAu);

    /* Зафиксированные блоки */
    for (int r=0;r<TT_ROWS;r++) for (int c=0;c<TT_COLS;c++) {
        if (board[r][c])
            draw_cell_tt(c, r, TT_COLORS[board[r][c]]);
    }

    /* Текущая фигура */
    const uint8_t (*sh)[4] = PIECES[piece_type][piece_rot];
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) {
        if (sh[r][c] && piece_y+r >= 0)
            draw_cell_tt(piece_x+c, piece_y+r, TT_COLORS[piece_type+1]);
    }

    /* Ghost piece (куда упадёт) */
    int32_t ghost_y = piece_y;
    while (can_place(piece_type,piece_rot,piece_x,ghost_y+1)) ghost_y++;
    if (ghost_y != piece_y) {
        for (int r=0;r<4;r++) for (int c=0;c<4;c++) {
            if (!sh[r][c] || ghost_y+r < 0) continue;
            int px=TT_BOARD_X+(piece_x+c)*TT_CELL;
            int py=TT_BOARD_Y+(ghost_y+r)*TT_CELL;
            fb_draw_rect(px,py,TT_CELL,TT_CELL,fb_blend(0,TT_COLORS[piece_type+1],60));
        }
    }

    /* UI справа */
    int32_t ui_x = TT_BOARD_X + TT_COLS*TT_CELL + 20;
    fb_draw_string(ui_x, TT_BOARD_Y, "NEXT",  0x8888CCu, 0x101018u, true);
    fb_draw_string(ui_x, TT_BOARD_Y+60, "SCORE",0x8888CCu, 0x101018u, true);
    fb_draw_string(ui_x, TT_BOARD_Y+120,"LINES",0x8888CCu, 0x101018u, true);
    fb_draw_string(ui_x, TT_BOARD_Y+180,"LEVEL",0x8888CCu, 0x101018u, true);

    /* Следующая фигура */
    const uint8_t (*nsh)[4] = PIECES[next_type][0];
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) {
        if (!nsh[r][c]) continue;
        fb_fill_rect(ui_x + c*16, TT_BOARD_Y+16 + r*16,
                     14, 14, TT_COLORS[next_type+1]);
    }

    /* Числа */
    char buf[16];
    int bi; char tmp2[12]; int ti2;
    /* score */
    bi=0; ti2=0; int32_t sv=score;
    if(!sv){buf[bi++]='0';}
    else{while(sv>0){tmp2[ti2++]='0'+sv%10;sv/=10;}while(ti2>0)buf[bi++]=tmp2[--ti2];}
    buf[bi]='\0';
    fb_draw_string(ui_x, TT_BOARD_Y+76, buf, 0xFFFF88u, 0x101018u, true);
    /* lines */
    bi=0; ti2=0; sv=lines_total;
    if(!sv){buf[bi++]='0';}
    else{while(sv>0){tmp2[ti2++]='0'+sv%10;sv/=10;}while(ti2>0)buf[bi++]=tmp2[--ti2];}
    buf[bi]='\0';
    fb_draw_string(ui_x, TT_BOARD_Y+136, buf, 0xFFFF88u, 0x101018u, true);
    /* level */
    bi=0; ti2=0; sv=level;
    if(!sv){buf[bi++]='0';}
    else{while(sv>0){tmp2[ti2++]='0'+sv%10;sv/=10;}while(ti2>0)buf[bi++]=tmp2[--ti2];}
    buf[bi]='\0';
    fb_draw_string(ui_x, TT_BOARD_Y+196, buf, 0xFFFF88u, 0x101018u, true);

    fb_draw_string(ui_x, TT_BOARD_Y+260, "UP:rotate",  0x666688u, 0x101018u, true);
    fb_draw_string(ui_x, TT_BOARD_Y+278, "SPC:drop",   0x666688u, 0x101018u, true);
    fb_draw_string(ui_x, TT_BOARD_Y+296, "Q:quit",     0x666688u, 0x101018u, true);

    fb_swap();
}

void tetris_run(void) {
    rng2 = pit_get_ticks() ^ 0xDEAD;
    memset(board, 0, sizeof(board));
    score=0; lines_total=0; level=1; running=true;
    next_type = rand_piece();
    spawn_piece();

    uint32_t last_fall = pit_get_ticks();
    uint32_t fall_interval = 50; /* тиков между автопадением */

    while (running) {
        /* Ввод */
        while (keyboard_has_input()) {
            int c = keyboard_getkey();
            switch(c) {
                case KEY_ARROW_LEFT:
                    if (can_place(piece_type,piece_rot,piece_x-1,piece_y))
                        piece_x--;
                    break;
                case KEY_ARROW_RIGHT:
                    if (can_place(piece_type,piece_rot,piece_x+1,piece_y))
                        piece_x++;
                    break;
                case KEY_ARROW_DOWN:
                    if (can_place(piece_type,piece_rot,piece_x,piece_y+1))
                        piece_y++;
                    break;
                case KEY_ARROW_UP: {
                    int32_t nr = (piece_rot+1)%4;
                    if (can_place(piece_type,nr,piece_x,piece_y)) piece_rot=nr;
                    break;
                }
                case ' ': /* Hard drop */
                    while (can_place(piece_type,piece_rot,piece_x,piece_y+1))
                        piece_y++;
                    /* fallthrough → фиксируем */
                    lock_piece();
                    {int32_t cl=clear_lines();
                     if(cl>0){
                         static const int32_t pts[]={0,100,300,500,800};
                         score+=pts[cl]*level;
                         lines_total+=cl;
                         level=lines_total/10+1;
                         fall_interval=(uint32_t)(50-level*4);
                         if(fall_interval<4) fall_interval=4;
                     }}
                    spawn_piece();
                    break;
                case 'q': case 'Q': running=false; break;
            }
        }

        /* Автопадение */
        uint32_t now = pit_get_ticks();
        if (now - last_fall >= fall_interval) {
            last_fall = now;
            if (can_place(piece_type,piece_rot,piece_x,piece_y+1)) {
                piece_y++;
            } else {
                lock_piece();
                int32_t cl = clear_lines();
                if (cl > 0) {
                    static const int32_t pts[]={0,100,300,500,800};
                    score  += pts[cl]*level;
                    lines_total += cl;
                    level   = lines_total/10+1;
                    fall_interval = (uint32_t)(50 - level*4);
                    if (fall_interval<4) fall_interval=4;
                }
                spawn_piece();
            }
        }

        tt_render();
        __asm__ volatile ("hlt");
    }

    /* Game Over */
    fb_fill_rect(TT_BOARD_X, TT_BOARD_Y+TT_ROWS*TT_CELL/2-24,
                 TT_COLS*TT_CELL, 48, 0x200020u);
    fb_draw_string(TT_BOARD_X+20, TT_BOARD_Y+TT_ROWS*TT_CELL/2-16,
                   "GAME OVER", 0xFF4444u, 0x200020u, true);
    fb_draw_string(TT_BOARD_X+10, TT_BOARD_Y+TT_ROWS*TT_CELL/2+4,
                   "Press any key", 0x888888u, 0x200020u, true);
    fb_swap();
    while (!keyboard_has_input()) __asm__ volatile ("hlt");
    keyboard_getkey();
}
