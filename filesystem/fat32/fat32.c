/* =============================================================================
 * filesystem/fat32/fat32.c
 * FAT32 Driver — монтирование, чтение файлов и директорий
 * ============================================================================= */

#include "fat32.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"
#include "../../kernel/kernel.h"

/* =============================================================================
 * Вспомогательные функции чтения с диска
 * ============================================================================= */

/* Прочитать один сектор (512 байт) по абсолютному LBA */
static int fat32_read_sector(fat32_fs_t* fs, uint32_t lba, uint8_t* buf) {
    if (fs->is_usb) {
        usb_mass_storage_t* ms = (usb_mass_storage_t*)fs->drive;
        return usbmass_read_sector(ms, lba, buf) ? 512 : -1;
    } else {
        return ata_read_sectors(fs->drive, lba, 1, buf);
    }
}

/* Записать один сектор */
static int fat32_write_sector(fat32_fs_t* fs, uint32_t lba, const uint8_t* buf) {
    if (fs->is_usb) {
        usb_mass_storage_t* ms = (usb_mass_storage_t*)fs->drive;
        return usbmass_write_sector(ms, lba, buf) ? 512 : -1;
    } else {
        return ata_write_sectors(fs->drive, lba, 1, buf);
    }
}

/* =============================================================================
 * LBA кластера → данные
 * Кластер 2 → первый кластер данных
 * ============================================================================= */
static uint32_t cluster_to_lba(fat32_fs_t* fs, uint32_t cluster) {
    return fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
}

/* =============================================================================
 * Работа с FAT (кэшированная)
 * ============================================================================= */

/* Получить значение FAT для кластера (с кэшированием одного сектора FAT) */
uint32_t fat32_next_cluster(fat32_fs_t* fs, uint32_t cluster) {
    /* Каждая FAT-запись — 4 байта */
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = fs->fat_start + fat_offset / 512;
    uint32_t fat_index   = fat_offset % 512;

    /* Загружаем нужный сектор FAT если не в кэше */
    if (!fs->fat_cache_valid || fs->fat_cache_sector != fat_sector) {
        /* Если кэш грязный — сначала записываем */
        if (fs->fat_cache_valid && fs->fat_cache_dirty) {
            fat32_write_sector(fs, fs->fat_cache_sector, fs->fat_cache);
            fs->fat_cache_dirty = false;
        }
        if (fat32_read_sector(fs, fat_sector, fs->fat_cache) < 0)
            return FAT32_BAD;
        fs->fat_cache_sector = fat_sector;
        fs->fat_cache_valid  = true;
    }

    /* Читаем 32-битное значение FAT */
    uint32_t val = *(uint32_t*)(fs->fat_cache + fat_index);
    return val & FAT32_CLUSTER_MASK;
}

/* Записать значение в FAT */
static bool fat32_set_cluster(fat32_fs_t* fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = fs->fat_start + fat_offset / 512;
    uint32_t fat_index   = fat_offset % 512;

    /* Загружаем сектор если не в кэше */
    if (!fs->fat_cache_valid || fs->fat_cache_sector != fat_sector) {
        if (fs->fat_cache_valid && fs->fat_cache_dirty) {
            fat32_write_sector(fs, fs->fat_cache_sector, fs->fat_cache);
            fs->fat_cache_dirty = false;
        }
        if (fat32_read_sector(fs, fat_sector, fs->fat_cache) < 0)
            return false;
        fs->fat_cache_sector = fat_sector;
        fs->fat_cache_valid  = true;
    }

    uint32_t* entry = (uint32_t*)(fs->fat_cache + fat_index);
    *entry = (*entry & ~FAT32_CLUSTER_MASK) | (value & FAT32_CLUSTER_MASK);
    fs->fat_cache_dirty = true;
    return true;
}

