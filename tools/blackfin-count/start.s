.section .text
.global __start
__start:
    SP.H = _stack_top; SP.L = _stack_top;
    FP = 0;
    call _main;
    P0 = 1;          /* exit */
    EXCPT 0;
1:  jump 1b;
.section .bss
.align 4
.space 65536
_stack_top:
.global _stack_top
