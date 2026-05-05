/* =============================================================================
 * filesystem/pe/pe.c
 * PE32 загрузчик — загрузка Windows .exe в память MyOS
 * ============================================================================= */

#include "pe.h"
#include "../vfs.h"
#include "../../memory/heap.h"
#include "../../memory/vmm.h"
#include "../../memory/pmm.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"
#include "../../kernel/kernel.h"

/* =============================================================================
 * pe_validate — проверить магические числа и тип PE
 * ============================================================================= */
bool pe_validate(const uint8_t* data, uint32_t size) {
    if (size < sizeof(pe_dos_header_t) + 4) return false;

    const pe_dos_header_t* dos = (const pe_dos_header_t*)data;

    /* Магия DOS: "MZ" */
    if (dos->magic != 0x5A4D) return false;

    /* Проверяем смещение PE заголовка */
    uint32_t pe_off = dos->pe_offset;
    if (pe_off + sizeof(pe_header_t) > size) return false;

    const pe_header_t* pe = (const pe_header_t*)(data + pe_off);

    /* Магия PE: "PE\0\0" */
    if (pe->signature != 0x00004550) return false;

    /* Только i386 */
    if (pe->coff.machine != PE_MACHINE_I386) {
        vga_put_string("  [PE] Only i386 (0x014C) supported\n");
        return false;
    }

    /* PE32 (не PE32+) */
    if (pe->opt.magic != 0x010B) {
        vga_put_string("  [PE] Only PE32 (not PE32+) supported\n");
        return false;
    }

    /* Должен быть исполняемым, не DLL */
    if (!(pe->coff.characteristics & PE_CHAR_EXECUTABLE)) return false;
    if (pe->coff.characteristics & PE_CHAR_DLL) {
        vga_put_string("  [PE] DLL loading not supported\n");
        return false;
    }

    return true;
}

/* =============================================================================
 * pe_load — загрузить PE образ в память
 *
 * АЛГОРИТМ:
 * 1. Читаем DOS + PE заголовки
 * 2. Выбираем базовый адрес (image_base из заголовка или 0x400000)
 * 3. Выделяем непрерывный диапазон памяти через VMM (image_size байт)
 * 4. Копируем заголовки
 * 5. Для каждой секции: копируем raw_size байт из файла в virtual_address
 * 6. BSS секции (.bss): обнуляем
 * 7. Применяем релокации если base изменился
 * 8. Возвращаем entry = image_base + entry_point_rva
 * ============================================================================= */
pe_info_t pe_load(const uint8_t* data, uint32_t size) {
    pe_info_t info;
    memset(&info, 0, sizeof(info));
    info.valid = false;

    if (!pe_validate(data, size)) return info;

    const pe_dos_header_t* dos = (const pe_dos_header_t*)data;
    const pe_header_t*     pe  = (const pe_header_t*)(data + dos->pe_offset);

    uint32_t preferred_base = pe->opt.image_base;
    uint32_t image_size     = pe->opt.image_size;
    uint32_t num_sections   = pe->coff.num_sections;

    /* Выбираем базовый адрес загрузки.
     * Используем preferred_base если она не занята ядром.
     * Ядро занимает 0x100000..kernel_end (~4-8 МБ).
     * preferred_base обычно 0x400000 — должно быть свободно. */
    uint32_t load_base = preferred_base;

    /* Простая проверка: если базовый адрес в зоне ядра — сдвигаем */
    if (load_base < 0x400000) load_base = 0x400000;

    /* Выделяем физические страницы и маппируем под PE образ */
    uint32_t pages = ALIGN_UP(image_size, 4096) / 4096;
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t virt = load_base + p * 4096;
        uint32_t phys = pmm_alloc_page();
        if (phys == PMM_ALLOC_ERROR) {
            vga_put_string("  [PE] Out of memory\n");
            return info;
        }
        vmm_map_page(virt, phys, VMM_KERNEL_FLAGS);
        memset((void*)virt, 0, 4096);
    }

    /* Копируем заголовки */
    uint32_t headers_size = pe->opt.headers_size;
    if (headers_size > size) headers_size = size;
    memcpy((void*)load_base, data, headers_size);

    /* Адрес таблицы секций (сразу после Optional Header) */
    const pe_section_header_t* sections =
        (const pe_section_header_t*)(
            (const uint8_t*)(data + dos->pe_offset)
            + 4                              /* signature */
            + sizeof(pe_coff_header_t)
            + pe->coff.optional_header_size
        );

    /* Загружаем секции */
    for (uint16_t i = 0; i < num_sections; i++) {
        const pe_section_header_t* s = &sections[i];
        uint32_t dest  = load_base + s->virtual_address;
        uint32_t vsz   = s->virtual_size;
        uint32_t rawsz = s->raw_size;
        uint32_t rawoff= s->raw_offset;

        if (dest + vsz > load_base + image_size) continue;

        if (rawsz > 0 && rawoff + rawsz <= size) {
            /* Копируем данные из файла */
            uint32_t copy_sz = (rawsz < vsz) ? rawsz : vsz;
            memcpy((void*)dest, data + rawoff, copy_sz);
        }
        /* Остаток виртуального размера — нули (BSS) */
        if (vsz > rawsz) {
            memset((void*)(dest + rawsz), 0, vsz - rawsz);
        }
    }

    /* Применяем базовые релокации если load_base != preferred_base */
    int32_t delta = (int32_t)load_base - (int32_t)preferred_base;
    if (delta != 0) {
        /* Находим секцию .reloc через Data Directory [5] */
        uint32_t reloc_rva  = pe->opt.data_dir[PE_DIR_BASERELOC].rva;
        uint32_t reloc_size = pe->opt.data_dir[PE_DIR_BASERELOC].size;

        if (reloc_rva && reloc_size) {
            uint8_t* reloc_base = (uint8_t*)(load_base + reloc_rva);
            uint8_t* reloc_end  = reloc_base + reloc_size;

            while (reloc_base < reloc_end) {
                pe_basereloc_block_t* block = (pe_basereloc_block_t*)reloc_base;
                if (block->block_size < 8) break;

                uint16_t* entries = (uint16_t*)(reloc_base + 8);
                uint32_t  count   = (block->block_size - 8) / 2;

                for (uint32_t e = 0; e < count; e++) {
                    uint8_t  type   = (uint8_t)(entries[e] >> 12);
                    uint16_t offset = entries[e] & 0x0FFF;

                    if (type == PE_RELOC_HIGHLOW) {
                        uint32_t* patch = (uint32_t*)(
                            load_base + block->page_rva + offset);
                        *patch = (uint32_t)((int32_t)(*patch) + delta);
                    }
                }
                reloc_base += block->block_size;
            }
        }
    }

    /* Точка входа */
    info.valid      = true;
    info.entry      = load_base + pe->opt.entry_point;
    info.image_base = load_base;
    info.image_size = image_size;

    return info;
}

