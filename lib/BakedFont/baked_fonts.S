.section .rodata
/* .incbin paths are from the project root. gas is not given -I. */
.global baked_mincho_start
.global baked_mincho_end
.global baked_gothic_start
.global baked_gothic_end
.balign 4
baked_mincho_start:
.incbin "lib/BakedFont/mincho.ygzf"
baked_mincho_end:
.balign 4
baked_gothic_start:
.incbin "lib/BakedFont/gothic.ygzf"
baked_gothic_end:
.balign 4
