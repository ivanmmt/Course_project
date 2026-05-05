; =============================================================================
; kernel/isr.asm
; Точки входа для всех прерываний (ISR/IRQ стабы)
;
; ЗАЧЕМ ЭТО НУЖНО:
; Когда происходит прерывание, процессор автоматически сохраняет EIP, CS,
; EFLAGS (и ESP, SS при смене кольца) на стек. Но он НЕ сохраняет остальные
; регистры (EAX, EBX, ...) — это должны делать мы.
;
; Также некоторые исключения имеют "код ошибки" (error code), а другие нет.
; Чтобы C-функция получала одинаковую структуру registers_t, мы
; искусственно добавляем нулевой error code для тех что его не имеют.
;
; Схема каждого ISR:
;   1. [Опционально] push 0 (фейковый error code)
;   2. push номер_прерывания
;   3. jmp isr_common_stub
;
; common_stub:
;   1. Сохраняет DS
;   2. Переключается на сегмент данных ядра
;   3. pusha (сохраняет все регистры)
;   4. Вызывает C-обработчик
;   5. popa, восстанавливает DS
;   6. iret (возврат из прерывания)
; =============================================================================

bits 32

; Экспортируем все стабы чтобы idt.c мог их зарегистрировать
global isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
global isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
global irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
global irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
global idt_flush

; Внешние C-обработчики
extern isr_handler
extern irq_handler

; =============================================================================
; Макросы для генерации ISR стабов
; Это удобнее чем писать каждый стаб вручную
; =============================================================================

; ISR без кода ошибки (большинство исключений)
%macro ISR_NO_ERR 1
isr%1:
    push byte 0         ; фейковый error code — для единообразия структуры
    push byte %1        ; номер прерывания
    jmp isr_common_stub
%endmacro

; ISR с кодом ошибки (процессор сам кладёт error code на стек)
%macro ISR_ERR 1
isr%1:
    ; error code уже на стеке — процессор положил автоматически
    push byte %1
    jmp isr_common_stub
%endmacro

; IRQ стаб (аппаратное прерывание)
%macro IRQ 2
irq%1:
    push byte 0         ; нет error code
    push byte %2        ; номер вектора (32 + номер IRQ)
    jmp irq_common_stub
%endmacro

; =============================================================================
; Генерируем все 32 ISR
; Исключения 8, 10-14, 17 имеют код ошибки (CPU кладёт его на стек сам)
; =============================================================================

ISR_NO_ERR  0   ; Division By Zero
ISR_NO_ERR  1   ; Debug
ISR_NO_ERR  2   ; NMI
ISR_NO_ERR  3   ; Breakpoint
ISR_NO_ERR  4   ; Overflow
ISR_NO_ERR  5   ; Bound Range Exceeded
ISR_NO_ERR  6   ; Invalid Opcode
ISR_NO_ERR  7   ; Device Not Available
ISR_ERR     8   ; Double Fault (error code всегда 0)
ISR_NO_ERR  9   ; Coprocessor Segment Overrun
ISR_ERR     10  ; Invalid TSS
ISR_ERR     11  ; Segment Not Present
ISR_ERR     12  ; Stack-Segment Fault
ISR_ERR     13  ; General Protection Fault
ISR_ERR     14  ; Page Fault
ISR_NO_ERR  15  ; Reserved
ISR_NO_ERR  16  ; x87 FP Exception
ISR_ERR     17  ; Alignment Check
ISR_NO_ERR  18  ; Machine Check
ISR_NO_ERR  19  ; SIMD FP Exception
ISR_NO_ERR  20  ; Virtualization Exception
ISR_NO_ERR  21
ISR_NO_ERR  22
ISR_NO_ERR  23
ISR_NO_ERR  24
ISR_NO_ERR  25
ISR_NO_ERR  26
ISR_NO_ERR  27
ISR_NO_ERR  28
ISR_NO_ERR  29
ISR_NO_ERR  30
ISR_NO_ERR  31

; =============================================================================
; Генерируем 16 IRQ обработчиков (вектора 32-47)
; =============================================================================
IRQ  0, 32   ; Timer (PIT)
IRQ  1, 33   ; Keyboard
IRQ  2, 34   ; Cascade (slave PIC)
IRQ  3, 35   ; COM2
IRQ  4, 36   ; COM1
IRQ  5, 37   ; LPT2
IRQ  6, 38   ; Floppy
IRQ  7, 39   ; LPT1
IRQ  8, 40   ; RTC
IRQ  9, 41   ; Free
IRQ 10, 42   ; Free
IRQ 11, 43   ; Free
IRQ 12, 44   ; PS/2 Mouse
IRQ 13, 45   ; FPU
IRQ 14, 46   ; Primary ATA
IRQ 15, 47   ; Secondary ATA

; =============================================================================
; Общий стаб для исключений CPU
; На этот момент стек содержит (снизу вверх):
; [SS, ESP, EFLAGS, CS, EIP] — сохранено процессором
; [err_code, int_no] — наш push
; =============================================================================
isr_common_stub:
    ; Сохраняем сегментный регистр DS
    push ds

    ; Сохраняем все регистры общего назначения одной инструкцией
    ; (EAX, ECX, EDX, EBX, исходный ESP, EBP, ESI, EDI — в таком порядке)
    pusha

    ; Переключаемся на сегмент данных ядра (на случай если пришли из ring 3)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Передаём указатель на регистры (ESP сейчас указывает на наш registers_t)
    push esp
    call isr_handler        ; вызываем C-обработчик
    add esp, 4              ; убираем аргумент

    ; Восстанавливаем регистры и возвращаемся
    popa
    pop ds
    add esp, 8              ; убираем err_code и int_no с нашего push

    iret                    ; восстанавливает EIP, CS, EFLAGS (и ESP, SS если было)

; =============================================================================
; Общий стаб для аппаратных прерываний IRQ
; =============================================================================
irq_common_stub:
    push ds
    pusha

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    popa
    pop ds
    add esp, 8

    iret

; =============================================================================
; Загрузка IDT
; =============================================================================
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; =============================================================================
; Секция .note.GNU-stack — указываем линкеру что стек НЕ исполняемый
; Без этой секции i686-linux-gnu-ld выдаёт предупреждение:
;   "missing .note.GNU-stack section implies executable stack"
; и создаёт PT_GNU_STACK с правами RWX, что ломает загрузку GRUB
; =============================================================================
section .note.GNU-stack noalloc noexec nowrite progbits
