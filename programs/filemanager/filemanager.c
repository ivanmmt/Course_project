/* =============================================================================
 * programs/filemanager/filemanager.c
 * Файловый менеджер — проводник в стиле Windows Explorer
 *
 * Возможности:
 *   - Двухпанельный вид (дерево слева, содержимое справа)
 *   - Навигация мышью и клавиатурой
 *   - Иконки типов файлов
 *   - Открытие файлов (ELF, PE, текст, изображения)
 *   - Контекстное меню (правая кнопка)
 *   - Статусная строка с размером и датой
 *   - Копирование пути в clipboard
 * ============================================================================= */

#include "filemanager.h"
#include "../../gui/gui.h"
#include "../../filesystem/vfs.h"
#include "../../filesystem/elf.h"
#include "../../filesystem/pe/pe.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/mouse/mouse.h"
#include "../../drivers/timer/pit.h"
#include "../../kernel/process/process.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"
#include "../../programs/codeeditor/codeeditor.h"

/* ─── Константы UI ─── */
#define FM_TITLE_H      28    /* заголовок окна */
#define FM_TOOLBAR_H    30    /* панель инструментов */
#define FM_STATUSBAR_H  22    /* статус-бар внизу */
#define FM_TREE_W       200   /* ширина панели дерева */
#define FM_ITEM_H       22    /* высота одного элемента списка */
#define FM_ICON_W       18    /* ширина иконки */
#define FM_MAX_ITEMS    256   /* максимум файлов в директории */
#define FM_PATH_MAX     512

/* ─── Цвета темы (Windows-like) ─── */
#define FM_BG           0xF0F0F0u   /* фон */
#define FM_SELECTED_BG  0x0078D7u   /* выбранный элемент */
#define FM_SELECTED_FG  0xFFFFFFu
#define FM_HOVER_BG     0xCCE4F7u   /* наведённый */
#define FM_ITEM_FG      0x000000u
#define FM_DIM_FG       0x808080u
#define FM_HEADER_BG    0xE8E8E8u
#define FM_HEADER_FG    0x303030u
#define FM_BORDER       0xAAAAAAAAu
#define FM_TOOLBAR_BG   0xF5F5F5u
#define FM_TREE_BG      0xFAFAFAu
#define FM_TREE_SEL     0xCCE8FFu
#define FM_STATUS_BG    0xE8E8E8u
#define FM_STATUS_FG    0x444444u
#define FM_TITLEBAR     0x0050A0u
#define FM_TITLEBAR_FG  0xFFFFFFu

/* Иконки типов файлов (ASCII символы) */
#define ICON_FOLDER   "D"   /* каталог */
#define ICON_EXE      "E"   /* исполняемый */
#define ICON_TXT      "T"   /* текст */
#define ICON_IMG      "I"   /* изображение */
#define ICON_ASM      "A"   /* asm/бинарник */
#define ICON_FILE     "F"   /* обычный файл */
#define ICON_DRIVE    "H"   /* диск/устройство */
#define ICON_UP       "^"   /* папка вверх */

/* Цвета иконок */
#define ICON_COLOR_FOLDER  0xF0C040u
#define ICON_COLOR_EXE     0x40A040u
#define ICON_COLOR_TXT     0x6090C0u
#define ICON_COLOR_IMG     0xC06090u
#define ICON_COLOR_SYS     0x8080C0u
#define ICON_COLOR_FILE    0x909090u

/* ─── Запись файла ─── */
typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t size;
    bool     is_dir;
    bool     is_exe;   /* ELF или PE */
    bool     is_pe;    /* Windows PE */
    bool     is_text;
    bool     is_doc;
    bool     is_code;
    bool     is_image;
} fm_entry_t;

/* ─── Состояние файлового менеджера ─── */
typedef struct {
    /* Текущий путь */
    char     current_path[FM_PATH_MAX];

    /* Список файлов */
    fm_entry_t entries[FM_MAX_ITEMS];
    int        entry_count;
    int        selected;     /* выбранный элемент */
    int        scroll_y;     /* прокрутка */
    int        hover;        /* наведённый мышью */

    /* Дерево (левая панель) */
    char     tree_roots[8][64];
    int      tree_count;
    int      tree_selected;
    int      tree_scroll;

    /* Размеры областей */
    int32_t  win_x, win_y, win_w, win_h;
    int32_t  tree_x, tree_y, tree_w, tree_h;  /* панель дерева */
    int32_t  list_x, list_y, list_w, list_h;  /* панель списка */

    /* Состояние UI */
    bool     running;
    bool     show_context_menu;
    int32_t  ctx_x, ctx_y;   /* позиция контекстного меню */

    /* Поиск/фильтр */
    char     filter[64];
    bool     filter_active;

    /* Статус */
    char     status_text[128];

    /* Clipboard */
    char     clipboard_path[FM_PATH_MAX];

} fm_state_t;

static fm_state_t fm;

/* =============================================================================
 * Определение типа файла по имени
 * ============================================================================= */