/* =============================================================================
 * pe_load_file — загрузить PE из VFS
 * ============================================================================= */
pe_info_t pe_load_file(const char* path) {
    pe_info_t info;
    memset(&info, 0, sizeof(info));

    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0 || st.size == 0) {
        vga_printf("  [PE] Not found: %s\n", path);
        return info;
    }

    uint8_t* buf = (uint8_t*)kmalloc(st.size);
    if (!buf) return info;

    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) { kfree(buf); return info; }

    int32_t n = vfs_read(fd, buf, st.size);
    vfs_close(fd);

    if (n > 0) info = pe_load(buf, (uint32_t)n);
    kfree(buf);
    return info;
}

/* =============================================================================
 * pe_exec — запустить загруженный PE
 *
 * Точка входа PE: int __stdcall WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
 * Для консольных: int main(int, char**)
 * Мы вызываем как обычную C функцию с argc/argv
 * ============================================================================= */
int pe_exec(pe_info_t* info, int argc, char** argv) {
    if (!info || !info->valid) return -1;

    typedef int (*pe_entry_t)(int, char**);
    pe_entry_t entry = (pe_entry_t)info->entry;

    vga_printf("  [PE] Entry=0x%x base=0x%x size=%u\n",
               info->entry, info->image_base, info->image_size);

    int ret = entry(argc, argv);
    vga_printf("  [PE] Exited: %d\n", ret);
    return ret;
}

/* =============================================================================
 * pe_dump_info — отладочный вывод
 * ============================================================================= */
void pe_dump_info(const uint8_t* data, uint32_t size) {
    if (!pe_validate(data, size)) {
        vga_put_string("  [PE] Not a valid PE32 executable\n");
        return;
    }

    const pe_dos_header_t* dos = (const pe_dos_header_t*)data;
    const pe_header_t*     pe  = (const pe_header_t*)(data + dos->pe_offset);

    vga_printf("  PE32 Header:\n");
    vga_printf("    Machine:   0x%x (%s)\n",
        pe->coff.machine,
        pe->coff.machine == PE_MACHINE_I386 ? "i386" : "other");
    vga_printf("    Sections:  %u\n",   pe->coff.num_sections);
    vga_printf("    ImageBase: 0x%x\n", pe->opt.image_base);
    vga_printf("    ImageSize: %u KB\n", pe->opt.image_size / 1024);
    vga_printf("    EntryRVA:  0x%x\n", pe->opt.entry_point);
    vga_printf("    Subsystem: %u (%s)\n",
        pe->opt.subsystem,
        pe->opt.subsystem == 2 ? "GUI" :
        pe->opt.subsystem == 3 ? "Console" : "Native");

    /* Секции */
    const pe_section_header_t* sections =
        (const pe_section_header_t*)(
            (const uint8_t*)(data + dos->pe_offset)
            + 4 + sizeof(pe_coff_header_t) + pe->coff.optional_header_size
        );

    vga_put_string("  Sections:\n");
    for (uint16_t i = 0; i < pe->coff.num_sections; i++) {
        const pe_section_header_t* s = &sections[i];
        char name[9]; memcpy(name, s->name, 8); name[8] = '\0';
        vga_printf("    %-8s  VA=0x%x  VSZ=%u  RAW=%u  flags=0x%x\n",
            name, s->virtual_address, s->virtual_size,
            s->raw_size, s->characteristics);
    }

    /* Релокации */
    if (pe->opt.data_dir[PE_DIR_BASERELOC].rva)
        vga_printf("  .reloc: RVA=0x%x size=%u\n",
            pe->opt.data_dir[PE_DIR_BASERELOC].rva,
            pe->opt.data_dir[PE_DIR_BASERELOC].size);
    else
        vga_put_string("  .reloc: none (fixed base)\n");
}
