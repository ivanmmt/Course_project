
bits 32

MULTIBOOT2_MAGIC   equ 0xE85250D6
MULTIBOOT_ARCH_X86 equ 0
HEADER_LENGTH      equ (multiboot_header_end - multiboot_header)
CHECKSUM           equ -(MULTIBOOT2_MAGIC + MULTIBOOT_ARCH_X86 + HEADER_LENGTH)

section .multiboot_header
align 8
multiboot_header:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT_ARCH_X86
    dd HEADER_LENGTH
    dd CHECKSUM

    ; Тип 5: framebuffer tag
    ; Говорим GRUB что хотим линейный framebuffer с нужными параметрами.
    ; width=0, height=0, depth=0 означает "любой подходящий".
    ; GRUB подберёт лучший доступный режим.
    align 8
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32

    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:


section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov  esp, stack_top
    push ebx
    push eax
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
