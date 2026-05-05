
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS {
    /*
     * Завантажуємо з адреси 1 МБ (0x100000).
     * Нижче: BIOS (0x0-0x500), IVT, BDA, EBDA, VGA (0xA0000-0xBFFFF)
     */
    . = 1M;

    /* Multiboot2 заголовок повинен бути у перших 32 КБ бінарника */
    .multiboot_header : {
        KEEP(*(.multiboot_header))
    }

    /* Код */
    .text ALIGN(4K) : {
        *(.text .text.*)
    }

    /* Константи (read-only) */
    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    /* Ініціалізовані дані */
    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    /* Неініціалізовані данні */
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.hash)
        *(.hash)
        *(.rel.*)
        *(.rela.*)
        *(.plt)
        *(.got)
        *(.got.plt)
        *(.interp)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note.GNU-stack)
        *(.note.gnu.build-id)
    }

    kernel_end = .;
}
