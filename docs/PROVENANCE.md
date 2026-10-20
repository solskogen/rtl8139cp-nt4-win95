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