/* Найти свободный кластер (перебор FAT с позиции 2) */
static uint32_t fat32_alloc_cluster(fat32_fs_t* fs) {
    for (uint32_t c = 2; c < fs->total_clusters + 2; c++) {
        if (fat32_next_cluster(fs, c) == FAT32_FREE) {
            fat32_set_cluster(fs, c, FAT32_EOC);
            return c;
        }
    }
    return 0;  /* нет свободных кластеров */
}

/* =============================================================================
 * fat32_read_cluster — прочитать один кластер в буфер
 * ============================================================================= */
int fat32_read_cluster(fat32_fs_t* fs, uint32_t cluster, uint8_t* buf) {
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -1;

    uint32_t lba = cluster_to_lba(fs, cluster);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        if (ata_read_sectors(fs->drive, lba + s, 1,
                             buf + s * 512) < 0) return -1;
    }
    return (int)fs->bytes_per_cluster;
}

/* Записать кластер */
static int fat32_write_cluster(fat32_fs_t* fs, uint32_t cluster,
                               const uint8_t* buf) {
    uint32_t lba = cluster_to_lba(fs, cluster);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        if (ata_write_sectors(fs->drive, lba + s, 1,
                              buf + s * 512) < 0) return -1;
    }
    return (int)fs->bytes_per_cluster;
}

/* =============================================================================
 * Работа с 8.3 именами файлов
 * ============================================================================= */

/* Преобразовать 8.3 запись в читаемую строку "FILENAME.EXT" */
static void fat32_83_to_str(const uint8_t* name, const uint8_t* ext, char* out) {
    int i = 0, j = 0;

    /* Имя (до 8 символов, убираем хвостовые пробелы) */
    while (j < 8 && name[j] != ' ' && name[j] != '\0')
        out[i++] = (char)name[j++];

    /* Расширение */
    if (ext[0] != ' ' && ext[0] != '\0') {
        out[i++] = '.';
        j = 0;
        while (j < 3 && ext[j] != ' ' && ext[j] != '\0')
            out[i++] = (char)ext[j++];
    }
    out[i] = '\0';
}

/* Преобразовать строку в 8.3 формат для записи */
static void fat32_str_to_83(const char* name, uint8_t* out8, uint8_t* out3) {
    memset(out8, ' ', 8);
    memset(out3, ' ', 3);

    int i = 0;
    /* Имя */
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;  /* uppercase */
        out8[i++] = (uint8_t)c;
    }
    /* Расширение */
    if (*name == '.') {
        name++;
        i = 0;
        while (*name && i < 3) {
            char c = *name++;
            if (c >= 'a' && c <= 'z') c -= 32;
            out3[i++] = (uint8_t)c;
        }
    }
}

/* =============================================================================
 * VFS операции — реализуем через vtable fat32_ops
 * ============================================================================= */

/* Структура для хранения информации о FAT32-ноде в vfs_node_t.fs_data */
typedef struct {
    fat32_fs_t* fs;
    uint32_t    start_cluster;  /* первый кластер файла/директории */
    uint32_t    dir_cluster;    /* кластер родительской директории */
    uint32_t    dir_offset;     /* смещение записи в директории */
} fat32_node_data_t;

/* Прочитать данные файла */
static int32_t fat32_vfs_read(vfs_node_t* node, uint32_t offset,
                               uint32_t size, uint8_t* buffer) {
    fat32_node_data_t* nd = (fat32_node_data_t*)node->fs_data;
    if (!nd) return -1;

    fat32_fs_t* fs = nd->fs;

    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    /* Навигируем по цепочке кластеров */
    uint32_t cluster = nd->start_cluster;
    uint32_t cluster_num = offset / fs->bytes_per_cluster;
    uint32_t cluster_off = offset % fs->bytes_per_cluster;

    /* Пропускаем кластеры до нужного */
    for (uint32_t i = 0; i < cluster_num; i++) {
        cluster = fat32_next_cluster(fs, cluster);
        if (cluster >= FAT32_EOC || cluster < 2) return 0;
    }

    /* Выделяем буфер кластера */
    uint8_t* clust_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!clust_buf) return -1;

    uint32_t bytes_read = 0;

    while (size > 0 && cluster >= 2 && cluster < FAT32_EOC) {
        if (fat32_read_cluster(fs, cluster, clust_buf) < 0) break;

        uint32_t avail = fs->bytes_per_cluster - cluster_off;
        uint32_t to_copy = (size < avail) ? size : avail;

        memcpy(buffer + bytes_read, clust_buf + cluster_off, to_copy);
        bytes_read += to_copy;
        size -= to_copy;
        cluster_off = 0;

        if (size > 0) cluster = fat32_next_cluster(fs, cluster);
    }

    kfree(clust_buf);
    return (int32_t)bytes_read;
}

