section .text

_start:
    ; MessageBoxA(0, text, caption, MB_OK=0)
    push 0
    push caption
    push text
    push 0
    call [__imp_MessageBoxA]

    push 0
    call [__imp_ExitProcess]

section .data
text: db "Hello from RASM!", 0
caption: db "RASM", 0
