# Implementation references and boundaries

## Original code

The project source, compatibility declarations, tests and INF files are
original MIT-licensed work. It does not vendor a DDK or a Linux source file.
The compatibility header `compat/ndis_miniport.h` describes public NDIS 3
function signatures, the 60-byte x86 miniport-characteristics structure and
only the structure fields the driver needs; opaque objects are treated as
sized storage. The installed public-domain ReactOS-derived MinGW
`ddk/ndis.h` was consulted for the registration signatures and
characteristic ordering; its modern version gates are not used.

## Hardware specification

Realtek **RTL8139C(L)+**, revision 1.6, track ID JATR-1076-21:

- https://datasheet4u.com/pdf/1565347/RTL8139C+.pdf
- PDF SHA-256:
  `0d7c64093a1258e2c632342ffcbe5eef9ee45a07ab510c268b4eba3d01e454df`
- Relevant sections: register map, 6.33 C+ command, 6.34 receive descriptor
  address, and the C+ descriptor-format descriptions.

The document is consulted for hardware facts, not redistributed here.

## QEMU reference

QEMU `v11.1.2`, `hw/net/rtl8139.c`:

- https://github.com/qemu/qemu/blob/v11.1.2/hw/net/rtl8139.c
- Annotated tag object: `ceaea6cb3e2a7d966f9ca97c7ebb5b88cd764dd0`
- File SHA-256:
  `06d7d34c2fbb30f9dd9f4304d3a626416f8b2d2a4bb948317b3907925712f0a6`
- The file carries an MIT-style permission notice, attributed to Igor
  Kovalenko (2006), and contributor credits.

The original core was checked against the register/descriptor facts and C+
identification used by that implementation. No QEMU source is vendored.
The integration test uses an installed QEMU binary rather than incorporating
its code into the driver.

## Linux request

Upstream reference identified during feasibility research:
Linux `v7.2.11`, `drivers/net/ethernet/realtek/8139cp.c`.
kernel.org's release metadata labelled that version stable at the time of
planning:

- https://www.kernel.org/releases.json
- https://raw.githubusercontent.com/gregkh/linux/v7.2.11/drivers/net/ethernet/realtek/8139cp.c

The file explicitly restricts use to an entirely GPL-licensed operating
system. This project does not assume a GPL license on a Windows miniport
resolves that notice. The user selected an original implementation using
documentation/permissive references instead. No claim of formal clean-room
development is made: the Linux file was inspected during planning.

## OS documentation

- NDIS version overview:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/network/overview-of-ndis-versions
- Task-offload overview:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/network/task-offload
- Legacy NDIS attributes:
  https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553619(v=vs.85)
- Archived DDK explanation of script-based `OEMSETUP.INF`:
  https://techshelps.github.io/MSDN/NTDDK/native/ddk/pg/src/appenda_4.htm
- Archived NT DDK binding-value definitions (a NIC driver's `Bind` lists
  underlying adapter device names):
  https://techshelps.github.io/MSDN/NTDDK/native/ddk/ng1/src/103bind_3.htm
- Archived NT DDK miniport linkage example (`Bind` and `Export` identify
  the same adapter, with the quoted instance name in `Route`):
  https://techshelps.github.io/MSDN/NTDDK/native/ddk/ng1/src/103bind_14.htm
- Archived NT DDK `NdisMRegisterMiniport` contract (NDIS 3 compatibility,
  version-specific characteristics length and documented failure statuses):
  https://techshelps.github.io/MSDN/NTDDK/native/ddk/nr/src/103ndisx_125.htm

Modern documentation does not prove NT4/Win95 support. In particular,
Windows 95 INF conventions, NT4's script-based network installer, and
modern NT `AddService` INFs are not interchangeable.

## Miniport interface references