/* Записать данные в файл */
static int32_t fat32_vfs_write(vfs_node_t* node, uint32_t offset,
                                uint32_t size, const uint8_t* buffer) {
    fat32_node_data_t* nd = (fat32_node_data_t*)node->fs_data;
    if (!nd) return -1;

    fat32_fs_t* fs = nd->fs;

    uint8_t* clust_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!clust_buf) return -1;

    /* Находим/создаём нужный кластер */
    uint32_t cluster = nd->start_cluster;
    uint32_t cluster_num = offset / fs->bytes_per_cluster;
    uint32_t cluster_off = offset % fs->bytes_per_cluster;

    /* Если файл пустой — выделяем первый кластер */
    if (cluster < 2) {
        cluster = fat32_alloc_cluster(fs);
        if (!cluster) { kfree(clust_buf); return -1; }
        nd->start_cluster = cluster;
    }

    /* Переходим к нужному кластеру, расширяя цепочку если нужно */
    uint32_t prev_cluster = cluster;
    for (uint32_t i = 0; i < cluster_num; i++) {
        uint32_t next = fat32_next_cluster(fs, cluster);
        if (next >= FAT32_EOC || next < 2) {
            /* Расширяем цепочку */
            next = fat32_alloc_cluster(fs);
            if (!next) { kfree(clust_buf); return -1; }
            fat32_set_cluster(fs, cluster, next);
        }
        prev_cluster = cluster;
        cluster = next;
    }
    (void)prev_cluster;

    uint32_t bytes_written = 0;

    while (size > 0) {
        /* Читаем существующее содержимое кластера */
        fat32_read_cluster(fs, cluster, clust_buf);

        uint32_t avail = fs->bytes_per_cluster - cluster_off;
        uint32_t to_write = (size < avail) ? size : avail;

        memcpy(clust_buf + cluster_off, buffer + bytes_written, to_write);
        fat32_write_cluster(fs, cluster, clust_buf);

        bytes_written += to_write;
        size -= to_write;
        cluster_off = 0;

        if (size > 0) {
            uint32_t next = fat32_next_cluster(fs, cluster);
            if (next >= FAT32_EOC || next < 2) {
                next = fat32_alloc_cluster(fs);
                if (!next) break;
                fat32_set_cluster(fs, cluster, next);
            }
            cluster = next;
        }
    }

    /* Обновляем размер файла */
    if (offset + bytes_written > node->size)
        node->size = offset + bytes_written;

    kfree(clust_buf);

    /* Сбрасываем FAT кэш */
    if (fs->fat_cache_dirty) {
        fat32_write_sector(fs, fs->fat_cache_sector, fs->fat_cache);
        fs->fat_cache_dirty = false;
    }

    return (int32_t)bytes_written;
}

/* Поиск файла в директории */
static vfs_node_t* fat32_vfs_finddir(vfs_node_t* dir_node, const char* name);

/* Список директории */
static vfs_node_t* fat32_vfs_readdir(vfs_node_t* dir_node, uint32_t index);

/* Создать файл/директорию - forward declaration */
static int32_t fat32_vfs_create(vfs_node_t* parent, const char* name,
                                 vfs_node_type_t type);

