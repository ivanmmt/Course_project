/* =============================================================================
 * gui/apps.c — Apps: About, Calculator (integer), System Info, Launchers
 * Калькулятор использует целочисленную арифметику (×1000) без double/float
 * ============================================================================= */
#include "gui.h"
#include "../programs/editor/editor.h"
#include "../programs/games/games.h"
#include "../programs/filemanager/filemanager.h"
#include "../programs/browser/browser.h"
#include "../programs/wordproc/wordproc.h"
#include "../programs/mediaplayer/video/mediaplayer_video.h"
#include "../programs/mediaplayer/audio/mediaplayer_audio.h"
#include "../programs/codeeditor/codeeditor.h"
#include "../drivers/framebuffer/framebuffer.h"
#include "../drivers/timer/pit.h"
#include "../memory/pmm.h"
#include "../lib/string.h"

/* ═══════════════════════════════════════════════════════════════════
 * About Box
 * ═══════════════════════════════════════════════════════════════════ */
static void about_paint(gui_window_t* w) {
    fb_rect_t cr = gui_client_rect(w);
    fb_fill_round_rect(cr.x+10,cr.y+10,60,60,8,0x1E5A8Au);
    fb_draw_string_scaled(cr.x+16,cr.y+18,"My",FB_WHITE,2);
    fb_draw_string_scaled(cr.x+16,cr.y+38,"OS",0x4FC3F7u,2);
    fb_draw_string(cr.x+82,cr.y+14,"MyOS",       0x1A3A6Cu,0,true);
    fb_draw_string(cr.x+82,cr.y+32,"x86 OS",0x666666u,0,true);
    fb_draw_string(cr.x+82,cr.y+50,"C + Assembly",      0x888888u,0,true);
    fb_draw_hline(cr.x+10,cr.y+82,cr.w-20,0xCCCCCCu);
    const char* s[]={"GDT + IDT (Protected Mode)",
                     "PMM + VMM + Heap",
                     "Framebuffer + Mouse + GUI",
                     "VFS + RamFS + ELF loader",
                     "Editor + Games"};
    for(int i=0;i<5;i++)
        fb_draw_string(cr.x+16,cr.y+96+i*18,s[i],0x2266AAu,0,true);
}
static void about_close(void* ud){ gui_close_window((gui_window_t*)ud); }
static gui_window_t* app_about_open(void){
    gui_window_t* w=gui_create_window("About MyOS",180,100,320,300);
    if(!w)return (gui_window_t*)0;
    w->on_paint=about_paint;
    gui_add_button(w,110,252,80,"Close",about_close,w);
    return w;
}

/* ═══════════════════════════════════════════════════════════════════
 * Calculator — целочисленная арифметика с фиксированной точкой ×1000
 * Хранит значения как int32_t в тысячных долях: 1.5 → 1500
 * ═══════════════════════════════════════════════════════════════════ */
#define CALC_SCALE 1000   /* 3 знака после запятой */

typedef struct {
    int32_t accum;      /* накопленное значение ×SCALE */
    char    disp[24];   /* строка для отображения */
    char    op;         /* текущая операция */
    bool    new_num;    /* следующий ввод начинает новое число */
    bool    has_dot;    /* введена ли точка */
    int32_t dot_div;    /* делитель для дробной части (10, 100, 1000) */
    int32_t cur;        /* текущее вводимое значение ×SCALE */
} calc_t;
static calc_t cs;

/* Обновить строку отображения из cs.cur */
static void calc_update_display(void) {
    int32_t v = cs.cur;
    bool neg = (v < 0); if(neg) v = -v;
    int32_t whole = v / CALC_SCALE;
    int32_t frac  = v % CALC_SCALE;

    int i = 0;
    if(neg) cs.disp[i++] = '-';

    /* Целая часть */
    if(whole == 0) cs.disp[i++]='0';
    else {
        char tmp[12]; int ti=0;
        int32_t t=whole;
        while(t>0){tmp[ti++]='0'+t%10;t/=10;}
        while(ti>0) cs.disp[i++]=tmp[--ti];
    }

    /* Дробная часть — только если не нуль */
    if(frac > 0) {
        cs.disp[i++] = '.';
        /* 3 знака */
        char tmp[4];
        tmp[0] = '0' + (frac/100)%10;
        tmp[1] = '0' + (frac/10)%10;
        tmp[2] = '0' + frac%10;
        tmp[3] = '\0';
        /* Убираем хвостовые нули */
        int tl=2;
        while(tl>0 && tmp[tl]=='0') tl--;
        for(int k=0;k<=tl;k++) cs.disp[i++]=tmp[k];
    }
    cs.disp[i] = '\0';
}

