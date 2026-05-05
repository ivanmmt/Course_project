/* =============================================================================
 * kernel/process/process.c
 * Управление процессами — загрузка, запуск, завершение ELF программ
 * ============================================================================= */

#include "process.h"
#include "../syscall/syscall.h"
#include "../../filesystem/elf.h"
#include "../../filesystem/vfs.h"
#include "../../memory/heap.h"
#include "../../memory/pmm.h"
#include "../../drivers/vga/vga_text.h"
#include "../../drivers/timer/pit.h"
#include "../../lib/string.h"
#include "../../kernel/kernel.h"

static process_t  proc_table[PROC_MAX];
static process_t* current_proc = (process_t*)0;
static uint32_t   next_pid     = 1;

/* =============================================================================
 * process_init
 * ============================================================================= */
void process_init(void) {
    memset(proc_table, 0, sizeof(proc_table));
    current_proc = (process_t*)0;
}

process_t* process_current(void) { return current_proc; }

/* =============================================================================
 * Найти свободный слот в таблице процессов
 * ============================================================================= */
static process_t* proc_alloc(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_EMPTY) {
            memset(&proc_table[i], 0, sizeof(process_t));
            proc_table[i].pid   = next_pid++;
            proc_table[i].state = PROC_RUNNING;
            return &proc_table[i];
        }
    }
    return (process_t*)0;
}

/* =============================================================================
 * process_exec_buf — запустить ELF из буфера памяти
 * ============================================================================= */
int process_exec_buf(const char* name, const uint8_t* elf_data,
                     uint32_t elf_size, int argc, char** argv) {
    /* Создаём дескриптор процесса */
    process_t* proc = proc_alloc();
    if (!proc) {
        vga_put_string("  [PROC] No free process slots\n");
        return -1;
    }

    strncpy(proc->name, name ? name : "unknown", PROC_NAME_MAX - 1);

    /* Выделяем стек */
    proc->stack = (uint8_t*)kmalloc(PROC_STACK_SIZE);
    if (!proc->stack) {
        proc->state = PROC_EMPTY;
        vga_put_string("  [PROC] Stack allocation failed\n");
        return -1;
    }
    memset(proc->stack, 0, PROC_STACK_SIZE);
    proc->stack_top = (uint32_t)proc->stack + PROC_STACK_SIZE - 16;

    /* Загружаем ELF */
    elf_info_t elf = elf_load(elf_data, elf_size);
    if (!elf.valid) {
        kfree(proc->stack);
        proc->state = PROC_EMPTY;
        vga_put_string("  [PROC] ELF load failed\n");
        return -1;
    }

    proc->entry     = elf.entry;
    proc->load_base = elf.load_base;
    proc->load_end  = elf.load_end;

    vga_printf("  [PROC] Starting '%s' pid=%u entry=0x%x\n",
               proc->name, proc->pid, proc->entry);

    /* Очищаем флаг выхода */
    syscall_clear_exit();

    /* Запускаем процесс */
    process_t* saved_proc = current_proc;
    current_proc = proc;

    /* Прыжок на точку входа ELF
     * Передаём argc и argv через регистры/стек по соглашению cdecl:
     *   stack: [return_addr][argc][argv]
     * Используем новый стек процесса */

    typedef int (*entry_fn_t)(int, char**);
    entry_fn_t entry = (entry_fn_t)proc->entry;

    /* Переключаем стек процесса и вызываем точку входа ELF.
     *
     * Проблема с inline ASM при переключении стека:
     * Компилятор не знает что ESP меняется — он может держать
     * переменные на стеке. Поэтому используем отдельные ASM блоки
     * с правильными output/input операндами.
     *
     * "=r" — output в регистр (не на стек!)
     * "r"  — input из регистра
     */
    uint32_t old_esp = 0;
    uint32_t new_esp = proc->stack_top;
    int      ret_val = 0;

    /* Шаг 1: сохраняем текущий ESP в переменную (через регистр) */
    __asm__ volatile (
        "mov %%esp, %0"
        : "=r"(old_esp)
        :
        : "memory"
    );

    /* Шаг 2: переключаемся на стек процесса */
    __asm__ volatile (
        "mov %0, %%esp"
        :
        : "r"(new_esp)
        : "memory"
    );

    /* Шаг 3: вызываем программу на новом стеке */
    ret_val = entry(argc, argv);

    /* Шаг 4: восстанавливаем стек ядра */
    __asm__ volatile (
        "mov %0, %%esp"
        :
        : "r"(old_esp)
        : "memory"
    );

    /* Проверяем завершение через exit() */
    if (syscall_exit_requested()) {
        proc->exit_code = syscall_exit_code();
        syscall_clear_exit();
    } else {
        proc->exit_code = ret_val;
    }

    vga_printf("  [PROC] '%s' exited with code %d\n",
               proc->name, proc->exit_code);

    /* Освобождаем ресурсы */
    kfree(proc->stack);
    proc->stack = (uint8_t*)0;
    proc->state = PROC_ZOMBIE;

    current_proc = saved_proc;

    int code = proc->exit_code;
    proc->state = PROC_EMPTY;
    return code;
}

/* =============================================================================
 * process_exec — запустить ELF файл по пути в VFS
 * ============================================================================= */
int process_exec(const char* path, int argc, char** argv) {
    if (!path) return -1;

    /* Читаем файл из VFS */
    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0 || st.size == 0) {
        vga_printf("  [PROC] File not found: %s\n", path);
        return -1;
    }

    uint8_t* buf = (uint8_t*)kmalloc(st.size);
    if (!buf) return -1;

    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) { kfree(buf); return -1; }

    int32_t n = vfs_read(fd, buf, st.size);
    vfs_close(fd);

    if (n <= 0) { kfree(buf); return -1; }

    /* Извлекаем имя файла для display */
    const char* name = path;
    const char* p = path;
    while (*p) { if (*p == '/') name = p + 1; p++; }

    int ret = process_exec_buf(name, buf, (uint32_t)n, argc, argv);
    kfree(buf);
    return ret;
}

/* =============================================================================
 * process_exit — вызывается при syscall SYS_EXIT
 * ============================================================================= */
void process_exit(int code) {
    if (current_proc) {
        current_proc->exit_code = code;
        current_proc->state     = PROC_ZOMBIE;
    }
    /* syscall.c уже установил флаг, процесс завершится после возврата из entry */
}

/* =============================================================================
 * process_wait — ждать завершения (с периодической проверкой вместо busy-wait)
 * ============================================================================= */
int process_wait(int pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if ((int)proc_table[i].pid == pid) {
            /* Ждём пока процесс не завершится, проверяем каждые 10ms */
            while (proc_table[i].state == PROC_RUNNING) {
                pit_sleep(10);  /* Спим 10ms вместо busy-wait hlt */
            }
            return proc_table[i].exit_code;
        }
    }
    return -1;
}

/* =============================================================================
 * process_list — вывести таблицу процессов
 * ============================================================================= */
void process_list(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_put_string("  PID  STATE    NAME\n");
    vga_put_string("  ---  -------  ----\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    bool any = false;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t* p = &proc_table[i];
        if (p->state == PROC_EMPTY) continue;
        any = true;
        const char* state_str =
            p->state == PROC_RUNNING ? "running" :
            p->state == PROC_ZOMBIE  ? "zombie " : "?";
        vga_printf("  %3u  %s  %s\n", p->pid, state_str, p->name);
    }
    if (!any) vga_put_string("  (no processes)\n");
}