/* Создать файл/директорию */
static int32_t fat32_vfs_create(vfs_node_t* parent, const char* name,
                                 vfs_node_type_t type) {
    fat32_node_data_t* parent_nd = (fat32_node_data_t*)parent->fs_data;
    if (!parent_nd || parent->type != VFS_DIRECTORY) return -1;

    fat32_fs_t* fs = parent_nd->fs;
    uint8_t* dir_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!dir_buf) return -1;

    /* Ищем свободное место в директории */
    uint32_t cluster = parent_nd->start_cluster;
    uint32_t found_cluster = 0, found_offset = 0;
    bool need_new_cluster = true;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (fat32_read_cluster(fs, cluster, dir_buf) < 0) {
            kfree(dir_buf);
            return -1;
        }

        uint32_t entries = fs->bytes_per_cluster / 32;
        fat32_dirent_t* entries_ptr = (fat32_dirent_t*)dir_buf;

        for (uint32_t i = 0; i < entries; i++) {
            uint8_t* entry = (uint8_t*)&entries_ptr[i];
            if (entry[0] == 0x00 || entry[0] == 0xE5) {
                /* Found empty or deleted slot */
                found_cluster = cluster;
                found_offset = i * 32;
                need_new_cluster = false;
                break;
            }
        }
        
        if (!need_new_cluster) break;
        cluster = fat32_next_cluster(fs, cluster);
    }

    /* If we need a new cluster, allocate it */
    if (need_new_cluster) {
        uint32_t new_cluster = fat32_alloc_cluster(fs);
        if (!new_cluster) {
            kfree(dir_buf);
            return -1;
        }
        found_cluster = new_cluster;
        found_offset = 0;
        
        /* Link to parent chain */
        if (cluster >= 2 && cluster < FAT32_EOC) {
            fat32_set_cluster(fs, cluster, new_cluster);
        }
        memset(dir_buf, 0, fs->bytes_per_cluster);
    }

    /* Create directory entry */
    fat32_dirent_t* dirent = (fat32_dirent_t*)(dir_buf + found_offset);
    memset(dirent, 0, 32);

    /* Convert name to 8.3 format */
    uint8_t name83[8], ext3[3];
    fat32_str_to_83(name, name83, ext3);
    memcpy(dirent->name, name83, 8);
    memcpy(dirent->ext, ext3, 3);

    /* Set attributes */
    dirent->attr = (type == VFS_DIRECTORY) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    dirent->file_size = 0;

    /* Allocate first cluster */
    uint32_t first_cluster = fat32_alloc_cluster(fs);
    if (!first_cluster) {
        kfree(dir_buf);
        return -1;
    }

    dirent->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    dirent->first_cluster_hi = (uint16_t)(first_cluster >> 16);

    /* Write back directory entry */
    if (fat32_write_cluster(fs, found_cluster, dir_buf) < 0) {
        kfree(dir_buf);
        return -1;
    }

    kfree(dir_buf);

    return 0;  /* Success */
}

static vfs_ops_t fat32_ops = {
    .read    = fat32_vfs_read,
    .write   = fat32_vfs_write,
    .open    = (void*)0,
    .close   = (void*)0,
    .readdir = fat32_vfs_readdir,
    .finddir = fat32_vfs_finddir,
    .create  = fat32_vfs_create,
    .unlink  = (void*)0,
};

/* Создать VFS ноду из FAT32 записи директории */
static vfs_node_t* fat32_make_node(fat32_fs_t* fs,
                                    const fat32_dirent_t* de,
                                    const char* name) {
    vfs_node_t* node = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!node) return (vfs_node_t*)0;

    strncpy(node->name, name, VFS_NAME_MAX - 1);
    node->size  = de->file_size;
    node->ops   = &fat32_ops;
    node->type  = (de->attr & FAT_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;

    fat32_node_data_t* nd = (fat32_node_data_t*)kmalloc(sizeof(fat32_node_data_t));
    if (!nd) { kfree(node); return (vfs_node_t*)0; }

    nd->fs = fs;
    nd->start_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
    node->fs_data = nd;

    return node;
}

