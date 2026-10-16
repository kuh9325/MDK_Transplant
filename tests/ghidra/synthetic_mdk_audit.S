# Hand-authored synthetic fixture. No original MDK instructions or data.
.ifndef RESOLVED
.equ RESOLVED, 0
.endif
.section .text
.global synthetic_start
synthetic_start:
    call synthetic_punch
    ret
.org 0x40210c - 0x401000
synthetic_stop:
    ret
.org 0x4022b8 - 0x401000
synthetic_play:
    ret
.org 0x402388 - 0x401000
synthetic_ensure:
    ret
.org 0x432f84 - 0x401000
synthetic_punch:
    mov 0x540c74, %eax
    mov 0x54161f, %ecx
    mov 0x54c5e0, %edx
    push %edx
    call synthetic_notify
    add $4, %esp
.if RESOLVED
    nop
    nop
.else
    call *%eax
.endif
    jmp synthetic_play
.org 0x465228 - 0x401000
synthetic_latch:
    movl $1, 0x540c74
    mov 0x54c5e4, %eax
    call synthetic_stop
    ret
.org 0x469668 - 0x401000
synthetic_notify:
    pushl 0x54c5e4
    call synthetic_ensure
    add $4, %esp
    ret
.section .data
.org 0x100
    .ascii "SYNTHETIC_MDK_AUDIT_FIXTURE_ONLY_v1"
.org 0xc74
    .long 0
.org 0x161f
    .long 3
.org 0xc5e0
    .long 0
    .long 0