static void calc_paint(gui_window_t* w) {
    fb_rect_t cr = gui_client_rect(w);
    fb_fill_rect(cr.x+6,cr.y+6,cr.w-12,28,0x111111u);
    fb_draw_rect(cr.x+6,cr.y+6,cr.w-12,28,0x444444u);
    int32_t tw=(int32_t)strlen(cs.disp)*FB_FONT_W;
    fb_draw_string(cr.x+cr.w-12-tw,cr.y+14,cs.disp,0x00FF66u,0x111111u,true);
}

static void calc_apply_op(void) {
    if(!cs.op) return;
    int32_t v = cs.cur;
    switch(cs.op) {
        case '+': cs.accum += v; break;
        case '-': cs.accum -= v; break;
        case '*': {
            /* a*b/SCALE — используем наши 64-бит функции из divdi3.c */
            int32_t sign = 1;
            int32_t aa = cs.accum, bb = v;
            if(aa < 0){sign=-sign; aa=-aa;}
            if(bb < 0){sign=-sign; bb=-bb;}
            /* Приближение: делаем через uint32_t с ограничением */
            /* aa_whole * bb + aa_frac * bb / SCALE */
            uint32_t aw = (uint32_t)aa / CALC_SCALE;
            uint32_t af = (uint32_t)aa % CALC_SCALE;
            uint32_t bv = (uint32_t)bb;
            uint32_t result = aw * bv + (af * (bv / CALC_SCALE));
            /* Дробная часть произведения */
            uint32_t bfrac = bv % (uint32_t)CALC_SCALE;
            if(bfrac) result += (af * bfrac) / (uint32_t)CALC_SCALE;
            cs.accum = sign < 0 ? -(int32_t)result : (int32_t)result;
            break;
        }
        case '/':
            if(v != 0) {
                /* a/b с масштабом: (accum * SCALE) / v — все uint32_t если числа небольшие */
                int32_t sign = 1;
                int32_t aa = cs.accum, bb = v;
                if(aa < 0){sign=-sign; aa=-aa;}
                if(bb < 0){sign=-sign; bb=-bb;}
                /* (aa * SCALE) / bb — делаем в 32-бит частями */
                uint32_t whole = (uint32_t)aa / (uint32_t)bb;
                uint32_t rem   = (uint32_t)aa % (uint32_t)bb;
                /* дробная часть: rem * SCALE / bb */
                uint32_t frac  = (rem * (uint32_t)CALC_SCALE) / (uint32_t)bb;
                uint32_t res   = whole * (uint32_t)CALC_SCALE + frac;
                cs.accum = sign < 0 ? -(int32_t)res : (int32_t)res;
            } else {
                strcpy(cs.disp,"Err"); cs.op=0; cs.new_num=true; return;
            }
            break;
    }
    cs.cur = cs.accum;
    calc_update_display();
}

static void calc_btn(void* ud) {
    const char* k=(const char*)ud; char c=k[0];

    if(c>='0' && c<='9') {
        if(cs.new_num) {
            cs.cur=0; cs.has_dot=false; cs.dot_div=10; cs.new_num=false;
            cs.disp[0]='0'; cs.disp[1]='\0';
        }
        if(!cs.has_dot) {
            cs.cur = cs.cur * 10 + (c-'0') * CALC_SCALE;
        } else {
            /* дробная часть */
            if(cs.dot_div <= CALC_SCALE) {
                cs.cur += (c-'0') * (CALC_SCALE / cs.dot_div);
                cs.dot_div *= 10;
            }
        }
        calc_update_display();
        if(cs.has_dot) {
            /* восстанавливаем точку в строке */
            int l=strlen(cs.disp);
            if(!strchr(cs.disp,'.')) {
                cs.disp[l]='.'; cs.disp[l+1]='0'; cs.disp[l+2]='\0';
            }
        }
    } else if(c == '.') {
        if(!cs.has_dot) { cs.has_dot=true; cs.dot_div=10;
                           int l=strlen(cs.disp);
                           cs.disp[l]='.'; cs.disp[l+1]='0'; cs.disp[l+2]='\0'; }
    } else if(c == 'C') {
        cs.accum=0; cs.cur=0; cs.op=0; cs.new_num=true;
        cs.has_dot=false; cs.dot_div=10;
        cs.disp[0]='0'; cs.disp[1]='\0';
    } else if(c=='+' || c=='-' || c=='*' || c=='/') {
        if(cs.op && !cs.new_num) calc_apply_op();
        else cs.accum = cs.cur;
        cs.op = c; cs.new_num = true;
    } else if(c == '=') {
        if(cs.op && !cs.new_num) calc_apply_op();
        else { cs.accum=cs.cur; calc_update_display(); }
        cs.op=0; cs.new_num=true;
    }
}