`compat/ndis_miniport.h` contains original declarations of public NDIS 3
interface facts: function signatures, status/OID/error constants, the
packet-private prefix (`PhysicalCount`, `TotalLength`, `Head`), buffer
chaining, and the offset (216) of the Ethernet filter pointer `EthDB` in the
x86 miniport block, which receive indication macros dereference. These were
taken from the public Windows NT 4.0 DDK `ndis.h` as republished in
https://github.com/Wack0/entii-for-workcubes/tree/main/nt4/ddk (interface
reference only; nothing is copied) and cross-checked against MinGW's
public-domain `ddk/ndis.h`, which yields the same offset. Leaked Microsoft
source trees were deliberately not used.

The NT4 `OEMSETUP.INF` net rules follow the archived NT DDK Network Guide,
sections 3.4-3.4.7 (type/use/bindform/class/bindable syntax and adapter
rules): https://techshelps.github.io/MSDN/NTDDK/native/ddk/ng1/src/103bind_4.htm
The script calls the standard NT4 `UTILITY.INF`/`SUBROUTN.INF` subroutines
by their documented-by-convention names; their behavior on the target is a
listed runtime risk.

Tally-counter dump (`DTCCR`) semantics come from the RTL8139C+ datasheet
section 6.3 and agree with QEMU's `rtl8139_TxStatus_write`.

## Hardware facts compared with Linux 7.3

At the user's request the driver was compared with Linux `v7.3`
`drivers/net/ethernet/realtek/8139cp.c` (identical in `v7.4-rc2` apart
from NAPI interrupt masking). Its header notice reads, in part: "This file
is not a complete program and may only be used when the entire operating
system is licensed under the GPL."

- https://raw.githubusercontent.com/torvalds/linux/v7.3/drivers/net/ethernet/realtek/8139cp.c

No Linux code, comment text or structure was copied, translated or
paraphrased into the project. Only the hardware behaviour below was taken
as facts and implemented in original code and wording. Each item is listed
with the independent source that covers it, where one exists:

| Fact | Independent confirmation |
|------|--------------------------|
| Rx descriptor bit 22 reports a FIFO overflow and is not part of the bit-20 error summary | Datasheet Rx status descriptor: `FOVF` is bit 22, and `RES` covers only CRC, RUNT, RWT and FAE. The datasheet says the overflow occurred before the packet; dropping such frames (rather than delivering them) is a conservative design choice |
| Tx descriptor bit 25 is a FIFO underrun outside the bit-23 summary | Datasheet Tx status descriptor (`UNF`); QEMU v11.1.2 defines both bits |
| Write ring addresses after C+CR and before TE/RE (TCR/RCR after TE/RE) | Datasheet 6.33 recommends TE/RE before ring addresses; QEMU v11.1.2 clears the Rx ring address on reset and declines to receive while it is zero, which shows the hazard of the datasheet order |
| C+CR bit 3 enables PCI multiple read/write | Datasheet 6.33; QEMU treats the bit as writable |
| ISR bit 10 can be raised although documented reserved | None: Linux is the only source. Acknowledging it is harmless if it never fires |
| A tally dump may take longer than 1 ms; clear `DTCCR` afterwards | None: the 10 ms bound and the clearing follow Linux. Both only bound a wait and disarm a register |

Resetting the chip with bus mastering off before use, reading PCI command
writes back, clearing PCI status errors on reset and computing tally
counters as differences are the project's own design.

## 86Box reference

The user's local 86Box checkout (commit
`bf2f665b80c493a9441121a59d5d87816960e9b8`, `src/network/net_rtl8139.c`,
GPL-2.0, derived from QEMU's model) was read only to see how that emulator
handles reset and ring addresses. Nothing from it is used in the project;
the behaviour cited above is attributed to QEMU, where it was confirmed
independently.

## Windows 9x reference material

Section and file alignment of Realtek's own Windows 95, 98 and ME drivers,
the exports of Windows 98 SE and ME `NDIS.VXD`, and the hardware IDs in
Windows ME's in-box `netrtsnt.inf` were observed from the user's own
installation media and driver downloads. Only those facts are used: no
Microsoft or Realtek file is redistributed or copied.