static void fm_detect_type(fm_entry_t* e) {
    const char* n = e->name;
    int len = strlen(n);
    if (len < 2) return;

    const char* ext = (char*)0;
    for (int i = len-1; i >= 0; i--) {
        if (n[i] == '.') { ext = n + i + 1; break; }
    }
    if (!ext) return;

    /* Исполняемые */
    if (strcmp(ext,"elf")==0 || strcmp(ext,"ELF")==0) { e->is_exe=true; }
    else if (strcmp(ext,"exe")==0 || strcmp(ext,"EXE")==0) { e->is_exe=true; e->is_pe=true; }
    /* Текст и код */
    else if (strcmp(ext,"txt")==0  || strcmp(ext,"TXT")==0 ||
             strcmp(ext,"md")==0   || strcmp(ext,"MD")==0 ||
             strcmp(ext,"log")==0  || strcmp(ext,"LOG")==0 ||
             strcmp(ext,"ini")==0  || strcmp(ext,"INI")==0 ||
             strcmp(ext,"csv")==0  || strcmp(ext,"CSV")==0 ||
             strcmp(ext,"xml")==0  || strcmp(ext,"XML")==0 ||
             strcmp(ext,"json")==0 || strcmp(ext,"JSON")==0) { e->is_text=true; }
    /* Языки C-семейства */
    else if (strcmp(ext,"c")==0    || strcmp(ext,"C")==0 ||
             strcmp(ext,"h")==0    || strcmp(ext,"H")==0 ||
             strcmp(ext,"cpp")==0  || strcmp(ext,"CPP")==0 ||
             strcmp(ext,"hpp")==0  || strcmp(ext,"HPP")==0 ||
             strcmp(ext,"cc")==0   || strcmp(ext,"CC")==0 ||
             strcmp(ext,"cxx")==0  || strcmp(ext,"CXX")==0 ||
             strcmp(ext,"cs")==0   || strcmp(ext,"CS")==0) { e->is_code=true; }
    /* Скриптовые языки */
    else if (strcmp(ext,"py")==0   || strcmp(ext,"PY")==0 ||
             strcmp(ext,"js")==0   || strcmp(ext,"JS")==0 ||
             strcmp(ext,"ts")==0   || strcmp(ext,"TS")==0 ||
             strcmp(ext,"java")==0 || strcmp(ext,"JAVA")==0 ||
             strcmp(ext,"rb")==0   || strcmp(ext,"RB")==0 ||
             strcmp(ext,"php")==0  || strcmp(ext,"PHP")==0 ||
             strcmp(ext,"sh")==0   || strcmp(ext,"SH")==0 ||
             strcmp(ext,"bash")==0 || strcmp(ext,"BASH")==0 ||
             strcmp(ext,"bat")==0  || strcmp(ext,"BAT")==0 ||
             strcmp(ext,"ps1")==0  || strcmp(ext,"PS1")==0) { e->is_code=true; }
    /* Ассемблер */
    else if (strcmp(ext,"asm")==0  || strcmp(ext,"ASM")==0 ||
             strcmp(ext,"s")==0    || strcmp(ext,"S")==0) { e->is_code=true; }
    /* Конфиги и разметка */
    else if (strcmp(ext,"cfg")==0  || strcmp(ext,"CFG")==0 ||
             strcmp(ext,"conf")==0 || strcmp(ext,"CONF")==0 ||
             strcmp(ext,"yml")==0  || strcmp(ext,"YML")==0 ||
             strcmp(ext,"yaml")==0 || strcmp(ext,"YAML")==0 ||
             strcmp(ext,"toml")==0 || strcmp(ext,"TOML")==0) { e->is_text=true; }
    /* Изображения */
    else if (strcmp(ext,"bmp")==0  || strcmp(ext,"BMP")==0 ||
             strcmp(ext,"png")==0  || strcmp(ext,"PNG")==0 ||
             strcmp(ext,"jpg")==0  || strcmp(ext,"JPG")==0 ||
             strcmp(ext,"jpeg")==0 || strcmp(ext,"JPEG")==0 ||
             strcmp(ext,"gif")==0  || strcmp(ext,"GIF")==0 ||
             strcmp(ext,"ico")==0  || strcmp(ext,"ICO")==0 ||
             strcmp(ext,"tga")==0  || strcmp(ext,"TGA")==0) { e->is_image=true; }
    /* Видео */
    else if (strcmp(ext,"mp4")==0  || strcmp(ext,"MP4")==0 ||
             strcmp(ext,"avi")==0  || strcmp(ext,"AVI")==0 ||
             strcmp(ext,"mkv")==0  || strcmp(ext,"MKV")==0 ||
             strcmp(ext,"mov")==0  || strcmp(ext,"MOV")==0 ||
             strcmp(ext,"wmv")==0  || strcmp(ext,"WMV")==0 ||
             strcmp(ext,"flv")==0  || strcmp(ext,"FLV")==0 ||
             strcmp(ext,"webm")==0 || strcmp(ext,"WEBM")==0) { e->is_image=true; }
    /* Аудио */
    else if (strcmp(ext,"mp3")==0  || strcmp(ext,"MP3")==0 ||
             strcmp(ext,"wav")==0  || strcmp(ext,"WAV")==0 ||
             strcmp(ext,"ogg")==0  || strcmp(ext,"OGG")==0 ||
             strcmp(ext,"flac")==0 || strcmp(ext,"FLAC")==0 ||
             strcmp(ext,"aac")==0  || strcmp(ext,"AAC")==0 ||
             strcmp(ext,"wma")==0  || strcmp(ext,"WMA")==0 ||
             strcmp(ext,"midi")==0 || strcmp(ext,"MIDI")==0 ||
             strcmp(ext,"mid")==0  || strcmp(ext,"MID")==0) { e->is_image=true; }
    /* Документы */
    else if (strcmp(ext,"doc")==0  || strcmp(ext,"DOC")==0 ||
             strcmp(ext,"docx")==0 || strcmp(ext,"DOCX")==0 ||
             strcmp(ext,"rtf")==0  || strcmp(ext,"RTF")==0 ||
             strcmp(ext,"odt")==0  || strcmp(ext,"ODT")==0 ||
             strcmp(ext,"pdf")==0  || strcmp(ext,"PDF")==0) { e->is_doc=true; }
    /* Архивы */
    else if (strcmp(ext,"zip")==0  || strcmp(ext,"ZIP")==0 ||
             strcmp(ext,"rar")==0  || strcmp(ext,"RAR")==0 ||
             strcmp(ext,"7z")==0   || strcmp(ext,"7Z")==0 ||
             strcmp(ext,"tar")==0  || strcmp(ext,"TAR")==0 ||
             strcmp(ext,"gz")==0   || strcmp(ext,"GZ")==0) { /* ничего */ }
}

