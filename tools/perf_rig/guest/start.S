    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .text.rig_start, "ax"
    .global _start
    .thumb_func
_start:
    ldr r0, =__stack_top
    mov sp, r0
    bl main
    ldr r1, =0x4FF00004
    movs r0, #1
    str r0, [r1]
1:  b 1b
    .ltorg

    .section .scene, "a"
    .global scene_blob
    .balign 4
scene_blob:
    .incbin SCENE_BIN
