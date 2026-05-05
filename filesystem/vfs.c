/* =============================================================================
 * filesystem/vfs.c
 * Реализация Virtual File System
 * ============================================================================= */

#include "vfs.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../kernel/kernel.h"

/* ─── Глобальное состояние VFS ─── */
static vfs_node_t* vfs_root = (vfs_node_t*)0;  /* корневая нода "/" */
static vfs_node_t* vfs_cwd  = (vfs_node_t*)0;  /* текущая директория */

/* Таблица файловых дескрипторов */
static vfs_fd_t fd_table[VFS_MAX_FDS];

/* Счётчик inode */

/* =============================================================================
 * Вспомогательные функции
 * ============================================================================= */

static int32_t alloc_fd(void) {
    for (int i = 3; i < VFS_MAX_FDS; i++) {  /* 0,1,2 зарезервированы */
        if (!fd_table[i].used) return i;
    }
    return -1;
}

static bool fd_valid(int32_t fd) {
    return fd >= 0 && fd < VFS_MAX_FDS && fd_table[fd].used;
}

/* =============================================================================
 * vfs_init — инициализация таблиц
 * ============================================================================= */
void vfs_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    vfs_root = (vfs_node_t*)0;
    vfs_cwd  = (vfs_node_t*)0;
}

void vfs_mount_root(vfs_node_t* root) {
    vfs_root = root;
    vfs_cwd  = root;
}

/* =============================================================================
 * vfs_resolve — найти ноду по абсолютному или относительному пути
 *
 * Разбиваем путь по '/' и идём по дереву.
 * Например "/programs/shell" → root → programs → shell
 * ============================================================================= */
vfs_node_t* vfs_resolve(const char* path) {
    if (!path || !vfs_root) return (vfs_node_t*)0;

    /* Абсолютный путь начинается с '/' */
    vfs_node_t* node = (path[0] == '/') ? vfs_root : vfs_cwd;
    if (!node) return (vfs_node_t*)0;

    /* Пропускаем ведущий '/' */
    const char* p = path;
    while (*p == '/') p++;

    /* Идём по компонентам пути */
    char component[VFS_NAME_MAX];
    while (*p) {
        /* Читаем следующий компонент до '/' */
        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX - 1)
            component[i++] = *p++;
        component[i] = '\0';

        /* Пропускаем повторные '/' */
        while (*p == '/') p++;

        /* Специальные компоненты */
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (node->parent) node = node->parent;
            continue;
        }

        /* Ищем дочернюю ноду */
        if (node->ops && node->ops->finddir) {
            vfs_node_t* child = node->ops->finddir(node, component);
            if (!child) return (vfs_node_t*)0;
            node = child;
        } else {
            /* Линейный поиск среди дочерних нод */
            bool found = false;
            for (uint32_t ci = 0; ci < node->child_count; ci++) {
                if (strcmp(node->children[ci]->name, component) == 0) {
                    node = node->children[ci];
                    found = true;
                    break;
                }
            }
            if (!found) return (vfs_node_t*)0;
        }
    }
    return node;
}

vfs_node_t* vfs_resolve_rel(vfs_node_t* cwd, const char* path) {
    vfs_node_t* saved = vfs_cwd;
    vfs_cwd = cwd;
    vfs_node_t* result = vfs_resolve(path);
    vfs_cwd = saved;
    return result;
}

/* =============================================================================
 * Высокоуровневый API
 * ============================================================================= */

int32_t vfs_open(const char* path, uint32_t flags) {
    vfs_node_t* node = vfs_resolve(path);

    /* Создать если нет и установлен O_CREATE */
    if (!node && (flags & VFS_O_CREATE)) {
        /* Отделяем имя файла от пути родителя */
        char parent_path[VFS_PATH_MAX];
        strncpy(parent_path, path, VFS_PATH_MAX - 1);

        char* last_slash = (char*)0;
        for (char* c = parent_path; *c; c++)
            if (*c == '/') last_slash = c;

        const char* filename = path;
        vfs_node_t* parent   = vfs_root;

        if (last_slash) {
            *last_slash = '\0';
            filename    = last_slash + 1;
            parent      = vfs_resolve(parent_path);
        }

        if (!parent) return -1;
        if (parent->ops && parent->ops->create)
            parent->ops->create(parent, filename, VFS_FILE);

        node = vfs_resolve(path);
    }

    if (!node) return -1;

    int32_t fd = alloc_fd();
    if (fd < 0) return -1;

    if (node->ops && node->ops->open)
        node->ops->open(node, flags);

    fd_table[fd].node   = node;
    fd_table[fd].offset = (flags & VFS_O_APPEND) ? node->size : 0;
    fd_table[fd].flags  = flags;
    fd_table[fd].used   = true;

    if (flags & VFS_O_TRUNC) node->size = 0;

    return fd;
}