static const char* CK[]={"7","8","9","/","4","5","6","*","1","2","3","-","C","0","=","+"};
static gui_window_t* app_calc_open(void) {
    gui_window_t* w=gui_create_window("Calculator",360,80,216,244);
    if(!w)return (gui_window_t*)0;
    cs.accum=0; cs.cur=0; cs.op=0; cs.new_num=true;
    cs.has_dot=false; cs.dot_div=10;
    cs.disp[0]='0'; cs.disp[1]='\0';
    w->on_paint=calc_paint;
    for(int r=0;r<4;r++) for(int c=0;c<4;c++){
        int idx=r*4+c;
        gui_add_button(w,6+c*50,44+r*32,46,CK[idx],calc_btn,(void*)CK[idx]);
    }
    return w;
}

/* ═══════════════════════════════════════════════════════════════════
 * System Info — всё через uint32_t, без uint64_t деления
 * ═══════════════════════════════════════════════════════════════════ */
static void itoa32(uint32_t n, char* buf) {
    if(!n){ buf[0]='0'; buf[1]='\0'; return; }
    char tmp[12]; int ti=0;
    while(n>0){tmp[ti++]='0'+n%10;n/=10;}
    int i=0; while(ti>0) buf[i++]=tmp[--ti];
    buf[i]='\0';
}

static void sysinfo_paint(gui_window_t* w) {
    fb_rect_t cr = gui_client_rect(w);
    const char* info[]={
        "OS:     MyOS v0.1.0",
        "Arch:   x86 32-bit Protected Mode",
        "GDT:    5 descriptors",
        "IDT:    256 vectors (IRQ@0x20-0x2F)",
        "PIT:    100 Hz system timer",
        "VFS:    RamFS mounted at /",
        "Apps:   Editor, Snake, Tetris, Pong",
    };
    for(int i=0;i<7;i++)
        fb_draw_string(cr.x+8,cr.y+8+i*20,info[i],0x2255AAu,0,true);

    fb_draw_hline(cr.x+8,cr.y+152,cr.w-16,0xCCCCCCu);

    /* RAM — только uint32_t операции */
    uint32_t fkb = pmm_get_free_kb();
    uint32_t tkb = (uint32_t)pmm_get_total_pages() * 4u;
    uint32_t ukb = (tkb > fkb) ? tkb - fkb : 0u;
    int32_t bw   = cr.w - 60;

    fb_draw_string(cr.x+8, cr.y+162, "RAM:", 0x444444u, 0, true);
    fb_fill_rect(cr.x+44, cr.y+164, bw, 12, 0xDDDDDDu);
    /* Вычисляем ширину бара через uint32_t без overflow */
    uint32_t uw32 = tkb ? (ukb / (tkb / (uint32_t)bw + 1u)) : 0u;
    if((int32_t)uw32 > bw) uw32 = (uint32_t)bw;
    fb_fill_rect(cr.x+44, cr.y+164, (int32_t)uw32, 12, 0x2266AAu);
    fb_draw_rect(cr.x+44, cr.y+164, bw, 12, 0x888888u);

    char buf[48]; int bi=0;
    char tmp[12];
    itoa32(fkb, tmp); for(int i=0;tmp[i];i++) buf[bi++]=tmp[i];
    const char* s1=" KB free / "; while(*s1) buf[bi++]=*s1++;
    itoa32(tkb/1024u, tmp); for(int i=0;tmp[i];i++) buf[bi++]=tmp[i];
    const char* s2=" MB"; while(*s2) buf[bi++]=*s2++;
    buf[bi]='\0';
    fb_draw_string(cr.x+8, cr.y+182, buf, 0x228822u, 0, true);

    /* Uptime */
    bi=0;
    const char* s3="Uptime: "; while(*s3) buf[bi++]=*s3++;
    itoa32(pit_get_ticks()/100u, tmp); for(int i=0;tmp[i];i++) buf[bi++]=tmp[i];
    const char* s4=" sec"; while(*s4) buf[bi++]=*s4++;
    buf[bi]='\0';
    fb_draw_string(cr.x+8, cr.y+200, buf, 0x228888u, 0, true);
}
static void sysinfo_close(void* ud){gui_close_window((gui_window_t*)ud);}