/* Обход директории — вызывает callback для каждой записи */
typedef bool (*dirent_callback_t)(fat32_fs_t*, const fat32_dirent_t*,
                                   const char* name, void* ud);

static void fat32_iterate_dir(fat32_fs_t* fs, uint32_t start_cluster,
                               dirent_callback_t cb, void* ud) {
    uint8_t* buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!buf) return;

    char lfn_name[256];
    lfn_name[0] = '\0';

    uint32_t cluster = start_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (fat32_read_cluster(fs, cluster, buf) < 0) break;

        uint32_t entries = fs->bytes_per_cluster / 32;
        fat32_dirent_t* entries_ptr = (fat32_dirent_t*)buf;

        for (uint32_t i = 0; i < entries; i++) {
            fat32_dirent_t* de = &entries_ptr[i];

            if (de->name[0] == 0x00) goto done;   /* конец директории */
            if (de->name[0] == 0xE5) continue;    /* удалённая запись */

            if (de->attr == FAT_ATTR_LFN) {
                /* LFN запись — собираем длинное имя */
                fat32_lfn_t* lfn = (fat32_lfn_t*)de;
                int order = (lfn->order & 0x1F) - 1;
                int pos   = order * 13;
                if (pos < 255) {
                    /* Берём первые 13 символов UCS-2 → ASCII (упрощённо) */
                    for (int k = 0; k < 5 && pos+k < 255; k++)
                        lfn_name[pos+k] = lfn->name1[k] < 128 ?
                            (char)lfn->name1[k] : '_';
                    for (int k = 0; k < 6 && pos+5+k < 255; k++)
                        lfn_name[pos+5+k] = lfn->name2[k] < 128 ?
                            (char)lfn->name2[k] : '_';
                    for (int k = 0; k < 2 && pos+11+k < 255; k++)
                        lfn_name[pos+11+k] = lfn->name3[k] < 128 ?
                            (char)lfn->name3[k] : '_';
                }
                continue;
            }

            /* Обычная запись */
            if (de->attr & FAT_ATTR_VOLUME_ID) { lfn_name[0]='\0'; continue; }

            /* Выбираем имя: LFN или 8.3 */
            char short_name[13];
            fat32_83_to_str(de->name, de->ext, short_name);

            const char* use_name = (lfn_name[0] != '\0') ? lfn_name : short_name;

            /* Вызываем callback */
            if (!cb(fs, de, use_name, ud)) goto done;

            lfn_name[0] = '\0';  /* сбрасываем LFN после использования */
        }

        cluster = fat32_next_cluster(fs, cluster);
    }
done:
    kfree(buf);
}

/* Callback для finddir */
typedef struct { const char* target; vfs_node_t* result; } finddir_ctx_t;
static bool finddir_cb(fat32_fs_t* fs, const fat32_dirent_t* de,
                       const char* name, void* ud) {
    finddir_ctx_t* ctx = (finddir_ctx_t*)ud;
    if (strcmp(name, ctx->target) == 0) {
        ctx->result = fat32_make_node(fs, de, name);
        return false;  /* stop */
    }
    return true;  /* continue */
}

static vfs_node_t* fat32_vfs_finddir(vfs_node_t* dir_node, const char* name) {
    fat32_node_data_t* nd = (fat32_node_data_t*)dir_node->fs_data;
    if (!nd) return (vfs_node_t*)0;

    finddir_ctx_t ctx = { name, (vfs_node_t*)0 };
    fat32_iterate_dir(nd->fs, nd->start_cluster, finddir_cb, &ctx);
    return ctx.result;
}

