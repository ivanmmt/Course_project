/* =============================================================================
 * filesystem/ramfs.c
 * RAM Filesystem — реализация
 * ============================================================================= */

#include "ramfs.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/vga/vga_text.h"

static uint32_t ramfs_inode_counter = 100;

/* =============================================================================
 * Операции RamFS
 * ============================================================================= */

static int32_t ramfs_read(vfs_node_t* node, uint32_t offset,
                           uint32_t size, uint8_t* buffer) {
    if (!node->data || offset >= node->size) return 0;
    uint32_t avail = node->size - offset;
    uint32_t count = (size < avail) ? size : avail;
    memcpy(buffer, node->data + offset, count);
    return (int32_t)count;
}

static int32_t ramfs_write(vfs_node_t* node, uint32_t offset,
                            uint32_t size, const uint8_t* buffer) {
    uint32_t needed = offset + size;

    /* Расширяем буфер если нужно */
    if (needed > node->data_capacity) {
        /* Удваиваем с запасом */
        uint32_t new_cap = needed * 2;
        if (new_cap < 64) new_cap = 64;

        uint8_t* new_data = (uint8_t*)krealloc(node->data, new_cap);
        if (!new_data) return -1;

        /* Обнуляем новую область */
        if (new_cap > node->data_capacity)
            memset(new_data + node->data_capacity, 0,
                   new_cap - node->data_capacity);

        node->data          = new_data;
        node->data_capacity = new_cap;
    }

    memcpy(node->data + offset, buffer, size);
    if (offset + size > node->size)
        node->size = offset + size;

    return (int32_t)size;
}

static int32_t ramfs_open(vfs_node_t* node, uint32_t flags) {
    (void)node; (void)flags;
    return 0;
}

static void ramfs_close(vfs_node_t* node) {
    (void)node;
}

static vfs_node_t* ramfs_finddir(vfs_node_t* dir, const char* name) {
    for (uint32_t i = 0; i < dir->child_count; i++) {
        if (strcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    }
    return (vfs_node_t*)0;
}

static vfs_node_t* ramfs_readdir(vfs_node_t* dir, uint32_t index) {
    if (index >= dir->child_count) return (vfs_node_t*)0;
    return dir->children[index];
}

static int32_t ramfs_create(vfs_node_t* parent, const char* name,
                             vfs_node_type_t type) {
    if (parent->child_count >= 64) return -1;

    vfs_node_t* node = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!node) return -1;

    strncpy(node->name, name, VFS_NAME_MAX - 1);
    node->type   = type;
    node->inode  = ramfs_inode_counter++;
    node->parent = parent;
    node->ops    = parent->ops;  /* наследуем ops от родителя */

    if (type == VFS_FILE) {
        /* Минимальный буфер */
        node->data          = (uint8_t*)kmalloc(64);
        node->data_capacity = 64;
        node->size          = 0;
    }

    parent->children[parent->child_count++] = node;
    return 0;
}

static int32_t ramfs_unlink(vfs_node_t* parent, const char* name) {
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (strcmp(parent->children[i]->name, name) == 0) {
            vfs_node_t* node = parent->children[i];

            /* Освобождаем данные */
            if (node->data) kfree(node->data);
            kfree(node);

            /* Сдвигаем массив */
            for (uint32_t j = i; j < parent->child_count - 1; j++)
                parent->children[j] = parent->children[j + 1];
            parent->child_count--;
            return 0;
        }
    }
    return -1;
}

/* Таблица операций RamFS */
static vfs_ops_t ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .open    = ramfs_open,
    .close   = ramfs_close,
    .readdir = ramfs_readdir,
    .finddir = ramfs_finddir,
    .create  = ramfs_create,
    .unlink  = ramfs_unlink,
};

/* =============================================================================
 * ramfs_create_node — создать ноду (внутренняя функция)
 * ============================================================================= */
static vfs_node_t* make_node(const char* name, vfs_node_type_t type) {
    vfs_node_t* node = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!node) return (vfs_node_t*)0;
    strncpy(node->name, name, VFS_NAME_MAX - 1);
    node->type  = type;
    node->inode = ramfs_inode_counter++;
    node->ops   = &ramfs_ops;
    return node;
}

/* =============================================================================
 * Публичный API
 * ============================================================================= */

vfs_node_t* ramfs_init(void) {
    vfs_node_t* root = make_node("/", VFS_DIRECTORY);
    return root;
}

vfs_node_t* ramfs_create_file(vfs_node_t* parent,
                               const char* name,
                               const uint8_t* data,
                               uint32_t size) {
    if (!parent || parent->child_count >= 64) return (vfs_node_t*)0;

    vfs_node_t* node = make_node(name, VFS_FILE);
    if (!node) return (vfs_node_t*)0;

    if (data && size > 0) {
        node->data          = (uint8_t*)kmalloc(size + 1);
        node->data_capacity = size + 1;
        node->size          = size;
        memcpy(node->data, data, size);
        node->data[size] = '\0';  /* null-terminate для текстовых файлов */
    } else {
        node->data          = (uint8_t*)kmalloc(64);
        node->data_capacity = 64;
        node->size          = 0;
    }

    node->parent = parent;
    parent->children[parent->child_count++] = node;
    return node;
}

vfs_node_t* ramfs_create_dir(vfs_node_t* parent, const char* name) {
    if (!parent || parent->child_count >= 64) return (vfs_node_t*)0;

    vfs_node_t* node = make_node(name, VFS_DIRECTORY);
    if (!node) return (vfs_node_t*)0;

    node->parent = parent;
    parent->children[parent->child_count++] = node;
    return node;
}

/* =============================================================================
 * ramfs_dump — отладочный вывод дерева файлов
 * ============================================================================= */
void ramfs_dump(vfs_node_t* node, int depth) {
    if (!node) return;

    for (int i = 0; i < depth * 2; i++) vga_put_char(' ');

    if (node->type == VFS_DIRECTORY)
        vga_printf("[%s/]  (%u entries)\n", node->name, node->child_count);
    else
        vga_printf(" %s  (%u bytes)\n", node->name, node->size);

    for (uint32_t i = 0; i < node->child_count; i++)
        ramfs_dump(node->children[i], depth + 1);
}
