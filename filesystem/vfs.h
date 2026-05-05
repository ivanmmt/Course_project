/* =============================================================================
 * filesystem/vfs.h
 * Virtual File System — единый интерфейс для всех файловых систем
 *
 * АРХИТЕКТУРА:
 *   VFS — это абстрактный слой. Код ядра работает только с VFS API
 *   (vfs_open, vfs_read, vfs_write...) не зная о конкретной ФС снизу.
 *
 *   Дерево монтирования:
 *     /           ← корень (наша initrd/ramfs)
 *     /dev/       ← устройства
 *     /programs/  ← исполняемые файлы
 *
 *   Каждая ФС регистрирует набор функций (vfs_ops_t).
 *   VFS вызывает их через указатели — как vtable в C++.
 * ============================================================================= */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── Константы ─── */
#define VFS_NAME_MAX     128   /* максимальная длина имени файла */
#define VFS_PATH_MAX     512   /* максимальная длина пути */
#define VFS_MAX_NODES    256   /* максимум нод в VFS дереве */
#define VFS_MAX_FDS      32    /* максимум открытых файлов */

/* ─── Флаги открытия файла ─── */
#define VFS_O_READ    (1 << 0)
#define VFS_O_WRITE   (1 << 1)
#define VFS_O_CREATE  (1 << 2)
#define VFS_O_TRUNC   (1 << 3)
#define VFS_O_APPEND  (1 << 4)
#define VFS_O_RDWR    (VFS_O_READ | VFS_O_WRITE)

/* ─── Тип ноды ─── */
typedef enum {
    VFS_FILE      = 1,
    VFS_DIRECTORY = 2,
    VFS_DEVICE    = 3,
    VFS_SYMLINK   = 4,
} vfs_node_type_t;

/* ─── Предварительное объявление ─── */
typedef struct vfs_node vfs_node_t;

/* ─── Операции файловой системы (vtable) ─── */
typedef struct {
    /* Чтение данных из файла */
    int32_t (*read) (vfs_node_t* node, uint32_t offset,
                     uint32_t size, uint8_t* buffer);
    /* Запись данных в файл */
    int32_t (*write)(vfs_node_t* node, uint32_t offset,
                     uint32_t size, const uint8_t* buffer);
    /* Открыть файл (инициализация) */
    int32_t (*open) (vfs_node_t* node, uint32_t flags);
    /* Закрыть файл */
    void    (*close)(vfs_node_t* node);
    /* Читать запись директории (для ls) */
    vfs_node_t* (*readdir)(vfs_node_t* node, uint32_t index);
    /* Найти дочернюю ноду по имени */
    vfs_node_t* (*finddir)(vfs_node_t* node, const char* name);
    /* Создать файл/директорию */
    int32_t (*create)(vfs_node_t* parent, const char* name,
                      vfs_node_type_t type);
    /* Удалить ноду */
    int32_t (*unlink)(vfs_node_t* parent, const char* name);
} vfs_ops_t;

/* ─── VFS нода (inode) ─── */
struct vfs_node {
    char            name[VFS_NAME_MAX]; /* имя файла/директории */
    vfs_node_type_t type;               /* тип (файл, директория, ...) */
    uint32_t        flags;              /* права доступа */
    uint32_t        size;               /* размер в байтах */
    uint32_t        inode;             /* уникальный идентификатор */

    /* Данные файла (для ramfs: указатель на буфер в памяти) */
    uint8_t*        data;
    uint32_t        data_capacity;     /* выделено байт */

    /* Связь в дереве */
    vfs_node_t*     parent;
    vfs_node_t*     children[64];      /* дочерние ноды (для директорий) */
    uint32_t        child_count;

    /* Операции ФС */
    vfs_ops_t*      ops;

    /* Пользовательские данные (специфичны для ФС) */
    void*           fs_data;
};

/* ─── Файловый дескриптор ─── */
typedef struct {
    vfs_node_t* node;       /* нода к которой относится */
    uint32_t    offset;     /* текущая позиция */
    uint32_t    flags;      /* VFS_O_* */
    bool        used;       /* занят ли слот */
} vfs_fd_t;

/* ─── Запись директории (для readdir) ─── */
typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
    uint32_t size;
    vfs_node_type_t type;
} vfs_dirent_t;

/* ─── Статистика файла ─── */
typedef struct {
    uint32_t        size;
    vfs_node_type_t type;
    uint32_t        inode;
} vfs_stat_t;

/* ─── API ─── */

/* Инициализация VFS */
void vfs_init(void);

/* Монтирование корневой ФС */
void vfs_mount_root(vfs_node_t* root);

/* Операции с путями */
vfs_node_t* vfs_resolve(const char* path);      /* найти ноду по пути */
vfs_node_t* vfs_resolve_rel(vfs_node_t* cwd,
                              const char* path); /* относительный путь */

/* Высокоуровневый API (работает как POSIX) */
int32_t  vfs_open (const char* path, uint32_t flags);  /* → fd */
void     vfs_close(int32_t fd);
int32_t  vfs_read (int32_t fd, void* buf, uint32_t size);
int32_t  vfs_write(int32_t fd, const void* buf, uint32_t size);
int32_t  vfs_seek (int32_t fd, int32_t offset, int whence);
int32_t  vfs_tell (int32_t fd);
int32_t  vfs_stat (const char* path, vfs_stat_t* st);
bool     vfs_exists(const char* path);

/* Директории */
int32_t      vfs_mkdir(const char* path);
vfs_dirent_t vfs_readdir(const char* path, uint32_t index);
int32_t      vfs_unlink(const char* path);

/* Прямой доступ к ноде (для ядра) */
int32_t  vfs_node_read (vfs_node_t* n, uint32_t off,
                         uint32_t sz, uint8_t* buf);
int32_t  vfs_node_write(vfs_node_t* n, uint32_t off,
                         uint32_t sz, const uint8_t* buf);

/* Получить ноду по fd */
vfs_node_t* vfs_get_node(int32_t fd);

/* Текущая директория */
vfs_node_t* vfs_get_cwd(void);
void        vfs_set_cwd(vfs_node_t* dir);

#endif /* VFS_H */