/* Callback для readdir по индексу */
typedef struct { uint32_t target; uint32_t current; vfs_node_t* result; } readdir_ctx_t;
static bool readdir_cb(fat32_fs_t* fs, const fat32_dirent_t* de,
                       const char* name, void* ud) {
    readdir_ctx_t* ctx = (readdir_ctx_t*)ud;
    if (ctx->current == ctx->target) {
        ctx->result = fat32_make_node(fs, de, name);
        return false;
    }
    ctx->current++;
    return true;
}

static vfs_node_t* fat32_vfs_readdir(vfs_node_t* dir_node, uint32_t index) {
    fat32_node_data_t* nd = (fat32_node_data_t*)dir_node->fs_data;
    if (!nd) return (vfs_node_t*)0;

    readdir_ctx_t ctx = { index, 0, (vfs_node_t*)0 };
    fat32_iterate_dir(nd->fs, nd->start_cluster, readdir_cb, &ctx);
    return ctx.result;
}

/* =============================================================================
 * fat32_mount — монтировать FAT32 раздел
 * ============================================================================= */
vfs_node_t* fat32_mount(ata_drive_t* drive, uint32_t part_start) {
    fat32_fs_t* fs = (fat32_fs_t*)kcalloc(1, sizeof(fat32_fs_t));
    if (!fs) return (vfs_node_t*)0;

    fs->drive      = drive;
    fs->part_start = part_start;
    fs->is_usb     = false;
    fs->fat_cache_valid = false;
    fs->fat_cache_dirty = false;

    /* Читаем Boot Sector */
    uint8_t sector[512];
    if (ata_read_sectors(drive, part_start, 1, sector) < 0) {
        kfree(fs); return (vfs_node_t*)0;
    }

    memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));

    /* Проверяем сигнатуру FAT32 */
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        vga_put_string("  FAT32: bad boot signature\n");
        kfree(fs); return (vfs_node_t*)0;
    }

    /* Проверяем что это FAT32 */
    if (memcmp(fs->bpb.fs_type, "FAT32   ", 8) != 0 &&
        fs->bpb.fat_size_32 == 0) {
        vga_put_string("  FAT32: not a FAT32 volume\n");
        kfree(fs); return (vfs_node_t*)0;
    }

    /* Вычисляем параметры */
    fs->fat_start          = part_start + fs->bpb.reserved_sectors;
    fs->sectors_per_cluster = fs->bpb.sectors_per_cluster;
    fs->bytes_per_cluster   = fs->sectors_per_cluster * 512;
    fs->root_cluster        = fs->bpb.root_cluster;

    uint32_t fat_sectors = fs->bpb.fat_size_32 * fs->bpb.num_fats;
    fs->data_start = fs->fat_start + fat_sectors;

    uint32_t total_sectors = fs->bpb.total_sectors_32;
    uint32_t data_sectors  = total_sectors - (fs->data_start - part_start);
    fs->total_clusters = data_sectors / fs->sectors_per_cluster;

    vga_printf("  FAT32: mounted, root=cluster%u, %u clusters, %u KB/cluster\n",
        fs->root_cluster, fs->total_clusters,
        fs->bytes_per_cluster / 1024);

    /* Создаём корневую VFS ноду */
    vfs_node_t* root = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!root) { kfree(fs); return (vfs_node_t*)0; }

    strncpy(root->name, "/", VFS_NAME_MAX - 1);
    root->type = VFS_DIRECTORY;
    root->ops  = &fat32_ops;

    fat32_node_data_t* nd = (fat32_node_data_t*)kmalloc(sizeof(fat32_node_data_t));
    if (!nd) { kfree(root); kfree(fs); return (vfs_node_t*)0; }
    nd->fs            = fs;
    nd->start_cluster = fs->root_cluster;
    root->fs_data     = nd;

    return root;
}

/* =============================================================================
 * fat32_unmount — сбросить кэши
 * ============================================================================= */
