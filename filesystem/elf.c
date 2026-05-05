/* =============================================================================
 * filesystem/elf.c
 * ELF32 загрузчик — читаем Program Headers, копируем сегменты в память
 * ============================================================================= */

#include "elf.h"
#include "vfs.h"
#include "../memory/heap.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "../lib/string.h"
#include "../drivers/vga/vga_text.h"
#include "../kernel/kernel.h"

/* =============================================================================
 * elf_validate — проверить магию и базовые поля
 * ============================================================================= */
bool elf_validate(const uint8_t* data, uint32_t size) {
    if (size < sizeof(elf32_header_t)) return false;

    const elf32_header_t* hdr = (const elf32_header_t*)data;

    /* Магическое число: 0x7F 'E' 'L' 'F' */
    if (*(uint32_t*)hdr->e_ident != ELF_MAGIC) return false;

    /* Только 32-бит ELF */
    if (hdr->e_ident[4] != ELF_CLASS_32) return false;

    /* Little-endian */
    if (hdr->e_ident[5] != ELF_DATA_LSB) return false;

    /* Должен быть исполняемым */
    if (hdr->e_type != ET_EXEC) return false;

    /* Архитектура x86 */
    if (hdr->e_machine != EM_386) return false;

    return true;
}

/* =============================================================================
 * elf_load — загрузить ELF сегменты в память
 *
 * Алгоритм:
 * 1. Проверяем заголовок
 * 2. Проходим по Program Headers
 * 3. Для каждого PT_LOAD: копируем данные из файла в p_vaddr
 * 4. Если p_memsz > p_filesz: обнуляем разницу (BSS)
 * 5. Возвращаем точку входа e_entry
 * ============================================================================= */
/* Forward declaration */
static void elf_apply_relocations(const uint8_t* data, uint32_t size,
                                   const elf_info_t* info);

elf_info_t elf_load(const uint8_t* data, uint32_t size) {
    elf_info_t info;
    memset(&info, 0, sizeof(info));
    info.valid = false;

    if (!elf_validate(data, size)) {
        vga_put_string("  [ELF] Invalid ELF file\n");
        return info;
    }

    const elf32_header_t* hdr = (const elf32_header_t*)data;

    /* Сначала определяем диапазон адресов */
    uint32_t vaddr_min = 0xFFFFFFFF;
    uint32_t vaddr_max = 0;

    const elf32_phdr_t* phdrs = (const elf32_phdr_t*)(data + hdr->e_phoff);

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf32_phdr_t* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_vaddr < vaddr_min) vaddr_min = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > vaddr_max)
            vaddr_max = ph->p_vaddr + ph->p_memsz;
    }

    if (vaddr_max == 0) {
        vga_put_string("  [ELF] No loadable segments\n");
        return info;
    }

    /* Маппируем страницы для всех сегментов */
    uint32_t pages_needed =
        (ALIGN_UP(vaddr_max, 4096) - ALIGN_DOWN(vaddr_min, 4096)) / 4096;

    for (uint32_t p = 0; p < pages_needed; p++) {
        uint32_t virt  = ALIGN_DOWN(vaddr_min, 4096) + p * 4096;
        uint32_t phys  = pmm_alloc_page();
        if (phys == PMM_ALLOC_ERROR) {
            vga_put_string("  [ELF] Out of memory\n");
            return info;
        }
        vmm_map_page(virt, phys, VMM_KERNEL_FLAGS);
        /* Обнуляем страницу */
        memset((void*)virt, 0, 4096);
    }

    /* Загружаем каждый PT_LOAD сегмент */
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf32_phdr_t* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        /* Копируем данные из файла */
        if (ph->p_filesz > 0) {
            if (ph->p_offset + ph->p_filesz > size) {
                vga_put_string("  [ELF] Segment out of file bounds\n");
                return info;
            }
            memcpy((void*)ph->p_vaddr,
                   data + ph->p_offset,
                   ph->p_filesz);
        }

        /* BSS: обнуляем разницу между memsz и filesz */
        if (ph->p_memsz > ph->p_filesz) {
            memset((void*)(ph->p_vaddr + ph->p_filesz),
                   0,
                   ph->p_memsz - ph->p_filesz);
        }
    }

    info.entry     = hdr->e_entry;
    info.load_base = vaddr_min;
    info.load_end  = vaddr_max;
    info.valid     = true;

    /* Apply relocations if present */
    elf_apply_relocations(data, size, &info);

    return info;
}

/* =============================================================================
 * elf_load_file — загрузить ELF из VFS
 * ============================================================================= */
