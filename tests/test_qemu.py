#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the actual shared C core against QEMU's RTL8139C+.

No guest OS, root access, TAP interface, or external network is needed.
qtest maps the test's coherent host DMA model into disposable guest RAM.
This exercises the NIC engine, not Windows loader/NDIS compatibility.
"""
import ctypes as C
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import time

U8, U16, U32 = C.c_uint8, C.c_uint16, C.c_uint32
PU8 = C.POINTER(U8)
COUNT, BUFFER = 64, 1536
OWN = 0x80000000


class Descriptor(C.Structure):
    _fields_ = [("status", U32), ("vlan", U32), ("low", U32), ("high", U32)]


class Dma(C.Structure):
    _fields_ = [
        ("rx_ring", C.POINTER(Descriptor)), ("tx_ring", C.POINTER(Descriptor)),
        ("rx_ring_bus", U32), ("tx_ring_bus", U32),
        ("rx_buffer", PU8 * COUNT), ("tx_buffer", PU8 * COUNT),
        ("rx_bus", U32 * COUNT), ("tx_bus", U32 * COUNT),
        ("tally", C.c_void_p), ("tally_bus", U32),
    ]


class Tally(C.Structure):
    _fields_ = [
        ("tx_ok", C.c_uint64), ("rx_ok", C.c_uint64), ("tx_error", C.c_uint64),
        ("rx_error", U32), ("rx_missed", U16), ("rx_alignment", U16),
        ("tx_one_collision", U32), ("tx_multi_collision", U32),
        ("rx_unicast", C.c_uint64), ("rx_broadcast", C.c_uint64),
        ("rx_multicast", U32), ("tx_abort", U16), ("tx_underrun", U16),
    ]


assert C.sizeof(Tally) == 64


R8 = C.CFUNCTYPE(U8, C.c_void_p, C.c_uint)
R16 = C.CFUNCTYPE(U16, C.c_void_p, C.c_uint)
R32 = C.CFUNCTYPE(U32, C.c_void_p, C.c_uint)
W8 = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, U8)
W16 = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, U16)
W32 = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, U32)
STALL = C.CFUNCTYPE(None, C.c_void_p, C.c_uint)
BARRIER = C.CFUNCTYPE(None, C.c_void_p)
RECEIVE = C.CFUNCTYPE(None, C.c_void_p, PU8, C.c_uint)


class Bus(C.Structure):
    _fields_ = [
        ("context", C.c_void_p), ("r8", R8), ("r16", R16), ("r32", R32),
        ("w8", W8), ("w16", W16), ("w32", W32),
        ("stall", STALL), ("barrier", BARRIER),
    ]


class Config(C.Structure):
    _fields_ = [("mac", U8 * 6), ("media", C.c_int), ("vlan", U16)]


class Stats(C.Structure):
    _fields_ = [(name, U32) for name in
                ("tx_ok", "tx_error", "rx_ok", "rx_error")]


class Device(C.Structure):
    _fields_ = [
        ("bus", Bus), ("dma", Dma), ("config", Config), ("stats", Stats),
        *[(name, C.c_uint) for name in
          ("tx_head", "tx_tail", "tx_count", "rx_head", "running", "faulted")],
    ]


class Qemu:
    def __init__(self, directory):
        exe = shutil.which(os.environ.get("QEMU", "qemu-system-i386"))
        if not exe:
            raise RuntimeError("qemu-system-i386 is required for test-qemu")
        self.path = str(pathlib.Path(directory) / "qtest.sock")
        # QEMU's network backend must be in RUNNING state. This tiny test ROM
        # halts the CPU with interrupts disabled, without booting an OS/BIOS
        # that could reconfigure our test device.
        rom = bytearray(65536)
        rom[0xFFF0:0xFFF4] = b"\xfa\xf4\xeb\xfd"
        rom_path = pathlib.Path(directory) / "halt.bin"
        rom_path.write_bytes(rom)
        self.log = open(pathlib.Path(directory) / "qemu.log", "w+")
        self.network, child = socket.socketpair()
        self.network.settimeout(5)
        self.process = subprocess.Popen([
            exe, "-no-user-config", "-nodefaults", "-display", "none",
            "-machine", "pc,accel=tcg", "-m", "32M", "-bios", str(rom_path),
            "-qtest", f"unix:{self.path},server=on,wait=off",
            "-qtest-log", "/dev/null", "-monitor", "none", "-serial", "none",
            "-device", "rtl8139,netdev=n,addr=04.0",
            "-netdev", f"socket,id=n,fd={child.fileno()}",
        ], pass_fds=[child.fileno()], stdout=self.log, stderr=self.log)
        child.close()
        self.control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.control.settimeout(5)
        try:
            deadline = time.monotonic() + 5
            while True:
                if self.process.poll() is not None:
                    self.log.seek(0)
                    raise RuntimeError("QEMU failed: " + self.log.read())
                try:
                    self.control.connect(self.path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() > deadline:
                        raise RuntimeError("QEMU qtest socket did not become responsive")
                    time.sleep(0.02)
            self.reader = self.control.makefile("rb")
            self.command("outl 0xcf8 0x80002000")
            assert int(self.command("inl 0xcfc"), 0) == 0x813910EC
            self.command("outl 0xcf8 0x80002010")
            self.command("outl 0xcfc 0xc001")
            self.command("outl 0xcf8 0x80002004")
            self.command("outw 0xcfc 5")
        except BaseException:
            self.close()
            raise

    def command(self, command):
        self.control.sendall((command + "\n").encode("ascii"))
        while True:
            reply = self.reader.readline().decode("ascii").strip()
            if reply.startswith("IRQ "):
                continue
            if not reply.startswith("OK"):
                raise RuntimeError(f"qtest {command}: {reply}")
            return reply[2:].strip()

    def read(self, address, length):
        text = self.command(f"read {address:#x} {length:#x}")
        return bytes.fromhex(text.removeprefix("0x"))

    def write(self, address, data):
        self.command(f"write {address:#x} {len(data):#x} 0x{data.hex()}")

    def receive_packet(self):
        def exact(size):
            result = bytearray()
            while len(result) < size:
                data = self.network.recv(size - len(result))
                if not data:
                    raise RuntimeError("QEMU closed its network socket")
                result.extend(data)
            return bytes(result)
        length = struct.unpack("!I", exact(4))[0]
        assert 14 <= length <= 65536, length
        return exact(length)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if hasattr(self, "reader"):
            self.reader.close()
        self.control.close()
        self.network.close()
        self.log.close()


class Mapping:
    def __init__(self, qemu, obj, bus_address):
        self.qemu = qemu
        self.address = C.addressof(obj)
        self.length = C.sizeof(obj)
        self.bus_address = bus_address
        self.previous = C.string_at(self.address, self.length)
        qemu.write(bus_address, self.previous)

    def sync(self):
        current = C.string_at(self.address, self.length)
        if current != self.previous:
            self.qemu.write(self.bus_address, current)
            self.previous = current
        else:
            actual = self.qemu.read(self.bus_address, self.length)
            C.memmove(self.address, actual, self.length)
            self.previous = actual


def run(qemu):
    lib = C.CDLL(str(pathlib.Path("build/libcore.so").resolve()))
    lib.rtl_init.argtypes = [C.POINTER(Device), C.POINTER(Bus),
                            C.POINTER(Dma), C.POINTER(Config)]
    lib.rtl_start.argtypes = lib.rtl_stop.argtypes = [C.POINTER(Device)]
    lib.rtl_send.argtypes = [C.POINTER(Device), PU8, C.c_uint]
    lib.rtl_set_filter.argtypes = [C.POINTER(Device), U32, PU8, C.c_uint]
    lib.rtl_poll_rx.argtypes = [C.POINTER(Device), C.c_uint, RECEIVE, C.c_void_p]
    lib.rtl_reap_tx.argtypes = [C.POINTER(Device)]
    lib.rtl_ack_interrupts.argtypes = [C.POINTER(Device), U16]
    lib.rtl_read_interrupts.argtypes = [C.POINTER(Device), C.POINTER(U16)]
    lib.rtl_dump_tally.argtypes = [C.POINTER(Device), C.POINTER(Tally)]

    # Allocate extra backing bytes so virtual and bus ring bases both meet
    # the device's 256-byte alignment contract.
    backing = [C.create_string_buffer(COUNT * 16 + 255) for _ in range(2)]
    rings = [(Descriptor * COUNT).from_address((C.addressof(b) + 255) & ~255)
             for b in backing]
    rx = [(U8 * BUFFER)() for _ in range(COUNT)]
    tx = [(U8 * BUFFER)() for _ in range(COUNT)]
    dma = Dma()
    dma.rx_ring, dma.tx_ring = rings
    dma.rx_ring_bus, dma.tx_ring_bus = 0x100000, 0x101000
    mappings = []
    for i in range(COUNT):
        dma.rx_buffer[i], dma.tx_buffer[i] = rx[i], tx[i]
        dma.rx_bus[i], dma.tx_bus[i] = 0x110000 + i * BUFFER, 0x130000 + i * BUFFER
        mappings += [Mapping(qemu, rx[i], dma.rx_bus[i]),
                     Mapping(qemu, tx[i], dma.tx_bus[i])]
    # Map each descriptor separately, to avoid overwriting another
    # descriptor's hardware update when the host changes a different slot.
    for ring, address in zip(rings, (dma.rx_ring_bus, dma.tx_ring_bus)):
        mappings += [Mapping(qemu, ring[i], address + i * 16) for i in range(COUNT)]

    tally_backing = C.create_string_buffer(64 + 63)
    tally_memory = Tally.from_address((C.addressof(tally_backing) + 63) & ~63)
    dma.tally, dma.tally_bus = C.addressof(tally_memory), 0x150000
    mappings.append(Mapping(qemu, tally_memory, dma.tally_bus))

    errors = []

    def guarded(fn, default=None):
        def invoke(*args):
            if errors:
                return default
            try:
                return fn(*args)
            except Exception as error:
                # ctypes callbacks cannot propagate exceptions through C.
                # Preserve and rethrow them at the next C-call boundary.
                errors.append(error)
                return default
        return invoke

    def sync():
        for mapping in mappings:
            mapping.sync()

    callbacks = [
        R8(guarded(lambda _, r: int(qemu.command(f"inb {0xc000+r:#x}"), 0), 0)),
        R16(guarded(lambda _, r: int(qemu.command(f"inw {0xc000+r:#x}"), 0), 0)),
        R32(guarded(lambda _, r: int(qemu.command(f"inl {0xc000+r:#x}"), 0), 0)),
        W8(guarded(lambda _, r, v: qemu.command(f"outb {0xc000+r:#x} {v:#x}"))),
        W16(guarded(lambda _, r, v: qemu.command(f"outw {0xc000+r:#x} {v:#x}"))),
        W32(guarded(lambda _, r, v: qemu.command(f"outl {0xc000+r:#x} {v:#x}"))),
        STALL(guarded(lambda _, us: time.sleep(us / 1_000_000))),
        BARRIER(guarded(lambda _: sync())),
    ]
    bus = Bus(None, *callbacks)
    device = Device()
    mac = b"\x02\x00\x00\x00\x00\x01"
    config = Config((U8 * 6).from_buffer_copy(mac), 0, 0)

    def call(fn, *args):
        result = fn(C.byref(device), *args)
        if errors:
            raise errors[0]
        return result

    assert call(lib.rtl_init, C.byref(bus), C.byref(dma), C.byref(config)) == 0
    assert call(lib.rtl_start) == 0
    assert call(lib.rtl_set_filter, 9, None, 0) == 0
    packets = []
    receive = RECEIVE(lambda _, data, size: packets.append(C.string_at(data, size)))

    pending = U16()

    def interrupt_status():
        assert call(lib.rtl_read_interrupts, C.byref(pending)) == 0
        return pending.value

    # More than two ring wraps, distinct payload on each frame.
    for i in range(130):
        payload_size = (0, 46, 1500)[i % 3]
        frame = b"\xff" * 6 + mac + b"\x88\xb5" + bytes([i]) * payload_size
        data = (U8 * len(frame)).from_buffer_copy(frame)
        assert call(lib.rtl_send, data, len(frame)) == 0
        assert qemu.receive_packet() == frame.ljust(60, b"\0")
        assert interrupt_status() & 4, "C+ TX completion status absent"
        assert int(qemu.command("inw 0xc03c"), 0) == 0
        sync()
        assert call(lib.rtl_reap_tx) == 1

        inbound = (mac + b"\x02\x00\x00\x00\x00\x02\x88\xb5" +
                   bytes([i]) * max(46, payload_size))
        qemu.network.sendall(struct.pack("!I", len(inbound)) + inbound)
        deadline = time.monotonic() + 5
        while rings[0][device.rx_head].status & OWN:
            sync()
            if time.monotonic() > deadline:
                raise RuntimeError(f"RX descriptor did not complete for packet {i}")
            time.sleep(0.001)
        assert interrupt_status() & 1, "C+ RX completion status absent"
        assert call(lib.rtl_ack_interrupts, 4) == 0
        assert interrupt_status() & 1 and not (pending.value & 4)
        # Masking/unmasking must not destroy a pending receive event.
        qemu.command("outw 0xc03c 5")
        assert interrupt_status() & 1
        qemu.command("outw 0xc03c 0")
        assert call(lib.rtl_ack_interrupts, pending.value) == 0
        assert interrupt_status() == 0
        assert call(lib.rtl_poll_rx, 64, receive, None) == 1
        assert packets[-1] == inbound
    assert device.stats.tx_ok == 130 and device.stats.rx_ok == 130
    assert device.stats.tx_error == 0
    # Hardware tally dump (DTCCR) through the DMA mapping.
    tally = Tally()
    assert call(lib.rtl_dump_tally, C.byref(tally)) == 0
    assert tally.tx_ok == 130 and tally.rx_ok == 130, (tally.tx_ok, tally.rx_ok)
    assert tally.rx_unicast == 130 and tally.tx_error == 0
    assert int(qemu.command("inl 0xc010"), 0) & 0x08 == 0
    assert call(lib.rtl_stop) == 0

    device.config.vlan = 100
    assert call(lib.rtl_start) == 0
    assert call(lib.rtl_set_filter, 9, None, 0) == 0
    frame = b"\xff" * 6 + mac + b"\x88\xb5" + b"\x55" * 1500
    data = (U8 * len(frame)).from_buffer_copy(frame)
    assert call(lib.rtl_send, data, len(frame)) == 0
    tagged = frame[:12] + b"\x81\x00\x00\x64" + frame[12:]
    assert qemu.receive_packet() == tagged
    sync()
    assert call(lib.rtl_reap_tx) == 1

    # A tagged RX must retain its tag on the hardware boundary but be
    # stripped exactly once before indicating the access frame to the caller.
    inbound = mac + b"\x02\x00\x00\x00\x00\x02\x88\xb5" + b"\xaa" * 1500
    tagged = inbound[:12] + b"\x81\x00\x00\x64" + inbound[12:]
    for tag, expected in ((100, True), (101, False)):
        wire = tagged[:14] + struct.pack("!H", tag) + tagged[16:]
        qemu.network.sendall(struct.pack("!I", len(wire)) + wire)
        deadline = time.monotonic() + 5
        while rings[0][device.rx_head].status & OWN:
            sync()
            if time.monotonic() > deadline:
                raise RuntimeError("VLAN RX descriptor did not complete")
            time.sleep(0.001)
        before = len(packets)
        assert call(lib.rtl_poll_rx, 64, receive, None) == 1
        assert len(packets) - before == int(expected)
        if expected:
            assert packets[-1] == inbound
    assert device.stats.rx_ok == 131 and device.stats.rx_error == 0
    assert call(lib.rtl_stop) == 0
    print("QEMU: C core TX/RX, tally dump, interrupt registers, padding, MTU 1500, ring wrap, restart and VLAN passed")


def main():
    with tempfile.TemporaryDirectory(prefix="rtl8139-qtest-") as directory:
        qemu = Qemu(directory)
        try:
            run(qemu)
        finally:
            qemu.close()


if __name__ == "__main__":
    main()