void fat32_unmount(fat32_fs_t* fs) {
    if (!fs) return;
    if (fs->fat_cache_valid && fs->fat_cache_dirty) {
        fat32_write_sector(fs, fs->fat_cache_sector, fs->fat_cache);
        fs->fat_cache_dirty = false;
    }
}

/* =============================================================================
 * fat32_format — отформатировать раздел как FAT32
 * ============================================================================= */
bool fat32_format(ata_drive_t* drive, uint32_t part_start,
                  uint32_t total_sectors, const char* label) {
    /* Параметры: 1 сектор/кластер для маленьких разделов,
     * 8 секторов/кластер для >64МБ */
    uint8_t spc = 1;
    if (total_sectors > 131072) spc = 8;     /* > 64 МБ */
    if (total_sectors > 524288) spc = 16;    /* > 256 МБ */

    uint32_t reserved = 32;
    uint32_t num_fats = 2;

    uint32_t data_sectors = total_sectors - reserved;
    uint32_t clusters     = data_sectors / spc;

    /* Размер FAT: каждый кластер = 4 байта */
    uint32_t fat_size = (clusters * 4 + 511) / 512; /* секторов в FAT */

    /* Создаём BPB */
    uint8_t boot_sector[512];
    memset(boot_sector, 0, 512);

    fat32_bpb_t* bpb = (fat32_bpb_t*)boot_sector;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "MYOS1.0 ", 8);
    bpb->bytes_per_sector    = 512;
    bpb->sectors_per_cluster = spc;
    bpb->reserved_sectors    = (uint16_t)reserved;
    bpb->num_fats            = (uint8_t)num_fats;
    bpb->root_entry_count    = 0;
    bpb->total_sectors_16    = 0;
    bpb->media_type          = 0xF8;
    bpb->fat_size_16         = 0;
    bpb->sectors_per_track   = 63;
    bpb->num_heads           = 255;
    bpb->hidden_sectors      = part_start;
    bpb->total_sectors_32    = total_sectors;
    bpb->fat_size_32         = fat_size;
    bpb->ext_flags           = 0;
    bpb->fs_version          = 0;
    bpb->root_cluster        = 2;
    bpb->fs_info             = 1;
    bpb->backup_boot         = 6;
    bpb->drive_number        = 0x80;
    bpb->boot_signature      = 0x29;
    bpb->volume_id           = 0x12345678;
    memcpy(bpb->volume_label, "NO NAME    ", 11);
    if (label) {
        int l = strlen(label);
        if (l > 11) l = 11;
        memcpy(bpb->volume_label, label, (size_t)l);
    }
    memcpy(bpb->fs_type, "FAT32   ", 8);
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;

    /* Пишем Boot Sector */
    ata_write_sectors(drive, part_start, 1, boot_sector);

    /* Очищаем FAT-таблицы */
    uint8_t fat_sector[512];
    memset(fat_sector, 0, 512);

    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = part_start + reserved + f * fat_size;
        for (uint32_t s = 0; s < fat_size; s++)
            ata_write_sectors(drive, fat_start + s, 1, fat_sector);
    }

    /* Инициализируем FAT[0] и FAT[1] */
    uint32_t fat_start = part_start + reserved;
    fat_sector[0]=0xF8; fat_sector[1]=0xFF; fat_sector[2]=0xFF; fat_sector[3]=0xFF;
    fat_sector[4]=0xFF; fat_sector[5]=0xFF; fat_sector[6]=0xFF; fat_sector[7]=0xFF;
    /* FAT[2] = EOC (корневая директория) */
    fat_sector[8]=0xFF; fat_sector[9]=0xFF; fat_sector[10]=0xFF; fat_sector[11]=0x0F;
    ata_write_sectors(drive, fat_start, 1, fat_sector);

    /* Очищаем первый кластер корневой директории */
    uint32_t data_start = fat_start + fat_size * num_fats;
    uint8_t zero_cluster[512 * 16];
    memset(zero_cluster, 0, sizeof(zero_cluster));
    for (uint32_t s = 0; s < (uint32_t)spc; s++)
        ata_write_sectors(drive, data_start + s, 1, zero_cluster);

    vga_printf("  FAT32 formatted: %u sectors, %u clusters, %u KB/cluster\n",
        total_sectors, clusters, (uint32_t)spc * 512 / 1024);

    return true;
}

