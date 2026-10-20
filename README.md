# RTL8139C+ driver for Windows NT 4.0 and Windows 95

An open-source NDIS 3 miniport driver for the Realtek RTL8139C+ Fast
Ethernet controller, for Windows NT 4.0 and Windows 95. One source tree,
built with `i686-w64-mingw32-gcc`, produces a single `r8139cp.sys` that
both systems load, plus an installer for each system.

## Status

| Environment | Result |
|-------------|--------|
| Windows NT 4.0 in 86Box (RTL8139C+ model) | Installs and works |
| Windows 95 OSR2.5 in QEMU (`rtl8139`) | Installs and works: DHCP, ping, HTTP download verified by SHA-256 |
| Physical RTL8139C+ cards | Not yet tested |
| Other Windows 95 builds, Windows 95 in 86Box | Not yet tested |

Reports from real hardware are welcome; see "Reporting results" in
[docs/DRIVER.md](docs/DRIVER.md).

## Hardware

PCI `10EC:8139` with the RTL8139C+ engine. This covers physical cards,
QEMU's `rtl8139` device and 86Box's RTL8139C+. Classic RTL8139, 8139A/B/C/D
chips share the PCI ID. The driver identifies the chip by its TxConfig
hardware version and refuses to start on non-C+ chips.

## Installing

Build the packages (see below), or use a release download if one is
published. Then:

- **Windows NT 4.0 SP6a:** Control Panel > Network > Adapters > Add >
  Have Disk, and point it at `A:\NT4`.
- **Windows 95 OSR2:** Device Manager > Other devices > PCI Ethernet
  Controller > Properties > Driver > Change Driver > Have Disk, and point
  it at `A:\WIN95`. Install it on the PCI device, not through
  Network > Add, otherwise the adapter gets no PCI resources.

[docs/DRIVER.md](docs/DRIVER.md) has the full instructions, configuration
keywords (`ConnectionType`, `VlanId`, `NetworkAddress`), event-log error
codes and known risks. For a first install, use a VM snapshot or a machine
you can restore.

## Building

Requires GNU make, `i686-w64-mingw32-gcc`, a host C compiler and Python 3.
`mtools` is needed for the floppy image and `qemu-system-i386` for the
QEMU tests.

```sh
make            # build/r8139cp.sys
make check      # run the tests
make package    # dist/nt4 and dist/win95
make floppy     # dist/rtl8139cp-floppy.img
make help       # all targets
```

## Layout

| Path | Purpose |
|------|---------|
| `src/core/`, `include/` | OS-independent C+ hardware core: reset, identification, descriptor rings, filters, VLAN, media, counters |
| `src/ndis/miniport.c` | NDIS 3 miniport, shared by NT 4.0 and Windows 95 |
| `src/boot/floppy.S` | Boot sector for the floppy: starts the hard disk, or asks you to remove the disk |
| `compat/` | Minimal NDIS 3 declarations, `NDIS.SYS` import list, port I/O |
| `packaging/` | NT 4.0 and Windows 95 installers |
| `tests/` | Host tests, QEMU tests, binary and package checks |
| `docs/` | Driver manual and provenance |

## Features

| Feature | State |
|---------|-------|
| Send/receive, directed/multicast/broadcast/promiscuous filters, statistics, reset and hang recovery | Supported |
| Link speed/duplex selection, MAC address override, static access VLAN | Supported, configurable |
| Wake-on-LAN | Not supported (NDIS 3 has no power-management interface) |
| Checksum and large-send offload | Not supported (the NT 4.0 and Windows 95 TCP/IP stacks cannot use them) |
| Jumbo frames, multiple VLANs, classic RTL8139 chips | Not supported |

## License and sources

MIT licensed; see [LICENSE](LICENSE). The driver was written from the
Realtek RTL8139C(L)+ datasheet, with QEMU's MIT-licensed `rtl8139` model as
a reference. It contains no code from Linux's `8139cp.c`, whose license
notice restricts it to GPL operating systems. See
[docs/PROVENANCE.md](docs/PROVENANCE.md) for details.

This project is not affiliated with or endorsed by Realtek. Realtek,
Windows and Windows NT are trademarks of their respective owners.
