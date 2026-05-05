/* =============================================================================
 * kernel/process/process.h
 * Process Manager — управление процессами
 *
 * На данном этапе реализуем упрощённый менеджер:
 *   - Загрузка ELF в память
 *   - Выполнение с отдельным стеком
 *   - Обработка exit()
 *   - Возврат управления ядру после завершения
 *
 * Полноценная многозадачность (переключение контекста, планировщик)
 * будет добавлена позже. Сейчас — cooperative single-tasking.
 * ============================================================================= */

#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdbool.h>

#define PROC_STACK_SIZE  (64 * 1024)  /* 64 КБ стека для процесса */
#define PROC_MAX         8            /* максимум одновременных процессов */
#define PROC_NAME_MAX    64

/* Состояния процесса */
typedef enum {
    PROC_EMPTY   = 0,
    PROC_RUNNING = 1,
    PROC_ZOMBIE  = 2,   /* завершён, ждёт wait() */
} proc_state_t;

/* Дескриптор процесса */
typedef struct {
    uint32_t     pid;
    proc_state_t state;
    char         name[PROC_NAME_MAX];

    /* Адресное пространство */
    uint32_t     entry;       /* точка входа ELF */
    uint32_t     load_base;   /* начало загруженного ELF */
    uint32_t     load_end;    /* конец загруженного ELF */

    /* Стек */
    uint8_t*     stack;       /* выделенный стек */
    uint32_t     stack_top;   /* вершина стека */

    /* Результат */
    int          exit_code;
} process_t;

/* ─── API ─── */
void process_init(void);

/* Создать процесс из ELF файла */
int process_exec(const char* path, int argc, char** argv);

/* Создать процесс из буфера памяти */
int process_exec_buf(const char* name, const uint8_t* elf_data,
                     uint32_t elf_size, int argc, char** argv);

/* Получить текущий процесс */
process_t* process_current(void);

/* Завершить текущий процесс */
void process_exit(int code);

/* Ждать завершения процесса (блокирующее) */
int process_wait(int pid);

/* Вывести список процессов */
void process_list(void);

#endif /* PROCESS_H */