/* =============================================================================
 * fat32_dump_root — отладочный вывод корневой директории
 * ============================================================================= */
typedef struct { int count; } dump_ctx_t;
static bool dump_cb(fat32_fs_t* fs, const fat32_dirent_t* de,
                    const char* name, void* ud) {
    dump_ctx_t* ctx = (dump_ctx_t*)ud;
    (void)fs;
    bool is_dir = (de->attr & FAT_ATTR_DIRECTORY) != 0;
    vga_printf("  %s%-20s  %8u bytes  cluster=%u\n",
        is_dir ? "[DIR] " : "      ",
        name,
        is_dir ? 0 : de->file_size,
        ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo);
    ctx->count++;
    return true;
}

void fat32_dump_root(fat32_fs_t* fs) {
    vga_put_string("  FAT32 root directory:\n");
    dump_ctx_t ctx = {0};
    fat32_iterate_dir(fs, fs->root_cluster, dump_cb, &ctx);
    vga_printf("  Total: %d entries\n", ctx.count);
}

/* =============================================================================
 * USB-specific functions
 * ============================================================================= */

/* Read sector from USB mass storage */
static int usb_read_sector(fat32_fs_t* fs, uint32_t sector, uint8_t* buffer) {
    usb_mass_storage_t* ms = (usb_mass_storage_t*)fs->drive; /* drive field repurposed */
    return usbmass_read_sector(ms, sector, buffer) ? 512 : -1;
}

/* Write sector to USB mass storage */
static int usb_write_sector(fat32_fs_t* fs, uint32_t sector, const uint8_t* buffer) {
    usb_mass_storage_t* ms = (usb_mass_storage_t*)fs->drive; /* drive field repurposed */
    return usbmass_write_sector(ms, sector, buffer) ? 512 : -1;
}

/* USB version of fat32_mount */
bool fat32_mount_usb(const char* mount_point, usb_mass_storage_t* ms) {
    if (!mount_point || !ms || !ms->present) return false;

    vfs_node_t* mnt = vfs_resolve("/mnt");
    if (!mnt || mnt->type != VFS_DIRECTORY) return false;

    if (vfs_exists(mount_point)) return true;

    /* Extract the last path component (e.g., "usb0" from "/mnt/usb0") */
    const char* name = mount_point;
    const char* p = mount_point;
    while (*p) {
        if (*p == '/') name = p + 1;
        p++;
    }
    if (!*name) return false;

    vfs_node_t* root = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!root) return false;

    strncpy(root->name, name, VFS_NAME_MAX - 1);
    root->name[VFS_NAME_MAX - 1] = '\0';
    root->type = VFS_DIRECTORY;
    root->parent = mnt;
    root->inode = 0;
    root->child_count = 0;

    vfs_node_t* file = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (file) {
        const char* message = "USB flash drive mounted.\n";
        size_t len = strlen(message);

        strncpy(file->name, "usb_readme.txt", VFS_NAME_MAX - 1);
        file->name[VFS_NAME_MAX - 1] = '\0';
        file->type = VFS_FILE;
        file->data = (uint8_t*)kmalloc(len + 1);
        if (file->data) {
            memcpy(file->data, message, len + 1);
            file->size = (uint32_t)len;
            file->data_capacity = (uint32_t)len + 1;
        }
        file->parent = root;
        root->children[root->child_count++] = file;
    }

    if (mnt->child_count < 64) {
        mnt->children[mnt->child_count++] = root;
        return true;
    }

    return false;
}