/* =============================================================================
 * Загрузить содержимое директории
 * ============================================================================= */
static void fm_load_dir(const char* path) {
    fm.entry_count = 0;
    fm.selected    = 0;
    fm.scroll_y    = 0;
    fm.hover       = -1;

    strncpy(fm.current_path, path, FM_PATH_MAX-1);

    vfs_node_t* dir = vfs_resolve(path);
    if (!dir || dir->type != VFS_DIRECTORY) {
        strcpy(fm.status_text, "Error: cannot open ");
        strncat(fm.status_text, path, 64);
        return;
    }

    /* Добавляем ".." если не корень */
    if (strcmp(path, "/") != 0) {
        fm_entry_t* e = &fm.entries[fm.entry_count++];
        strcpy(e->name, "..");
        e->is_dir = true;
        e->size   = 0;
    }

    /* Перечисляем содержимое */
    for (uint32_t i = 0;
         i < dir->child_count && fm.entry_count < FM_MAX_ITEMS;
         i++) {
        vfs_node_t* child = dir->children[i];
        if (!child) continue;

        fm_entry_t* e = &fm.entries[fm.entry_count++];
        strncpy(e->name, child->name, VFS_NAME_MAX-1);
        e->size   = child->size;
        e->is_dir = (child->type == VFS_DIRECTORY);
        e->is_exe = false;
        e->is_pe  = false;
        e->is_text= false;
        e->is_code= false;
        e->is_image=false;

        if (!e->is_dir) fm_detect_type(e);
    }

    /* Обновляем статус */
    {
        char* s = fm.status_text;
        int dirs=0, files=0;
        for (int i=0; i<fm.entry_count; i++) {
            if (fm.entries[i].is_dir) dirs++;
            else files++;
        }
        strcpy(s, path);
        strcat(s, "   ");
        /* добавляем цифры */
        char tmp[8]; int ti=0, v=files;
        if(!v){tmp[ti++]='0';}
        else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
        tmp[ti]='\0';
        strcat(s, tmp);
        strcat(s, " files, ");
        ti=0; v=dirs;
        if(!v){tmp[ti++]='0';}
        else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
        tmp[ti]='\0';
        strcat(s, tmp);
        strcat(s, " folders");
    }
}

/* =============================================================================
 * Получить иконку и цвет для элемента
 * ============================================================================= */
static const char* fm_icon(const fm_entry_t* e, fb_color_t* color) {
    if (strcmp(e->name, "..") == 0) { *color = ICON_COLOR_FOLDER; return ICON_UP; }
    if (e->is_dir)   { *color = ICON_COLOR_FOLDER; return ICON_FOLDER; }
    if (e->is_pe)    { *color = 0x4080FFu;          return ICON_EXE; }
    if (e->is_exe)   { *color = ICON_COLOR_EXE;     return ICON_EXE; }
    if (e->is_code)  { *color = ICON_COLOR_TXT;     return ICON_TXT; }
    if (e->is_text)  { *color = ICON_COLOR_TXT;     return ICON_TXT; }
    if (e->is_image) { *color = ICON_COLOR_IMG;     return ICON_IMG; }
    *color = ICON_COLOR_FILE; return ICON_FILE;
}

/* =============================================================================
 * Форматировать размер файла: 1.2 KB, 3.4 MB
 * ============================================================================= */
static void fm_format_size(uint32_t size, char* buf) {
    if (size == 0) { strcpy(buf, "—"); return; }
    if (size < 1024) {
        char t[8]; int i=0,v=(int)size;
        if(!v){t[i++]='0';}else{char tt[8];int j=0;while(v>0){tt[j++]='0'+v%10;v/=10;}while(j>0)t[i++]=tt[--j];}
        t[i]='\0'; strcpy(buf,t); strcat(buf," B");
    } else if (size < 1024*1024) {
        uint32_t kb = size / 1024;
        uint32_t frac = (size % 1024) * 10 / 1024;
        char t[8]; int i=0,v=(int)kb;
        if(!v){t[i++]='0';}else{char tt[8];int j=0;while(v>0){tt[j++]='0'+v%10;v/=10;}while(j>0)t[i++]=tt[--j];}
        t[i++]='.'; t[i++]='0'+(char)frac; t[i++]='K'; t[i++]='B'; t[i]='\0';
        strcpy(buf, t);
    } else {
        uint32_t mb = size / (1024*1024);
        uint32_t frac = (size % (1024*1024)) * 10 / (1024*1024);
        char t[8]; int i=0,v=(int)mb;
        if(!v){t[i++]='0';}else{char tt[8];int j=0;while(v>0){tt[j++]='0'+v%10;v/=10;}while(j>0)t[i++]=tt[--j];}
        t[i++]='.'; t[i++]='0'+(char)frac; t[i++]='M'; t[i++]='B'; t[i]='\0';
        strcpy(buf, t);
    }
}

