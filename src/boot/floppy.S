/* SPDX-License-Identifier: MIT
 *
 * Boot code for the driver floppy, placed after the FAT12 BPB (offset 0x3e)
 * and covering the rest of sector 0. The image has no 0x55AA signature, so
 * BIOSes that check it skip the floppy. A BIOS that runs it anyway gets this
 * code, which carries on as if the floppy were absent:
 * - A BIOS that follows the BIOS Boot Specification (most from 1996 on)
 *   boots the next device in its own boot order after INT 18h, so a CD-ROM
 *   set to boot before the hard disk is not passed over. Such a BIOS
 *   answers Plug and Play BIOS function 60h (BBS: get version), reached
 *   through the $PnP header in segment F000.
 * - Any other BIOS boots the floppy or the first hard disk, so the code
 *   starts that disk itself. If it cannot be read or has no boot signature,
 *   it asks for the floppy to be removed and reboots on a key press.
 */
    .code16
    .text
    .set COPY, 0x063e    /* relocated, so the MBR can be loaded at 0x7c00 */
start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    sti
    cld
    mov $0x7c00, %si
    mov $0x0600, %di
    mov $256, %cx
    rep movsw
    ljmp $0, $(COPY + relocated - start)
relocated:
    /* The $PnP header: on a 16-byte boundary in F000, its bytes (length at
       +5) summing to 0 */
    mov $0xf000, %ax
    mov %ax, %es
    xor %di, %di
scan:
    cmpw $0x5024, %es:(%di)  /* "$P" */
    jne next
    cmpw $0x506e, %es:2(%di) /* "nP" */
    jne next
    xor %ax, %ax
    mov %es:5(%di), %cl
    xor %ch, %ch
    jcxz next
    mov %di, %bx
sum:
    add %es:(%bx), %al
    inc %bx
    loop sum
    test %al, %al
    jz pnp
next:
    add $16, %di
    jnz scan
    jmp chain
pnp:
    /* Function 60h (Function, far *Version, BiosSelector) through the
       real-mode entry point: arguments pushed right to left, removed by the
       caller, AX = 0 when the BIOS follows the BBS */
    sub $2, %sp              /* the version */
    mov %sp, %bx
    pushw %es:0x1b(%di)      /* real-mode data segment */
    push %ss
    push %bx
    pushw $0x60
    lcall *%es:0x0d(%di)     /* real-mode entry: offset, then segment */
    add $10, %sp
    test %ax, %ax
    jnz chain
    int $0x18                /* the next device in the BIOS's boot order */
chain:
    xor %ax, %ax             /* ES pointed at F000 for the scan */
    mov %ax, %es
    mov $3, %bp
read:
    mov $0x0201, %ax     /* read one sector */
    mov $0x7c00, %bx
    mov $0x0001, %cx     /* cylinder 0, sector 1 */
    mov $0x0080, %dx     /* head 0, first hard disk */
    int $0x13
    jnc loaded
    xor %ah, %ah         /* reset the disk and retry */
    int $0x13
    dec %bp
    jnz read
    jmp fail
loaded:
    cmpw $0xaa55, 0x7dfe
    jne fail
    mov $0x80, %dl
    ljmp $0, $0x7c00
fail:
    mov $(COPY + message - start), %si
print:
    lodsb
    test %al, %al
    jz wait_key
    mov $0x0e, %ah
    mov $0x0007, %bx
    int $0x10
    jmp print
wait_key:
    xor %ah, %ah
    int $0x16
    int $0x19
message:
    .ascii "\r\nThis is the RTL8139C+ driver disk; it is not bootable.\r\n"
    .asciz "Remove it and press any key to restart.\r\n"
    /* The partition table and boot signature areas stay zero; .org fails
     * to assemble if the code grows into them. */
    .org 446 - 0x3e
    .fill 66, 1, 0
