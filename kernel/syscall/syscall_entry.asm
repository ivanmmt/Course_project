; =============================================================================
; kernel/syscall/syscall_entry.asm
; INT 0x80 обработчик системных вызовов
;
; Linux x86 ABI:
;   EAX = номер syscall
;   EBX = arg1, ECX = arg2, EDX = arg3
;   Результат возвращается в EAX
; =============================================================================

bits 32
global syscall_entry
extern syscall_handler_c

syscall_entry:
    ; Сохраняем регистры на стек
    push    esi
    push    edi
    push    ebp
    push    edx             ; arg3
    push    ecx             ; arg2
    push    ebx             ; arg1
    push    eax             ; syscall number

    ; Выделяем место для возвращаемого значения
    sub     esp, 4          ; uint32_t ret = 0

    ; Передаём аргументы в C функцию:
    ; syscall_handler_c(num, arg1, arg2, arg3, &ret)
    lea     eax, [esp]      ; eax = &ret
    push    eax             ; arg5: &ret
    push    dword [esp+20]  ; arg4: edx (arg3)
    push    dword [esp+20]  ; arg3: ecx (arg2)
    push    dword [esp+20]  ; arg2: ebx (arg1)
    push    dword [esp+20]  ; arg1: syscall number

    call    syscall_handler_c
    add     esp, 20         ; убираем 5 аргументов

    ; Читаем возвращаемое значение
    pop     eax             ; eax = ret
    add     esp, 4          ; убираем saved EAX (syscall num)

    ; Восстанавливаем регистры
    add     esp, 4          ; ebx
    add     esp, 4          ; ecx
    add     esp, 4          ; edx
    pop     ebp
    pop     edi
    pop     esi

    iret

section .note.GNU-stack noalloc noexec nowrite progbits