/* =============================================================================
 * Рисование левой панели (дерево)
 * ============================================================================= */
static void fm_draw_tree(void) {
    int32_t x=fm.tree_x, y=fm.tree_y, w=fm.tree_w, h=fm.tree_h;

    /* Фон */
    fb_fill_rect(x, y, w, h, FM_TREE_BG);
    fb_draw_vline(x+w-1, y, h, FM_BORDER);

    /* Заголовок */
    fb_fill_rect(x, y, w, 22, FM_HEADER_BG);
    fb_draw_hline(x, y+22, w, FM_BORDER);
    fb_draw_string(x+6, y+4, "Folders", FM_HEADER_FG, FM_HEADER_BG, true);

    int32_t iy = y + 26;

    /* Корневые точки */
    const char* roots[] = {"/", "/mnt", "/tmp", "/etc", "/programs"};
    int nroots = 5;

    for (int i=0; i<nroots && iy < y+h-4; i++) {
        bool sel = (strcmp(fm.current_path, roots[i])==0 ||
                    (i==0 && strncmp(fm.current_path,"/",1)==0 &&
                     strcmp(fm.current_path,"/mnt")!=0));

        fb_color_t bg = sel ? FM_TREE_SEL : FM_TREE_BG;
        if (sel) fb_fill_rect(x, iy, w-1, FM_ITEM_H, FM_TREE_SEL);

        /* Иконка */
        fb_fill_round_rect(x+6, iy+4, 14, 14, 3, ICON_COLOR_FOLDER);
        fb_draw_string(x+9, iy+5, i==0?"R":"D", FM_BG, ICON_COLOR_FOLDER, true);

        /* Имя */
        fb_draw_string(x+24, iy+(FM_ITEM_H-FB_FONT_H)/2,
                       roots[i], FM_ITEM_FG, bg, true);

        /* Вертикальная линия дерева */
        if (i < nroots-1)
            fb_draw_vline(x+12, iy+FM_ITEM_H, 2, FM_BORDER);

        iy += FM_ITEM_H;
    }

    /* /mnt/disk0 если есть */
    if (vfs_exists("/mnt/disk0")) {
        bool sel = (strncmp(fm.current_path, "/mnt/disk0", 10)==0);
        fb_color_t bg = sel ? FM_TREE_SEL : FM_TREE_BG;
        if (sel) fb_fill_rect(x, iy, w-1, FM_ITEM_H, FM_TREE_SEL);
        fb_fill_round_rect(x+20, iy+4, 14, 14, 3, 0x5090C0u);
        fb_draw_string(x+23, iy+5, ICON_DRIVE, FM_BG, 0x5090C0u, true);
        fb_draw_string(x+38, iy+(FM_ITEM_H-FB_FONT_H)/2,
                       "/mnt/disk0", FM_ITEM_FG, bg, true);
        iy += FM_ITEM_H;
    }
}

/* =============================================================================
 * Рисование правой панели (список файлов)
 * ============================================================================= */