/* ═══════════════════════════════════════════════════════════════════
 * Launchers
 * ═══════════════════════════════════════════════════════════════════ */
static void launch_editor (void*ud){(void)ud; editor_run("/tmp/untitled.txt");}
static void launch_snake  (void*ud){(void)ud; snake_run();}
static void launch_tetris (void*ud){(void)ud; tetris_run();}
static void launch_pong   (void*ud){(void)ud; pong_run();}
static void launch_about  (void*ud){(void)ud; app_about_open();}
static void launch_calc   (void*ud){(void)ud; app_calc_open();}
static void launch_fm      (void*ud){(void)ud; filemanager_run("/");}
static void launch_browser (void*ud){(void)ud;
    browser_open("https://google.com");
}
static void launch_word    (void*ud){(void)ud; wordproc_open((char*)0);}
static void launch_video   (void*ud){(void)ud; mediaplayer_video_run("/tmp/image.bmp");}
static void launch_audio   (void*ud){(void)ud; mediaplayer_audio_run("/tmp/audio.wav");}
static void launch_codeed  (void*ud){(void)ud; codeeditor_run("/tmp/untitled.c");}
static void launch_terminal(void*ud){(void)ud; gui_create_terminal_window();}
static void launch_sysinfo(void*ud){
    (void)ud;
    gui_window_t* w=gui_create_window("System Info",80,60,380,254);
    if(!w)return;
    w->on_paint=sysinfo_paint;
    gui_add_button(w,150,212,80,"Close",sysinfo_close,w);
}

/* ═══════════════════════════════════════════════════════════════════
 * Desktop icon grid
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct { const char* name; const char* ltr; fb_color_t c; void(*fn)(void*); } icon_t;
static const icon_t ICONS[]={
    {"Files",   "F", 0xE8A020u, launch_fm      },
    {"Browser", "W", 0x2060C0u, launch_browser },
    {"Word",    "W", 0x2050A0u, launch_word    },
    {"Code",    "<", 0x2A8A4Au, launch_codeed  },
    {"Editor",  "E", 0x2B5EA7u, launch_editor  },
    {"Video",   "V", 0xA02060u, launch_video   },
    {"Audio",   "M", 0x6A2A8Au, launch_audio   },
    {"Term",    ">", 0x1A1A2Au, launch_terminal},
    {"Calc",    "C", 0x2A8A4Au, launch_calc    },
    {"Snake",   "S", 0x8A2A2Au, launch_snake   },
    {"Tetris",  "T", 0x6A2A8Au, launch_tetris  },
    {"Pong",    "P", 0x8A6A2Au, launch_pong    },
    {"Info",    "i", 0x3A6A7Au, launch_sysinfo },
};
#define ICON_N  13
#define ICO_W   72
#define ICO_H   76
#define ICO_GAP 8

void apps_draw_desktop_icons(void) {
    int32_t sx=20, sy=48;
    for(int i=0;i<ICON_N;i++){
        int32_t ix=sx+i*(ICO_W+ICO_GAP), iy=sy;
        fb_fill_round_rect(ix+2,iy+2,ICO_W,ICO_H-14,8,0x00000050u);
        fb_fill_round_rect(ix,iy,ICO_W,ICO_H-14,8,ICONS[i].c);
        /* Светлая грань сверху */
        fb_draw_hline(ix+4,iy+1,ICO_W-8,fb_blend(ICONS[i].c,FB_WHITE,60));
        fb_draw_string_scaled(ix+ICO_W/2-8,iy+(ICO_H-14)/2-16,
                              ICONS[i].ltr,FB_WHITE,2);
        int32_t lw=(int32_t)strlen(ICONS[i].name)*FB_FONT_W;
        /* Тень текста */
        fb_draw_string(ix+(ICO_W-lw)/2+1,iy+ICO_H-11,
                       ICONS[i].name,0x00000080u,0,true);
        fb_draw_string(ix+(ICO_W-lw)/2,iy+ICO_H-12,
                       ICONS[i].name,FB_WHITE,0,true);
    }
}

void apps_handle_icon_click(int32_t mx, int32_t my) {
    int32_t sx=20, sy=48;
    for(int i=0;i<ICON_N;i++){
        int32_t ix=sx+i*(ICO_W+ICO_GAP), iy=sy;
        if(mx>=ix && mx<ix+ICO_W && my>=iy && my<iy+ICO_H){
            if(ICONS[i].fn) ICONS[i].fn((void*)0);
            return;
        }
    }
}

void apps_init(void){
    app_about_open();
    app_calc_open();
}