elf_info_t elf_load_file(const char* path) {
    elf_info_t info;
    memset(&info, 0, sizeof(info));

    /* Открываем файл */
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        vga_printf("  [ELF] Cannot open: %s\n", path);
        return info;
    }

    /* Узнаём размер */
    vfs_stat_t st;
    vfs_stat(path, &st);
    if (st.size == 0) {
        vga_put_string("  [ELF] Empty file\n");
        vfs_close(fd);
        return info;
    }

    /* Читаем в буфер */
    uint8_t* buf = (uint8_t*)kmalloc(st.size);
    if (!buf) {
        vga_put_string("  [ELF] kmalloc failed\n");
        vfs_close(fd);
        return info;
    }

    int32_t n = vfs_read(fd, buf, st.size);
    vfs_close(fd);

    if (n <= 0) {
        kfree(buf);
        vga_put_string("  [ELF] Read error\n");
        return info;
    }

    info = elf_load(buf, (uint32_t)n);
    kfree(buf);
    return info;
}

/* =============================================================================
 * elf_apply_relocations — обработать ELF relocations (R_386_*)
 *
 * Поддерживаемые типы:
 *   R_386_RELATIVE (8): reloc->r_offset += load_base
 *   R_386_32 (1):       reloc->r_offset += symbol_value
 *
 * Используется для position-independent code и dynamically linked binaries.
 * ============================================================================= */
static void elf_apply_relocations(const uint8_t* data, uint32_t size,
                                   const elf_info_t* info) {
    const elf32_header_t* hdr = (const elf32_header_t*)data;
    if (hdr->e_shnum == 0) return;

    const elf32_shdr_t* shdrs =
        (const elf32_shdr_t*)(data + hdr->e_shoff);

    /* Найти секции .rel.* и .rela.* */
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf32_shdr_t* shdr = &shdrs[i];

        /* SHT_REL = 9, SHT_RELA = 4 */
        if (shdr->sh_type != 9) continue;  /* только SHT_REL */
        if (shdr->sh_size == 0) continue;

        uint32_t num_relocs = shdr->sh_size / shdr->sh_entsize;

        /* Читаем relocation записи */
        typedef struct {
            uint32_t r_offset;
            uint32_t r_info;
        } __attribute__((packed)) elf32_rel_t;

        const elf32_rel_t* relocs = (const elf32_rel_t*)(data + shdr->sh_offset);

        for (uint32_t j = 0; j < num_relocs; j++) {
            const elf32_rel_t* rel = &relocs[j];

            uint32_t offset = rel->r_offset;
            uint32_t type = rel->r_info & 0xFF;
            uint32_t sym = rel->r_info >> 8;

            /* Применяем relocation */
            uint32_t* target = (uint32_t*)(offset);

            switch (type) {
                case 8: /* R_386_RELATIVE */
                    *target += info->load_base;
                    break;

                case 1: /* R_386_32 */
                    /* Упрощено: просто добавляем load_base
                     * В реальной системе здесь нужна таблица символов */
                    *target += info->load_base;
                    break;

                default:
                    /* Игнорируем неизвестные типы */
                    break;
            }
        }
    }
}

/* =============================================================================
 * elf_exec — прыгнуть на точку входа ELF
 *
 * ВАЖНО: сейчас работает в пространстве ядра (нет разделения ring0/ring3).
 * В будущих шагах добавим переключение на ring3 через iret.
 * ============================================================================= */
int elf_exec(elf_info_t* info, int argc, char** argv) {
    if (!info || !info->valid) return -1;

    elf_entry_t entry = (elf_entry_t)info->entry;

    vga_printf("  [ELF] Jumping to entry=0x%x argc=%d\n",
               info->entry, argc);

    /* Прыгаем на точку входа */
    int ret = entry(argc, argv);

    vga_printf("  [ELF] Process returned %d\n", ret);
    return ret;
}

/* =============================================================================
 * elf_dump_info — отладочный вывод ELF заголовков
 * ============================================================================= */
void elf_dump_info(const uint8_t* data, uint32_t size) {
    if (!elf_validate(data, size)) {
        vga_put_string("  [ELF] Not a valid ELF32 executable\n");
        return;
    }

    const elf32_header_t* hdr = (const elf32_header_t*)data;

    vga_printf("  ELF Header:\n");
    vga_printf("    Type:    %u (2=EXEC)\n", hdr->e_type);
    vga_printf("    Machine: %u (3=i386)\n", hdr->e_machine);
    vga_printf("    Entry:   0x%x\n", hdr->e_entry);
    vga_printf("    PHnum:   %u\n", hdr->e_phnum);

    const elf32_phdr_t* phdrs = (const elf32_phdr_t*)(data + hdr->e_phoff);
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf32_phdr_t* ph = &phdrs[i];
        if (ph->p_type == PT_LOAD) {
            vga_printf("    LOAD: vaddr=0x%x filesz=%u memsz=%u flags=%c%c%c\n",
                ph->p_vaddr, ph->p_filesz, ph->p_memsz,
                (ph->p_flags & PF_R) ? 'R' : '-',
                (ph->p_flags & PF_W) ? 'W' : '-',
                (ph->p_flags & PF_X) ? 'X' : '-');
        }
    }
}