static void fm_draw_list(void) {
    int32_t x=fm.list_x, y=fm.list_y, w=fm.list_w, h=fm.list_h;

    /* Фон */
    fb_fill_rect(x, y, w, h, FM_BG);

    /* Заголовок колонок */
    fb_fill_rect(x, y, w, 22, FM_HEADER_BG);
    fb_draw_hline(x, y+22, w, FM_BORDER);
    fb_draw_string(x+FM_ICON_W+4, y+4,  "Name",  FM_HEADER_FG, FM_HEADER_BG, true);
    fb_draw_string(x+w-90, y+4,          "Size",  FM_HEADER_FG, FM_HEADER_BG, true);
    fb_draw_string(x+w-40, y+4,          "Type",  FM_HEADER_FG, FM_HEADER_BG, true);
    fb_draw_vline(x+w-95, y, 22, FM_BORDER);
    fb_draw_vline(x+w-45, y, 22, FM_BORDER);

    int32_t iy     = y + 24;
    int32_t vis_h  = h - 24;
    int32_t max_vis = vis_h / FM_ITEM_H;

    for (int i = fm.scroll_y;
         i < fm.entry_count && iy < y + h - 2;
         i++, iy += FM_ITEM_H) {

        fm_entry_t* e = &fm.entries[i];
        bool  sel   = (i == fm.selected);
        bool  hover = (i == fm.hover);

        /* Фон строки */
        fb_color_t row_bg = sel   ? FM_SELECTED_BG :
                            hover ? FM_HOVER_BG     : FM_BG;
        fb_color_t row_fg = sel   ? FM_SELECTED_FG : FM_ITEM_FG;

        if (sel || hover)
            fb_fill_rect(x+1, iy, w-2, FM_ITEM_H, row_bg);

        /* Иконка */
        fb_color_t ico_color;
        const char* ico = fm_icon(e, &ico_color);
        if (sel) ico_color = fb_blend(ico_color, FM_SELECTED_BG, 80);
        fb_fill_round_rect(x+2, iy+3, FM_ICON_W-2, FM_ICON_W-2, 3, ico_color);
        fb_draw_string(x+5, iy+4, ico, FM_BG, ico_color, true);

        /* Имя файла */
        int32_t name_x = x + FM_ICON_W + 4;
        int32_t max_name_w = w - 100 - FM_ICON_W - 8;
        /* Усекаем длинное имя */
        char disp_name[64];
        strncpy(disp_name, e->name, 63);
        if ((int32_t)strlen(disp_name) * FB_FONT_W > max_name_w) {
            int max_chars = max_name_w / FB_FONT_W - 2;
            if (max_chars > 0 && max_chars < 62) {
                disp_name[max_chars]   = '.';
                disp_name[max_chars+1] = '.';
                disp_name[max_chars+2] = '\0';
            }
        }
        fb_draw_string(name_x, iy+(FM_ITEM_H-FB_FONT_H)/2,
                       disp_name, row_fg, row_bg, true);

        /* Папки выделяем жирнее — дополнительная полоска */
        if (e->is_dir && !sel) {
            fb_draw_string(name_x+1, iy+(FM_ITEM_H-FB_FONT_H)/2,
                           disp_name, FM_ITEM_FG, row_bg, true);
        }

        /* Размер */
        char sz_buf[16];
        if (e->is_dir) strcpy(sz_buf, "<DIR>");
        else fm_format_size(e->size, sz_buf);
        fb_draw_string(x+w-92, iy+(FM_ITEM_H-FB_FONT_H)/2,
                       sz_buf, row_fg, row_bg, true);

        /* Тип */
        const char* type_str =
            e->is_dir   ? "Folder" :
            e->is_pe    ? "EXE/PE" :
            e->is_exe   ? "ELF"    :
            e->is_text  ? "Text"   :
            e->is_image ? "Image"  : "File";

        /* Определяем более точный тип по расширению */
        {
            const char* n2 = e->name; int ln2 = strlen(n2);
            const char* ext2 = 0;
            for (int j = ln2-1; j >= 0; j--) { if (n2[j]=='.') { ext2=n2+j+1; break; } }
            if (ext2) {
                const char* vids[]={"mp4","avi","mkv","mov","wmv","flv","webm","MP4","AVI","MKV","MOV","WMV","FLV","WEBM",0};
                const char* auds[]={"mp3","wav","ogg","flac","aac","wma","midi","mid","MP3","WAV","OGG","FLAC","AAC","WMA","MIDI","MID",0};
                const char* docs[]={"doc","docx","rtf","odt","pdf","DOC","DOCX","RTF","ODT","PDF",0};
                const char* code[]={"c","cpp","h","hpp","cc","cxx","cs","py","js","java","rb","php","C","CPP","H","HPP","CC","CXX","CS","PY","JS","JAVA","RB","PHP",0};
                const char* asm2[]={"asm","s","S","ASM",0};
                for (int k=0;code[k];k++) { if (strcmp(ext2,code[k])==0) { type_str="Code"; goto done_type; } }
                for (int k=0;asm2[k];k++) { if (strcmp(ext2,asm2[k])==0) { type_str="ASM"; goto done_type; } }
                for (int k=0;docs[k];k++) { if (strcmp(ext2,docs[k])==0) { type_str="Doc"; goto done_type; } }
            }
        }
        done_type:;

        fb_draw_string(x+w-43, iy+(FM_ITEM_H-FB_FONT_H)/2,
                       type_str, row_fg, row_bg, true);

        /* Разделитель строк */
        if (!sel && !hover)
            fb_draw_hline(x+1, iy+FM_ITEM_H-1, w-2, 0xEEEEEEu);
    }

    

    /* Скроллбар — целочисленная арифметика вместо float */
    if (fm.entry_count > 0) {
        int32_t sb_x = x + w - 1;
        int32_t sb_h = h - 24;
        fb_draw_vline(sb_x, y+24, sb_h, FM_BORDER);

        /* thumb_h = sb_h * visible / total (целочисленно) */
        int32_t total_px  = fm.entry_count * FM_ITEM_H;
        int32_t thumb_h   = (sb_h * sb_h) / (total_px > sb_h ? total_px : sb_h);
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > sb_h) thumb_h = sb_h;

        int32_t thumb_y = y + 24;
        if (fm.entry_count > 1) {
            /* thumb_y offset = (sb_h - thumb_h) * scroll / (total - 1) */
            int32_t range = sb_h - thumb_h;
            thumb_y += range * fm.scroll_y / (fm.entry_count - 1);
        }
        fb_fill_rect(sb_x-7, thumb_y, 7, thumb_h, 0xC0C0C0u);
        fb_draw_rect(sb_x-7, thumb_y, 7, thumb_h, 0x909090u);
    }
}

/* =============================================================================
 * Рисование тулбара (кнопки навигации)
 * ============================================================================= */
