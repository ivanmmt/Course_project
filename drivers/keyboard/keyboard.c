/* =============================================================================
 * drivers/keyboard/keyboard.c
 * PS/2 Keyboard Driver — исправлен для поддержки KEY_ARROW_* как int
 * ============================================================================= */

#include "keyboard.h"
#include "../../kernel/idt.h"
#include "../../kernel/kernel.h"

/* Кольцевой буфер хранит int16_t: ASCII 0-127 или KEY_ARROW_* > 0x100 */
static int16_t  kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t buf_head = 0;
static volatile uint32_t buf_tail = 0;

static volatile bool shift_pressed = false;
static volatile bool ctrl_pressed  = false;
static volatile bool caps_lock     = false;
static volatile bool num_lock      = true;
static volatile int  current_layout = LAYOUT_US;

/* ─── US QWERTY ─── */
static const char scancode_to_ascii[] = {
    0,    0,   '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=',  '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', '\n', 0,  'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'','`',  0,  '\\','z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*',
    0,   ' ',  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static const char scancode_to_ascii_shift[] = {
    0,    0,   '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+',  '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n', 0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?',  0,  '*',
    0,   ' ',  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

/* ─── Ukrainian (UA) JCUKEN ─── */
static const char scancode_to_ascii_ua[] = {
    0,    0,   '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=',  '\b', '\t',
    0,    0,   0,   0,   0,   0,   0,   0,   /* q w e r t y u i */
    0,    0,   '[', ']', '\n', 0,   0,   0,   /* o p        a s  */
    0,    0,   0,   0,   0,   0,   0,   ';',  /* d f g h j k l ; */
    '\'','`',  0,  '\\', 0,   0,   0,   0,    /* ' ` \ z x c v    */
    0,    0,   0,   ',', '.', '/', 0,   '*',  /* b n m , . /      */
    0,   ' ',  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static const char scancode_to_ascii_ua_shift[] = {
    0,    0,   '!', '"', '#', '$', '%', ':',
    '&', '*', '(', ')', '_', '+',  '\b', '\t',
    0,    0,   0,   0,   0,   0,   0,   0,    /* Й Ц У К Е Н Г Ш */
    0,    0,   '{', '}', '\n', 0,   0,   0,    /* Щ З        Ф І  */
    0,    0,   0,   0,   0,   0,   0,   ':',   /* Ж Й        Л :  */
    '"', '~',  0,  '|',   0,   0,   0,   0,    /* " ~  |  Х Ї Ч  */
    0,    0,   0,   '<', '>', '?',  0,  '*',
    0,   ' ',  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

/* Украинские символы по скан-кодам (буквы)
 * Скан-коды: 0x10=q->Й, 0x11=w->Ц, 0x12=e->У, 0x13=r->К, 0x14=t->Е,
 *            0x15=y->Н, 0x16=u->Г, 0x17=i->Ш, 0x18=o->Щ, 0x19=p->З,
 *            0x1E=a->Ф, 0x1F=s->І, 0x20=d->В, 0x21=f->А, 0x22=g->П,
 *            0x23=h->Р, 0x24=j->О, 0x25=k->Л, 0x26=l->Д, 0x27=;->Ж,
 *            0x2C=z->Я, 0x2D=x->Х, 0x2E=c->Ч, 0x2F=v->С, 0x30=b->М,
 *            0x31=n->И, 0x32=m->Т, 0x33=,->Ь, 0x34=.->Б, 0x35=/->Ю
 */
static const char ua_lower[] = {
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l', [0x27]=';',
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b',
    [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/',
};
static const char ua_upper[] = {
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
    [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L', [0x27]=':',
    [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B',
    [0x31]='N', [0x32]='M', [0x33]='?', [0x34]='>', [0x35]='|',
};

/* NumPad таблица */
static const int numpad_normal[10] = {
    KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4,
    KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9
};
static const int numpad_numlock[10] = {
    0, '1', '2', '3', '4', '5', '6', '7', '8', '9'
};

/* Скан-коды расширенных клавиш (prefix 0xE0 уже принят) */
/* В PS/2 Set 1: стрелки: 0xE0 0x48/0x50/0x4B/0x4D */
static bool extended = false; /* получили 0xE0 */

static void buf_push(int16_t val) {
    uint32_t next = (buf_head + 1) % KBD_BUFFER_SIZE;
    if (next != buf_tail) {
        kbd_buffer[buf_head] = val;
        buf_head = next;
    }
}

static void keyboard_irq_handler(registers_t* regs) {
    (void)regs;
    uint8_t scancode = inb(KBD_DATA_PORT);

    /* Префикс расширенного кода */
    if (scancode == 0xE0) { extended = true; return; }

    bool released = (scancode & 0x80) != 0;
    uint8_t code  = scancode & 0x7F;

    if (extended) {
        extended = false;
        if (!released) {
            switch (code) {
                case 0x48: buf_push(KEY_ARROW_UP);    return;
                case 0x50: buf_push(KEY_ARROW_DOWN);  return;
                case 0x4B: buf_push(KEY_ARROW_LEFT);  return;
                case 0x4D: buf_push(KEY_ARROW_RIGHT); return;
                case 0x53: buf_push(KEY_DELETE);      return;
                default: return;
            }
        }
        return;
    }

    /* Модификаторы */
    if (code == KBD_SCANCODE_LSHIFT || code == KBD_SCANCODE_RSHIFT) {
        shift_pressed = !released; return;
    }
    if (code == KBD_SCANCODE_CTRL)  { ctrl_pressed = !released; return; }
    if (code == KBD_SCANCODE_CAPS && !released) { caps_lock = !caps_lock; return; }

    /* NumLock — скан-код 0x45 */
    if (code == 0x45 && !released) { num_lock = !num_lock; return; }

    if (released) return;

    /* ─── NumPad (скан-коды 0x47-0x52) ─── */
    if (code >= 0x47 && code <= 0x52) {
        int digit = 0;
        switch (code) {
            case 0x47: digit = 7; break;  /* Home / 7 */
            case 0x48: digit = 8; break;  /* Up / 8 */
            case 0x49: digit = 9; break;  /* PgUp / 9 */
            case 0x4B: digit = 4; break;  /* Left / 4 */
            case 0x4C: digit = 5; break;  /* Center / 5 */
            case 0x4D: digit = 6; break;  /* Right / 6 */
            case 0x4F: digit = 1; break;  /* End / 1 */
            case 0x50: digit = 2; break;  /* Down / 2 */
            case 0x51: digit = 3; break;  /* PgDn / 3 */
            case 0x52: digit = 0; break;  /* Insert / 0 */
        }
        if (num_lock) {
            buf_push((int16_t)numpad_numlock[digit]);
        } else {
            buf_push((int16_t)numpad_normal[digit]);
        }
        return;
    }

    /* NumPad специальные */
    if (code == 0x35 && shift_pressed) { buf_push(KEY_KP_DIVIDE); return; }
    if (code == 0x37) { buf_push(KEY_KP_MULTIPLY); return; }
    if (code == 0x4A) { buf_push(KEY_KP_MINUS); return; }
    if (code == 0x4E) { buf_push(KEY_KP_PLUS); return; }
    if (code == 0x53 && num_lock) { buf_push(KEY_KP_DOT); return; }
    if (code == 0x1C) { buf_push(KEY_KP_ENTER); return; }

    /* Ctrl+буква → управляющий код 1-26 */
    if (ctrl_pressed && code >= 0x10 && code <= 0x26) {
        char base = 0;
        if (current_layout == LAYOUT_UA) {
            if (code < 0x36) base = ua_lower[code];
        } else {
            base = scancode_to_ascii[code];
        }
        if ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z'))
            buf_push((int16_t)((base | 0x20) - 'a' + 1));
        return;
    }

    /* ─── Основная логика раскладки ─── */
    char ascii = 0;
    if (code < 0x59) {
        bool use_shift = shift_pressed;
        if (caps_lock && code >= 0x10 && code <= 0x32) use_shift = !use_shift;

        if (current_layout == LAYOUT_UA) {
            /* Украинская раскладка */
            if (code >= 0x10 && code <= 0x35 && ua_lower[code]) {
                ascii = use_shift ? ua_upper[code] : ua_lower[code];
            } else {
                /* Цифры и символы — из US */
                ascii = use_shift ? scancode_to_ascii_shift[code]
                                  : scancode_to_ascii[code];
            }
        } else {
            /* US раскладка */
            ascii = use_shift ? scancode_to_ascii_shift[code]
                              : scancode_to_ascii[code];
        }
    }
    if (ascii) buf_push((int16_t)ascii);
}

void keyboard_init(void) {
    idt_register_handler(33, keyboard_irq_handler);
}

bool keyboard_has_input(void) { return buf_head != buf_tail; }

/* Блокирующее чтение — возвращает int (включая KEY_ARROW_*) */
int keyboard_getkey(void) {
    while (!keyboard_has_input()) __asm__ volatile ("hlt");
    int16_t val = kbd_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % KBD_BUFFER_SIZE;
    return (int)val;
}

/* ASCII-only версия (для шелла) */
char keyboard_getchar(void) {
    int k = keyboard_getkey();
    if (k > 0 && k <= 127) return (char)k;
    return 0;
}

bool keyboard_shift(void) { return shift_pressed; }
bool keyboard_ctrl(void)  { return ctrl_pressed;  }
bool keyboard_numlock(void){ return num_lock;      }

void keyboard_set_layout(int layout) { current_layout = layout; }
int  keyboard_get_layout(void)       { return current_layout;   }
