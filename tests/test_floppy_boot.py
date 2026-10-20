#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the driver floppy in QEMU (SeaBIOS) and read the VGA text screen.

The test hard disk's MBR prints "TEST MBR DL=80" when started with
DL = 0x80, the BIOS number of the first hard disk.
"""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

QEMU = "qemu-system-i386"
FLOPPY = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                      "dist/rtl8139cp-floppy.img").resolve()
# xor ax,ax; mov ds,ax; mov si,ok; cmp dl,0x80; je p; mov si,bad
# p: lodsb; test al,al; jz h; mov ah,0x0e; mov bx,7; int 0x10; jmp p
# h: cli; hlt; jmp h
MBR = bytes([
    0x31, 0xC0, 0x8E, 0xD8, 0xBE, 0x21, 0x7C, 0x80, 0xFA, 0x80, 0x74, 0x03,
    0xBE, 0x30, 0x7C, 0xAC, 0x84, 0xC0, 0x74, 0x09, 0xB4, 0x0E, 0xBB, 0x07,
    0x00, 0xCD, 0x10, 0xEB, 0xF2, 0xFA, 0xF4, 0xEB, 0xFC,
]) + b"TEST MBR DL=80\0TEST MBR DL=??\0"
NOT_BOOTABLE = "not bootable"  # from src/boot/floppy.S
HD_STARTED = "TEST MBR DL=80"


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(str(path))
        self.sock.settimeout(5)
        self.file = self.sock.makefile("r")
        json.loads(self.file.readline())  # greeting
        self.command("qmp_capabilities")

    def command(self, name, **arguments):
        self.sock.sendall(json.dumps({"execute": name,
                                      "arguments": arguments}).encode())
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply:
                return reply["return"]
            assert "event" in reply, reply


def boot(tmp, extra, disk, until, timeout=20.0):
    """Run QEMU until until(screen) holds or timeout; return the screen."""
    monitor = tmp / "qmp.sock"
    dump = tmp / "screen.bin"
    command = [QEMU, "-nodefaults", "-vga", "std", "-display", "none",
               "-m", "16", "-qmp", f"unix:{monitor},server=on,wait=off",
               "-drive", f"if=floppy,format=raw,readonly=on,file={FLOPPY}",
               "-boot", "order=ac", *extra]
    if disk:
        command += ["-drive", f"if=ide,format=raw,file={disk}"]
    qemu = subprocess.Popen(command)
    screen = ""
    try:
        deadline = time.monotonic() + timeout
        while not monitor.exists():
            assert qemu.poll() is None and time.monotonic() < deadline
            time.sleep(0.05)
        qmp = Qmp(monitor)
        while time.monotonic() < deadline:
            time.sleep(0.25)
            qmp.command("pmemsave", val=0xB8000, size=4000,
                        filename=str(dump))
            screen = dump.read_bytes()[::2].decode("cp437")
            if until(screen):
                break
        qmp.command("quit")
        qemu.wait(5)
    finally:
        if qemu.poll() is None:
            qemu.kill()
            qemu.wait()
    return "\n".join(screen[i:i + 80].rstrip() for i in range(0, 2000, 80))


def main():
    with tempfile.TemporaryDirectory() as name:
        tmp = pathlib.Path(name)
        disk = tmp / "hd.img"
        disk.write_bytes(MBR.ljust(510, b"\0") + b"\x55\xaa" + bytes(1 << 20))

        # BIOS checks the boot signature: the floppy is skipped.
        screen = boot(tmp, [], disk, lambda s: HD_STARTED in s)
        assert "not a bootable disk" in screen and HD_STARTED in screen, screen
        assert NOT_BOOTABLE not in screen, screen

        # BIOS runs floppy boot code regardless: it starts the hard disk.
        screen = boot(tmp, ["-no-fd-bootchk"], disk, lambda s: HD_STARTED in s)
        assert screen.count("Booting from Floppy") == 1, screen
        assert "Booting from Hard Disk" not in screen, screen
        assert HD_STARTED in screen and NOT_BOOTABLE not in screen, screen

        # No hard disk: the floppy asks for removal instead of looping.
        screen = boot(tmp, ["-no-fd-bootchk"], None, lambda s: False, 4)
        assert screen.count("Booting from Floppy") == 1, screen
        assert NOT_BOOTABLE in screen and "press any key" in screen, screen
    print(f"{FLOPPY.name}: skipped by signature-checking BIOS; otherwise "
          "starts the hard disk, or asks for removal")


if __name__ == "__main__":
    main()