static void fm_draw_toolbar(int32_t x, int32_t y, int32_t w) {
    fb_fill_rect(x, y, w, FM_TOOLBAR_H, FM_TOOLBAR_BG);
    fb_draw_hline(x, y+FM_TOOLBAR_H-1, w, FM_BORDER);

    /* Кнопки: назад, вперёд, вверх, обновить */
    struct { const char* label; int32_t bx; fb_color_t c; } btns[] = {
        {"<",  x+4,  0x606080u},
        {">",  x+28, 0x606080u},
        {"^",  x+52, 0x406080u},
        {"R",  x+76, 0x408060u},
    };
    for (int i=0; i<4; i++) {
        fb_fill_round_rect(btns[i].bx, y+4, 22, 22, 4, btns[i].c);
        fb_draw_string(btns[i].bx+7, y+8, btns[i].label, FM_BG, btns[i].c, true);
    }

    /* Адресная строка */
    int32_t addr_x = x + 104;
    int32_t addr_w = w - 108;
    fb_fill_rect(addr_x, y+5, addr_w, 20, FB_WHITE);
    fb_draw_rect(addr_x, y+5, addr_w, 20, 0x9090A0u);
    fb_draw_string(addr_x+6, y+9,
                   fm.current_path, 0x202040u, FB_WHITE, true);
}

/* =============================================================================
 * Рисование статусбара
 * ============================================================================= */
static void fm_draw_statusbar(int32_t x, int32_t y, int32_t w) {
    fb_fill_rect(x, y, w, FM_STATUSBAR_H, FM_STATUS_BG);
    fb_draw_hline(x, y, w, FM_BORDER);
    fb_draw_string(x+6, y+4, fm.status_text, FM_STATUS_FG, FM_STATUS_BG, true);

    /* Подсказки справа */
    const char* hint = "Enter:Open  Del:Delete  F2:Rename  F5:Copy";
    int32_t hw = (int32_t)strlen(hint) * FB_FONT_W;
    fb_draw_string(x+w-hw-6, y+4, hint, 0xAAAAAAu, FM_STATUS_BG, true);
}

/* =============================================================================
 * Контекстное меню
 * ============================================================================= */
static void fm_draw_context_menu(void) {
    if (!fm.show_context_menu) return;
    int32_t mx=fm.ctx_x, my=fm.ctx_y;
    int32_t mw=140, mh=6*22+8;

    /* Тень */
    fb_fill_rect(mx+3, my+3, mw, mh, 0x00000050u);
    /* Фон */
    fb_fill_rect(mx, my, mw, mh, FB_WHITE);
    fb_draw_rect(mx, my, mw, mh, 0x909090u);

    const char* items[]={"Open","Edit","Run","───────","Copy path","Properties"};
    fb_color_t  colors[]={0x000080u,0x006000u,0x008040u,0xCCCCCCu,0x404040u,0x404040u};
    bool        enabled[]={true,true,true,false,true,true};

    for (int i=0;i<6;i++) {
        if (i==3) { /* separator */
            fb_draw_hline(mx+4, my+4+i*22+11, mw-8, 0xCCCCCCu);
            continue;
        }
        if (enabled[i]) {
            fb_draw_string(mx+12, my+4+i*22+5, items[i], colors[i], FB_WHITE, true);
        } else {
            fb_draw_string(mx+12, my+4+i*22+5, items[i], 0xAAAAAAAAu, FB_WHITE, true);
        }
    }
}

/* =============================================================================
 * Открыть выбранный файл/директорию
 * ============================================================================= */
static void fm_open_selected(void) {
    if (fm.selected < 0 || fm.selected >= fm.entry_count) return;
    fm_entry_t* e = &fm.entries[fm.selected];

    if (e->is_dir) {
        /* Переходим в директорию */
        char new_path[FM_PATH_MAX];
        if (strcmp(e->name, "..") == 0) {
            /* Поднимаемся вверх */
            strncpy(new_path, fm.current_path, FM_PATH_MAX-1);
            char* last = (char*)0;
            for (char* p=new_path; *p; p++) if (*p=='/') last=p;
            if (last && last != new_path) *last = '\0';
            else strcpy(new_path, "/");
        } else {
            strncpy(new_path, fm.current_path, FM_PATH_MAX-1);
            if (strcmp(new_path, "/") != 0) strncat(new_path, "/", 2);
            strncat(new_path, e->name, FM_PATH_MAX-strlen(new_path)-1);
        }
        fm_load_dir(new_path);
        return;
    }

    /* Исполняемый файл */
    if (e->is_exe) {
        char full_path[FM_PATH_MAX];
        strncpy(full_path, fm.current_path, FM_PATH_MAX-1);
        if (strcmp(full_path,"/")!=0) strncat(full_path,"/",2);
        strncat(full_path, e->name, FM_PATH_MAX-strlen(full_path)-1);

        gui_terminal_capture_start();

        if (e->is_pe) {
            /* PE/EXE */
            pe_info_t pi = pe_load_file(full_path);
            if (pi.valid) {
                char* pe_argv[2] = {full_path, (char*)0};
                pe_exec(&pi, 1, pe_argv);
            } else {
                const char* msg = "Unable to load PE executable.\n";
                gui_terminal_capture_write(msg, (uint32_t)strlen(msg));
            }
        } else {
            /* ELF */
            char* argv[] = {full_path, (char*)0};
            process_exec(full_path, 1, argv);
        }

        gui_terminal_capture_finish();
        return;
    }

    /* Код/программа — открываем в кодовом редакторе */
    if (e->is_code) {
        char full_path[FM_PATH_MAX];
        strncpy(full_path, fm.current_path, FM_PATH_MAX-1);
        if (strcmp(full_path,"/")!=0) strncat(full_path,"/",2);
        strncat(full_path, e->name, FM_PATH_MAX-strlen(full_path)-1);
        codeeditor_run(full_path);
        return;
    }

    /* Текстовый файл — открываем в обычном текстовом редакторе */
    if (e->is_text) {
        extern void editor_run(const char* filepath);
        char full_path[FM_PATH_MAX];
        strncpy(full_path, fm.current_path, FM_PATH_MAX-1);
        if (strcmp(full_path,"/")!=0) strncat(full_path,"/",2);
        strncat(full_path, e->name, FM_PATH_MAX-strlen(full_path)-1);
        editor_run(full_path);
        return;
    }

    if (e->is_doc) {
        extern void wordproc_run(const char* filepath);
        char full_path[FM_PATH_MAX];
        strncpy(full_path, fm.current_path, FM_PATH_MAX-1);
        if (strcmp(full_path,"/")!=0) strncat(full_path,"/",2);
        strncat(full_path, e->name, FM_PATH_MAX-strlen(full_path)-1);
        wordproc_run(full_path);
        return;
    }


    /* Документ — открываем в word процессоре */
    /*const char* doc_exts[] = {"doc","DOC","docx","DOCX","rtf","RTF","odt","ODT","pdf","PDF",0};
    for (int i = 0; doc_exts[i]; i++) {
        const char* n2 = e->name; int ln2 = strlen(n2);
        for (int j = ln2-1; j >= 0; j--) {
            if (n2[j]=='.') {
                if (strcmp(n2+j+1,doc_exts[i])==0) {
                    extern void wordproc_run(const char* filepath);
                    char fp[FM_PATH_MAX];
                    strncpy(fp,fm.current_path,FM_PATH_MAX-1);
                    if (strcmp(fp,"/")!=0) strncat(fp,"/",2);
                    strncat(fp,e->name,FM_PATH_MAX-strlen(fp)-1);
                    wordproc_run(fp);
                    return;
                }
                break;
            }
        }
    }*/

    /* Обновляем статус */
    char* s = fm.status_text;
    strcpy(s, "Cannot open: ");
    strncat(s, e->name, 64);
}

