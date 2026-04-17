; Link on official GRUB multiboot specification: https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
; GitLab : https://gitlab.freedesktop.org/gnu-grub/grub
; This part of code is used to load kernnel from GRUB
; For programming this asm part Im use this github repository : https://github.com/thedenisnikulin/os-project.git

; Use 32 bits protected mode
bits 32

; Give some constants
MULTIBOOT2_MAGIC equ 0xE85250D6
MULTIBOOT_ARCH_X86  equ 0
HEADER_LENGTH equ (multiboot_header_end - multiboot_header)
CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT_ARCH_X86 + HEADER_LENGTH)

; Multiboot header
section .multiboot_header
algin 8
multiboot_header:
	dd MULTIBOOT2_MAGIC
	dd MULTIBOOT_ARCH_X86
	dd HEADER_LENGTH
	dd CHECKSUM

	algin 8
	dw 5
	dw0
	dd 20
	dd 1280
	dd 1024
	dd 32

	algin 8
	dw 0
	dw 0
	dd 8
multiboot_header_end:

section .bss
algin 16
stack_bottom:
	resb 16384 ; 16KB stack
stack_top:

section .text
global _start
extern kernel_main

_start:
	; Set up the stack
	mov esp, stack_top
	push ebx
	push eax
	; Call the kernel main function
	call kernel_main
.hang:
	cli
	hlt
	jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits