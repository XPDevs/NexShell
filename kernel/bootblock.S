# -----------------------------------------------------------------------------
# DoorsOS NexShell Bootloader 
# -----------------------------------------------------------------------------
# Copyright (C) 2025 James Turner & (C) 2025 XPDevs
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
# Kernel Loading Loop
# -----------------------------------------------------------------------------

loadsector:
    mov     $1, %al
    mov     $0x02, %ah
    int     $0x13
    jc     load_error     

#------------------------------------------------------------------
# for use in the real NexShell jc must be set
# when testing diffrent things and dont want to boot set to jmp
#-------------------------------------------------------------------

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

halt_loop:
    hlt
    jmp     halt_loop

# -----------------------------------------------------------------------------
# Video Mode
# -----------------------------------------------------------------------------

set_video_mode:
    # Get VBE mode information for mode 0x101
    push    %es
    mov     $0x0800, %ax
    mov     %ax, %es            # es = 0x0800, for temp buffer
    xor     %di, %di            # es:di = 0x8000
    mov     $0x4F01, %ax
    mov     $0x101, %cx
    int     $0x10
    cmp     $0x004F, %ax
    jne     .fail

    # VBE call successful, get dimensions
    # Reload es to ensure it points to the buffer
    mov     $0x0800, %ax
    mov     %ax, %es
    movw    %es:0x12, %cx # XResolution
    movw    %es:0x14, %dx # YResolution
    pop     %es

    # Store dimensions at 0x9000
    push    %es
    mov     $0x0900, %ax
    mov     %ax, %es
    movw    %cx, %es:0
    movw    %dx, %es:2
    pop     %es

    # Set video mode
    mov     $0x4F02, %ax
    mov     $0x4101, %bx       # Mode 0x101 with Linear Frame Buffer (bit 14)
    int     $0x10
    jc      set_text_mode
    ret

.fail:
    pop     %es
    jmp     set_text_mode

set_text_mode:
    # Store default dimensions at 0x9000
    push    %es
    mov     $0x0900, %ax
    mov     %ax, %es
    movw    $80, %es:0
    movw    $25, %es:2
    pop     %es

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

# -----------------------------------------------------------------------------
# System Messages
# -----------------------------------------------------------------------------

loadmsg:
    .asciz "Loading NexShell..."

bootmsg:
    .asciz "\r\nBooting DoorsOS..."

errormsg:
    .asciz "\r\nError."

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