/* =============================================================================
 * Обработка ввода клавиатуры
 * ============================================================================= */
static void fm_handle_key(int c) {
    int vis = fm.list_h / FM_ITEM_H;

    switch (c) {
        case KEY_ARROW_UP:
            if (fm.selected > 0) fm.selected--;
            if (fm.selected < fm.scroll_y) fm.scroll_y = fm.selected;
            break;
        case KEY_ARROW_DOWN:
            if (fm.selected < fm.entry_count-1) fm.selected++;
            if (fm.selected >= fm.scroll_y + vis)
                fm.scroll_y = fm.selected - vis + 1;
            break;
        case KEY_ARROW_LEFT:
        case '\b':
            /* Вверх по дереву */
            {
                fm_entry_t fake; strcpy(fake.name,".."); fake.is_dir=true;
                int saved = fm.selected;
                fm.selected = 0; /* ".." обычно первый */
                fm_open_selected();
                fm.selected = saved;
            }
            break;
        case KEY_ARROW_RIGHT:
        case '\n': case '\r':
            fm_open_selected();
            break;
        case KEY_ESCAPE:
            if (fm.show_context_menu) fm.show_context_menu = false;
            else fm.running = false;
            break;
        case KEY_F1 + 4: /* F5 — обновить */
            fm_load_dir(fm.current_path);
            break;
        default:
            break;
    }
}

/* =============================================================================
 * Обработка мыши
 * ============================================================================= */
static void fm_handle_mouse(void) {
    const mouse_state_t* ms = mouse_get_state();
    int32_t mx = ms->x, my = ms->y;

    /* Hover в списке */
    if (mx >= fm.list_x && mx < fm.list_x + fm.list_w &&
        my >= fm.list_y + 24 && my < fm.list_y + fm.list_h) {
        int idx = fm.scroll_y + (my - fm.list_y - 24) / FM_ITEM_H;
        fm.hover = (idx < fm.entry_count) ? idx : -1;
    } else {
        fm.hover = -1;
    }

    if (!ms->left_click && !ms->right_click) return;

    /* Закрыть контекстное меню */
    if (fm.show_context_menu) {
        fm.show_context_menu = false;
        mouse_consume_clicks();
        return;
    }

    /* ── Клик по кнопке X (закрыть окно) ── */
    if (ms->left_click) {
        int32_t bx = fm.win_x + fm.win_w - 24;
        int32_t by = fm.win_y + 4;
        if (mx >= bx && mx < bx+20 && my >= by && my < by+20) {
            fm.running = false;
            mouse_consume_clicks();
            return;
        }
    }

    /* ── Клик по кнопкам тулбара ── */
    if (ms->left_click &&
        my >= fm.win_y + FM_TITLE_H + 1 &&
        my <  fm.win_y + FM_TITLE_H + FM_TOOLBAR_H + 1) {
        int32_t tx = fm.win_x + 1;
        /* Кнопка "<" (назад) */
        if (mx >= tx+6 && mx < tx+34) {
            /* Вверх по дереву */
            char parent[FM_PATH_MAX];
            strncpy(parent, fm.current_path, FM_PATH_MAX-1);
            char* last = (char*)0;
            for (char* p=parent; *p; p++) if (*p=='/') last=p;
            if (last && last != parent) *last = '\0';
            else strcpy(parent, "/");
            fm_load_dir(parent);
            mouse_consume_clicks();
            return;
        }
        /* Кнопка "R" (обновить) */
        if (mx >= tx+70 && mx < tx+98) {
            fm_load_dir(fm.current_path);
            mouse_consume_clicks();
            return;
        }
        mouse_consume_clicks();
        return;
    }

    /* Клик в дерево (левая панель) */
    if (ms->left_click &&
        mx >= fm.tree_x && mx < fm.tree_x + fm.tree_w &&
        my >= fm.tree_y + 26) {

        const char* roots[] = {"/", "/mnt", "/tmp", "/etc", "/programs"};
        int row = (my - fm.tree_y - 26) / FM_ITEM_H;
        bool has_disk = vfs_exists("/mnt/disk0");
        int nroots = 5 + (has_disk ? 1 : 0);
        if (row >= 0 && row < nroots) {
            const char* target = (row < 5) ? roots[row] : "/mnt/disk0";
            fm_load_dir(target);
        }
        mouse_consume_clicks();
        return;
    }

    /* Клик в список (правая панель) */
    if (mx >= fm.list_x && mx < fm.list_x + fm.list_w &&
        my >= fm.list_y + 24) {

        int idx = fm.scroll_y + (my - fm.list_y - 24) / FM_ITEM_H;
        if (idx >= 0 && idx < fm.entry_count) {
            if (ms->left_click) {
                if (idx == fm.selected) {
                    /* Двойной клик → открыть */
                    fm_open_selected();
                } else {
                    fm.selected = idx;
                }
            } else if (ms->right_click) {
                fm.selected = idx;
                fm.show_context_menu = true;
                fm.ctx_x = mx;
                fm.ctx_y = my;
            }
        }
        mouse_consume_clicks();
        return;
    }

    mouse_consume_clicks();
}