void vfs_close(int32_t fd) {
    if (!fd_valid(fd)) return;
    vfs_node_t* node = fd_table[fd].node;
    if (node && node->ops && node->ops->close)
        node->ops->close(node);
    fd_table[fd].used = false;
}

int32_t vfs_read(int32_t fd, void* buf, uint32_t size) {
    if (!fd_valid(fd)) return -1;
    vfs_fd_t* f = &fd_table[fd];
    if (!(f->flags & VFS_O_READ)) return -1;

    int32_t n = vfs_node_read(f->node, f->offset, size, (uint8_t*)buf);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int32_t vfs_write(int32_t fd, const void* buf, uint32_t size) {
    if (!fd_valid(fd)) return -1;
    vfs_fd_t* f = &fd_table[fd];
    if (!(f->flags & VFS_O_WRITE)) return -1;

    int32_t n = vfs_node_write(f->node, f->offset, size, (const uint8_t*)buf);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int32_t vfs_seek(int32_t fd, int32_t offset, int whence) {
    if (!fd_valid(fd)) return -1;
    vfs_fd_t* f = &fd_table[fd];
    switch (whence) {
        case 0: f->offset = (uint32_t)offset; break;            /* SEEK_SET */
        case 1: f->offset = (uint32_t)((int32_t)f->offset + offset); break; /* SEEK_CUR */
        case 2: f->offset = (uint32_t)((int32_t)f->node->size + offset); break; /* SEEK_END */
    }
    return (int32_t)f->offset;
}

int32_t vfs_tell(int32_t fd) {
    if (!fd_valid(fd)) return -1;
    return (int32_t)fd_table[fd].offset;
}

int32_t vfs_stat(const char* path, vfs_stat_t* st) {
    vfs_node_t* n = vfs_resolve(path);
    if (!n || !st) return -1;
    st->size  = n->size;
    st->type  = n->type;
    st->inode = n->inode;
    return 0;
}

bool vfs_exists(const char* path) {
    return vfs_resolve(path) != (vfs_node_t*)0;
}

int32_t vfs_mkdir(const char* path) {
    /* Разбиваем путь на родитель + имя */
    char parent_path[VFS_PATH_MAX];
    strncpy(parent_path, path, VFS_PATH_MAX - 1);
    char* last = (char*)0;
    for (char* c = parent_path; *c; c++) if (*c == '/') last = c;

    const char* dname  = path;
    vfs_node_t* parent = vfs_root;
    if (last) { *last = '\0'; dname = last + 1; parent = vfs_resolve(parent_path); }
    if (!parent) return -1;
    if (parent->ops && parent->ops->create)
        return parent->ops->create(parent, dname, VFS_DIRECTORY);
    return -1;
}

vfs_dirent_t vfs_readdir(const char* path, uint32_t index) {
    vfs_dirent_t de;
    memset(&de, 0, sizeof(de));
    vfs_node_t* dir = vfs_resolve(path);
    if (!dir || dir->type != VFS_DIRECTORY) return de;
    if (index >= dir->child_count) return de;
    vfs_node_t* child = dir->children[index];
    strncpy(de.name, child->name, VFS_NAME_MAX - 1);
    de.inode = child->inode;
    de.size  = child->size;
    de.type  = child->type;
    return de;
}

int32_t vfs_unlink(const char* path) {
    vfs_node_t* node = vfs_resolve(path);
    if (!node || !node->parent) return -1;
    vfs_node_t* parent = node->parent;
    if (parent->ops && parent->ops->unlink)
        return parent->ops->unlink(parent, node->name);
    return -1;
}

/* Прямой доступ к ноде */
int32_t vfs_node_read(vfs_node_t* n, uint32_t off, uint32_t sz, uint8_t* buf) {
    if (!n || !buf) return -1;
    if (n->ops && n->ops->read) return n->ops->read(n, off, sz, buf);
    /* Fallback: прямое чтение из data буфера */
    if (!n->data) return 0;
    if (off >= n->size) return 0;
    uint32_t avail = n->size - off;
    uint32_t count = (sz < avail) ? sz : avail;
    memcpy(buf, n->data + off, count);
    return (int32_t)count;
}

int32_t vfs_node_write(vfs_node_t* n, uint32_t off, uint32_t sz, const uint8_t* buf) {
    if (!n || !buf) return -1;
    if (n->ops && n->ops->write) return n->ops->write(n, off, sz, buf);
    return -1;
}

vfs_node_t* vfs_get_node(int32_t fd) {
    if (!fd_valid(fd)) return (vfs_node_t*)0;
    return fd_table[fd].node;
}

vfs_node_t* vfs_get_cwd(void) { return vfs_cwd; }
void vfs_set_cwd(vfs_node_t* dir) { if (dir) vfs_cwd = dir; }
