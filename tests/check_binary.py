#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the actual PE imports, relocation directory, entry point and code."""
import argparse
import pathlib
import re
import struct
import subprocess


# The PCI slot calls are the ones Realtek's own NDIS 3 drivers for NT4
# (RTLNIC4.SYS) and Windows 95 (RTL8139.SYS) import.
EXPECTED = {
    "NdisInitializeWrapper", "NdisMRegisterMiniport",
    "NdisTerminateWrapper", "NdisWriteErrorLogEntry",
    "EthFilterDprIndicateReceive", "EthFilterDprIndicateReceiveComplete",
    "NdisAllocateMemory", "NdisCloseConfiguration", "NdisFreeMemory",
    "NdisMAllocateMapRegisters", "NdisMAllocateSharedMemory",
    "NdisMDeregisterInterrupt", "NdisMDeregisterIoPortRange",
    "NdisMFreeMapRegisters", "NdisMFreeSharedMemory", "NdisMRegisterInterrupt",
    "NdisMRegisterIoPortRange", "NdisMSendResourcesAvailable",
    "NdisMSetAttributes", "NdisMSynchronizeWithInterrupt",
    "NdisOpenConfiguration", "NdisQueryBuffer", "NdisReadConfiguration",
    "NdisReadNetworkAddress",
    "NdisReadPciSlotInformation", "NdisWritePciSlotInformation",
    "NdisMPciAssignResources", "NdisMRegisterAdapterShutdownHandler",
    "NdisMDeregisterAdapterShutdownHandler",
}
DRIVER = "build/r8139cp.sys"


# i486 integer instructions (AT&T mnemonics, optional size suffix). An
# allowlist also rejects P6 multi-byte NOPs (nopw/nopl), which a denylist
# misses.
CC = r"(?:n?[obcezsplga]|n?(?:ae|be|ge|le)|p[eo])"
I486 = re.compile(
    r"nop|(?:mov[sz]?[bw]?|lea|push|pop|pusha|popa|pushf|popf|xchg|xadd|"
    r"cmpxchg|bswap|add|adc|sub|sbb|cmp|cmps|inc|dec|neg|not|and|or|xor|test|"
    r"i?mul|i?div|cbtw|cwtl|cwtd|cltd|sh[lr]d?|sa[lr]|r[co][lr]|bt[crs]?|"
    r"bs[fr]|set" + CC + r"|j" + CC + r"|jmp|jecxz|loop|call|ret|leave|enter|"
    r"in|out|ins|outs|stos|lods|scas|cld|std|clc|stc|cmc|cli|sti|int3|lahf|"
    r"sahf)[bwl]?")
PREFIXES = {"lock", "rep", "repz", "repe", "repnz", "repne", "data16",
            "addr16", "cs", "ds", "es", "fs", "gs", "ss"}


def unsupported(disassembly):
    """Return objdump -d instructions that are not i486 integer code."""
    bad = []
    for line in disassembly.splitlines():
        fields = line.split("\t")
        if len(fields) < 3 or not fields[2].strip():
            continue
        words = fields[2].split()
        while len(words) > 1 and words[0] in PREFIXES:
            words.pop(0)
        if not I486.fullmatch(words[0]):
            bad.append(line.strip())
    return bad


def self_test():
    def line(insn):
        return "   11000:\t0f 00 \t" + insn
    good = ["mov %eax,%ebx", "movzbl (%eax),%edx", "movsbl %al,%eax",
            "rep movsl %ds:(%esi),%es:(%edi)", "rep stos %eax,%es:(%edi)",
            "lock addl $0x0,(%esp)", "sete %al", "jae 11010", "jnle 11010",
            "bswap %eax", "cmpxchg %ecx,(%edx)", "nop", "ret $0x8", "cltd",
            "in (%dx),%al", "outw %ax,(%dx)", "shrd %cl,%edx,%eax"]
    bad = ["cmove %ecx,%eax", "nopw 0x0(%eax,%eax,1)", "nopl 0x0(%eax)",
           "lock cmpxchg8b (%esi)", "cpuid", "rdtsc", "fldl (%eax)",
           "movd %eax,%mm0", "movdqa %xmm0,(%eax)", "ud2", "endbr32",
           "sysenter", "(bad)", "fcomip %st(1),%st"]
    assert not unsupported("\n".join(map(line, good))), unsupported(
        "\n".join(map(line, good)))
    assert len(unsupported("\n".join(map(line, bad)))) == len(bad), bad


def pe_checksum(image, field):
    total = 0
    for offset in range(0, len(image) & ~1, 2):
        if field <= offset < field + 4:
            continue
        total += struct.unpack_from("<H", image, offset)[0]
        total = (total & 0xFFFF) + (total >> 16)
    if len(image) & 1:
        total += image[-1]
    total = (total & 0xFFFF) + (total >> 16)
    return (total & 0xFFFF) + len(image)


def check(path, objdump):
    image = pathlib.Path(path).read_bytes()
    assert image[:2] == b"MZ", path
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    assert image[pe:pe + 4] == b"PE\0\0", path
    machine, sections, stamp, _, _, opt_size, flags = struct.unpack_from(
        "<HHIIIHH", image, pe + 4)
    assert machine == 0x14C and opt_size >= 224, path
    assert not flags & 1, "base relocations stripped"
    assert stamp == 0, "timestamp makes the build irreproducible"
    opt = pe + 24
    assert struct.unpack_from("<H", image, opt)[0] == 0x10B, path
    assert struct.unpack_from("<I", image, opt + 16)[0] != 0, "missing entry"
    assert struct.unpack_from("<H", image, opt + 68)[0] == 1, "not native subsystem"
    assert struct.unpack_from("<HH", image, opt + 40) == (4, 0), "OS version"
    assert struct.unpack_from("<HH", image, opt + 48) == (4, 0), "subsystem version"
    assert struct.unpack_from("<I", image, opt + 64)[0] == \
        pe_checksum(image, opt + 64), "PE checksum"
    reloc_rva, reloc_size = struct.unpack_from("<II", image, opt + 96 + 5 * 8)
    assert reloc_rva and reloc_size, "missing base relocations"
    for i in range(sections):
        header = opt + opt_size + 40 * i
        name = image[header:header + 8].rstrip(b"\0").decode()
        characteristics = struct.unpack_from("<I", image, header + 36)[0]
        assert characteristics & 0xA0000000 != 0xA0000000, \
            f"writable and executable section {name}"
    output = subprocess.check_output([objdump, "-p", path], text=True)
    dlls = set(re.findall(r"DLL Name: (\S+)", output))
    assert dlls == {"NDIS.SYS"}, output
    imports = set(re.findall(r"^\s+[0-9a-f]+\s+(?:<none>\s+)?[0-9a-f]+\s+(\w+)\s*$",
                             output, re.MULTILINE))
    assert imports == EXPECTED, (imports ^ EXPECTED)
    bad = unsupported(subprocess.check_output([objdump, "-d", path], text=True))
    assert not bad, "non-i486 instructions:\n" + "\n".join(bad[:20])
    print(f"{path}: PE32/native, checksum, relocations, W^X, imports, "
          "i486 instructions passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", default="i686-w64-mingw32-objdump")
    args = parser.parse_args()
    self_test()
    check(DRIVER, args.objdump)
    undefined = subprocess.check_output(
        [args.objdump, "-t", "build/core-i486.o"], text=True)
    assert "*UND*" not in undefined, undefined
    print("shared core: freestanding i486 object has no unresolved runtime helpers")


if __name__ == "__main__":
    main()
