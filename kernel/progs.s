.section .rodata
.macro PROG name
.global prog_\name\()_start
.global prog_\name\()_end
prog_\name\()_start:
    .incbin "user/\name\().elf"
prog_\name\()_end:
.endm
PROG hello
PROG cat
PROG crash
PROG spin
PROG count
PROG launch
PROG hello32
.section .note.GNU-stack,"",@progbits