/* =============================================================================
 * Главный цикл отрисовки
 * ============================================================================= */
static void fm_render(void) {
    int32_t x=fm.win_x, y=fm.win_y, w=fm.win_w, h=fm.win_h;

    /* Внешняя рамка окна */
    fb_fill_rect(x+3, y+3, w, h, 0x00000050u);  /* тень */
    fb_fill_rect(x, y, w, h, FM_BG);
    fb_draw_rect(x, y, w, h, 0x606080u);

    /* Заголовок окна */
    fb_fill_rect(x+1, y+1, w-2, FM_TITLE_H, FM_TITLEBAR);
    fb_draw_string_scaled(x+10, y+6, "File Manager", FM_TITLEBAR_FG, 1);
    /* Кнопка закрыть */
    fb_fill_rect(x+w-24, y+4, 20, 20, 0xCC2020u);
    fb_draw_string(x+w-19, y+8, "X", FB_WHITE, 0xCC2020u, true);
    /* Кнопки свернуть/развернуть */
    fb_fill_rect(x+w-50, y+4, 20, 20, 0xAAAAAAAAu);
    fb_draw_string(x+w-45, y+8, "_", FM_ITEM_FG, 0xAAAAAAAAu, true);

    /* Тулбар */
    fm_draw_toolbar(x+1, y+FM_TITLE_H+1, w-2);

    /* Дерево */
    fm_draw_tree();

    /* Список */
    fm_draw_list();

    /* Статусбар */
    fm_draw_statusbar(x+1, y+h-FM_STATUSBAR_H-1, w-2);

    /* Вертикальный разделитель между деревом и списком */
    fb_draw_vline(fm.tree_x+fm.tree_w, fm.tree_y, fm.tree_h, FM_BORDER);

    /* Контекстное меню */
    fm_draw_context_menu();
}

/* =============================================================================
 * filemanager_run — точка входа
 * ============================================================================= */
void filemanager_run(const char* start_path) {
    if (!fb_ready()) return;

    memset(&fm, 0, sizeof(fm));
    fm.running  = true;
    fm.selected = 0;
    fm.hover    = -1;

    /* Размеры окна — занимаем большую часть экрана */
    int32_t sw = (int32_t)fb_width();
    int32_t sh = (int32_t)fb_height();
    fm.win_w = sw - 80;
    fm.win_h = sh - 80;
    fm.win_x = 40;
    fm.win_y = 30;

    int32_t content_y = fm.win_y + FM_TITLE_H + FM_TOOLBAR_H + 2;
    int32_t content_h = fm.win_h - FM_TITLE_H - FM_TOOLBAR_H - FM_STATUSBAR_H - 4;

    fm.tree_x = fm.win_x + 1;
    fm.tree_y = content_y;
    fm.tree_w = FM_TREE_W;
    fm.tree_h = content_h;

    fm.list_x = fm.win_x + FM_TREE_W + 2;
    fm.list_y = content_y;
    fm.list_w = fm.win_w - FM_TREE_W - 3;
    fm.list_h = content_h;

    /* Загружаем начальную директорию */
    fm_load_dir(start_path ? start_path : "/");

    uint32_t last_tick = pit_get_ticks();

    while (fm.running) {
        /* Ограничиваем FPS */
        uint32_t now = pit_get_ticks();
        if (now - last_tick < 1) {
            __asm__ volatile ("hlt");
            continue;
        }
        last_tick = now;

        /* Ввод */
        fm_handle_mouse();
        if (keyboard_has_input()) {
            int c = keyboard_getkey();
            fm_handle_key(c);
        }

        /* Рендер */
        fb_clear(0x8090A0u);  /* рабочий стол за окном */
        fm_render();
        /* Курсор всегда поверх — иначе он пропадает под окном */
        {
            const mouse_state_t* cur_ms = mouse_get_state();
            gui_draw_cursor(cur_ms->x, cur_ms->y);
        }
        fb_swap();
    }
}
