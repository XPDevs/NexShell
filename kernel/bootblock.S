# -----------------------------------------------------------------------------
# DoorsOS NexShell Bootloader 
# -----------------------------------------------------------------------------
# Copyright (C) 2015 The University of Notre Dame
# Modified by XPDevs (2025) for DoorsOS / NexShell integration
# Licensed under GNU GPL v2 or later.
# -----------------------------------------------------------------------------

.code16
.text
.global _start

# -----------------------------------------------------------------------------
# Bootloader Entry Point
# -----------------------------------------------------------------------------

_start:
    ljmp    $0x07c0, $_start2  # BOOTBLOCK_SEGMENT = 0x07c0

_start2:
    sti
    cld

    # Set up segment registers
    mov     %cs, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     $0x0000, %ax       # INTERRUPT_STACK_SEGMENT = 0x0000
    mov     %ax, %ss
    mov     $0xfff0, %sp       # INTERRUPT_STACK_OFFSET = 0xfff0

    # Clear screen and set text mode
    call    clear_screen

    # Save boot disk and partition table info
    mov     %dl, (disk_number)
    mov     partition_status, %di
    mov     $12, %cx
    rep     movsb

    # Display kernel loading message
    mov     $(loadmsg), %si
    call    bios_putstring

    # Reset disk system
    mov     $0, %ah
    int     $0x13

    # Get disk geometry
    mov     $0x08, %ah
    int     $0x13
    and     $0x3f, %cl
    mov     %cl, (disk_sectors)
    mov     %ch, (disk_cylinders)
    mov     %dh, (disk_heads)

    # Prepare for loading kernel
    mov     $0x1000, %ax       # KERNEL_SEGMENT = 0x1000
    mov     %ax, %es
    mov     $0x0000, %bx       # KERNEL_OFFSET = 0x0000
    mov     (disk_number), %dl
    xor     %ch, %ch
    xor     %dh, %dh
    mov     $2, %cl

# -----------------------------------------------------------------------------
# Kernel Loading Loop (With Slow Spinner)
# -----------------------------------------------------------------------------

loadsector:
    mov     $1, %al
    mov     $0x02, %ah
    int     $0x13
    jc     load_error     # jmp for debug jc for real

    # Show progress slowly
    mov     $'.', %al
    call    bios_putchar
    call    dot_delay

    mov     (sectors_left), %ax
    cmp     $0xffff, %ax
    jne     gotsectors
    mov     %es:20, %eax       # KERNEL_SIZE_OFFSET = 20
    shr     $9, %eax
    inc     %eax

gotsectors:
    dec     %ax
    mov     %ax, (sectors_left)
    cmp     $0, %ax
    je      loaddone

checksegment:
    add     $512, %bx
    cmp     $0, %bx
    jnz     nextsector
    mov     %es, %ax
    add     $0x1000, %ax
    mov     %ax, %es

nextsector:
    inc     %cl
    mov     (disk_sectors), %al
    cmp     %al, %cl
    jle     loadsector
    mov     $1, %cl

    inc     %dh
    mov     (disk_heads), %al
    cmp     %al, %dh
    jle     loadsector
    xor     %dh, %dh

    inc     %ch
    mov     (disk_cylinders), %al
    cmp     %al, %ch
    jle     loadsector

# -----------------------------------------------------------------------------
# Boot
# -----------------------------------------------------------------------------

loaddone:
    mov     $0, %ah
    int     $0x13

    mov     $(bootmsg), %si
    call    bios_putstring

    call    set_video_mode

    mov     $0x1000, %ax       # KERNEL_SEGMENT = 0x1000
    mov     %ax, %ds
    ljmp    $0x1000, $0x0000   # KERNEL_SEGMENT:KERNEL_OFFSET

# -----------------------------------------------------------------------------
# Error Handling
# -----------------------------------------------------------------------------

load_error:
    mov     $(errormsg), %si
    call    bios_putstring

ask_reboot:
    mov     $(rebootmsg), %si
    call    bios_putstring

wait_key:
    call    bios_getkey
    cmpb    $0, %al
    je      wait_key

    cmpb    $'Y', %al
    je      reboot_system
    cmpb    $'y', %al
    je      reboot_system
    cmpb    $'N', %al
    je      halt_loop
    cmpb    $'n', %al
    je      halt_loop
    jmp     wait_key

reboot_system:
    mov     $0, %ax
    int     $0x19

halt_loop:
    hlt
    jmp     halt_loop

# -----------------------------------------------------------------------------
# Video Mode
# -----------------------------------------------------------------------------

set_video_mode:
    mov     $0x4F02, %ax
    mov     $0x101, %bx
    int     $0x10
    jc      set_text_mode
    ret

set_text_mode:
    mov     $0x00, %ah
    mov     $0x03, %al
    int     $0x10
    ret

# -----------------------------------------------------------------------------
# BIOS Utility Functions
# -----------------------------------------------------------------------------

clear_screen:
    mov     $0x00, %ah
    mov     $0x03, %al
    int     $0x10
    ret

bios_putstring:
    mov     (%si), %al
    cmp     $0, %al
    jz      bios_putstring_done
    call    bios_putchar
    inc     %si
    jmp     bios_putstring
bios_putstring_done:
    ret

bios_putchar:
    push    %ax
    push    %bx
    mov     $0x0E, %ah
    mov     $0x01, %bl
    int     $0x10
    pop     %bx
    pop     %ax
    ret

bios_getkey:
    mov     $0x00, %ah
    int     $0x16
    ret

# -----------------------------------------------------------------------------
# Simple Delay (dot spinner)
# -----------------------------------------------------------------------------

dot_delay:
    push    %cx
    mov     $0xFFFF, %cx
.delay_loop:
    loop    .delay_loop
    pop     %cx
    ret

# -----------------------------------------------------------------------------
# System Messages
# -----------------------------------------------------------------------------

loadmsg:
    .asciz "[SYSTEM] Loading NexShell!"

bootmsg:
    .asciz "\r\n[SYSTEM] Booting NexShell!"

errormsg:
    .asciz "\r\n[SYSTEM ERROR] Failed to load NexShell. Halting."

rebootmsg:
    .asciz "\r\nDo you want to reboot? (Y/N):\n"

# -----------------------------------------------------------------------------
# BIOS Disk Info
# -----------------------------------------------------------------------------

disk_number:        .byte 0
disk_cylinders:     .byte 0
disk_heads:         .byte 0
disk_sectors:       .byte 0
sectors_left:       .word 0xffff

partition_status:       .byte 0
partition_start_chs:    .byte 0, 0, 0
partition_type:         .byte 0
partition_stop_chs:     .byte 0, 0, 0
partition_start_lba:    .long 0
partition_length:       .long 0

# -----------------------------------------------------------------------------
# Boot Signature
# -----------------------------------------------------------------------------

.org 510
bootflag:
    .word 0xAA55
