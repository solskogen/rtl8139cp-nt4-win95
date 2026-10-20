# RTL8139C+ NDIS 3 miniport: installation, configuration and diagnostics

**Status: NT4 confirmed working in an 86Box guest; Windows 95 OSR2.5
confirmed working in QEMU (`-device rtl8139`: DHCP, ping with fragmented
packets, a 2 MB HTTP download with matching SHA-256); physical cards not yet
confirmed.** Treat the first installs as tests: use a VM snapshot or a
machine you can recover. Please report results (see the end of this file).

`src/ndis/miniport.c` and the hardware core `src/core/rtl8139.c` build one
`r8139cp.sys`, shipped in both packages:

| Package | Target | Installer |
|---------|--------|-----------|
| `dist/nt4/` | Windows NT 4.0 SP6a x86 | `OEMSETUP.INF` (Network Control Panel script) |
| `dist/win95/` | Windows 95 OSR2 x86 | `r8139cp.inf` (Net class, `Have Disk`) |

`make floppy` writes `dist/rtl8139cp-floppy.img`, a 1.44 MB FAT12 disk with
both packages: use `A:\NT4` for Windows NT 4.0 and `A:\WIN95` for Windows 95
as the Have Disk path. Installer and driver files have 8.3 names.

The disk is deliberately not bootable. Its boot sector has no `0x55AA`
signature, so a BIOS that checks it skips the floppy and boots the next
device. A BIOS that runs the boot code anyway gets a small loader
(`src/boot/floppy.S`) that starts the first hard disk as if the floppy were
absent; if there is no bootable hard disk it asks for the floppy to be
removed and restarts on a key press. `make test-floppy` checks all three
cases in QEMU. Windows NT's FAT driver recognizes the volume by its jump
instruction and BPB, not the signature; Windows 95 also reads it (driver
installed from `A:\WIN95` in the QEMU test).

It is an NDIS 3.0 serialized miniport, imports only `NDIS.SYS` and is
compiled for i486 without a CRT or floating point.

## Supported hardware

PCI `10EC:8139` RTL8139C+, including QEMU's `-device rtl8139` and 86Box's
RTL8139C+. The driver checks the C+ hardware-version bits in `TxConfig` (not
the PCI revision) and fails cleanly on classic RTL8139/8139A/B/C/D chips,
which share the PCI ID.
The NT4 script can therefore offer installation on a classic chip; the
driver then logs "adapter not found" (marker `8139d00c`, see below).

## Installing

### Windows NT 4.0

1. Control Panel > Network > Adapters > Add > **Have Disk**.
2. Enter the path of `dist\nt4` or `A:\NT4` on the floppy image (the
   directory containing `OEMSETUP.INF`).
3. Select "Realtek RTL8139C+ PCI Fast Ethernet (open NDIS 3 driver)".
4. Setup detects every PCI `10EC:8139` function not already installed,
   copies `r8139cp.sys` to `%SystemRoot%\System32\drivers` and creates one
   adapter instance (`Rtl8139cp1`, ...) per function with its PCI bus and
   device number. Close the dialog, let the bindings/TCP/IP configuration
   run, and reboot.

Network > Adapters > **Properties** shows three dialogs in turn: connection
type, VLAN ID and network address. Values are checked before anything is
saved; Cancel in any dialog leaves the settings unchanged. Reboot when NT
asks. Removal uses Network > Adapters > Remove.

### Windows 95 OSR2

1. Let the New Hardware wizard find the PCI Ethernet controller, or use
   Device Manager > Other devices > PCI Ethernet Controller > Properties >
   Driver > Change Driver.
2. **Have Disk**, point to `dist\win95` or `A:\WIN95`, select the RTL8139C+
   entry.
3. Reboot when asked. Settings are on the adapter's **Advanced** tab.

Windows 95 needs a TCP/IP protocol bound to the adapter for `winipcfg`/`ping`
(Control Panel > Network > Add > Protocol > Microsoft > TCP/IP) if it is
not already installed.

Do not use Control Panel > Network > Add > Adapter: that installs a
separate, non-PCI adapter that gets no I/O port or interrupt, so the driver
cannot start. If it was added that way, remove it there first.

## Configuration

| Value | Type | Default | Meaning |
|-------|------|---------|---------|
| `ConnectionType` | integer 0-4 | 0 | 0 auto-negotiate, 1 10 Mbps half, 2 10 Mbps full, 3 100 Mbps half, 4 100 Mbps full |
| `VlanId` | integer 0-4094 | 0 | Static access VLAN; 0 disables. When set, transmitted frames are tagged and only frames of that VLAN (tag stripped) are received. When 0, tagged frames longer than 1514 bytes are dropped |
| `NetworkAddress` | 12 hex digits | (EEPROM) | Locally administered unicast MAC override |

NT4 stores them as `REG_DWORD` (or `REG_SZ` for `NetworkAddress`) under
`HKLM\SYSTEM\CurrentControlSet\Services\Rtl8139cpN\Parameters` and the
adapter's Properties dialogs edit them (an empty network address removes
the value). Setup also writes `BusType` (5), `BusNumber`,
`SlotNumber` (PCI device number, as Realtek's own INF does) and `MediaType`
(1) there; do not change those. The driver opens the card at `SlotNumber`
(NDIS supplies the bus). Windows 95 uses the Advanced tab; its INF also writes `BusType`/`AdapterType`
5 (PCI) and `MediaType` 1 like the vendor INF.

Out-of-range values are rejected: initialization fails with an error-log
entry instead of silently falling back to a default.

## Features

- C+ descriptor DMA: 64 TX and 64 RX descriptors, copy-based transmit and
  receive through NDIS shared memory (no virtual-to-physical guessing).
- Shared, level-sensitive PCI interrupts; the ISR claims only interrupts
  with a pending enabled source. Bounded deferred processing (8 status
  rounds, 64 received frames per round).
- Directed, broadcast, multicast (32-entry list, hardware hash),
  all-multicast and promiscuous filters; lookahead up to 1500 bytes.
- Link speed and media-connect status OIDs, refreshed on link-change
  interrupts (no asynchronous status indications, which keeps the import
  set minimal).
- Statistics: transmit/receive OK and error counts, receive no-buffer
  (missed + dropped), alignment errors, one/more collisions (hardware
  tally counters, accumulated across resets).
- Transmit-stall and fatal-bus-error detection via `CheckForHang`, with
  bounded reset; a device that disappears (all-ones reads) is stopped.
- A shutdown handler stops DMA on system shutdown.

Supported OIDs: `GEN_SUPPORTED_LIST`, `HARDWARE_STATUS`, `MEDIA_SUPPORTED`,
`MEDIA_IN_USE`, `MAXIMUM_LOOKAHEAD`, `MAXIMUM_FRAME_SIZE`, `LINK_SPEED`,
`TRANSMIT/RECEIVE_BUFFER_SPACE`, `TRANSMIT/RECEIVE_BLOCK_SIZE`, `VENDOR_ID`,
`VENDOR_DESCRIPTION`, `CURRENT_PACKET_FILTER`, `CURRENT_LOOKAHEAD`,
`DRIVER_VERSION`, `MAXIMUM_TOTAL_SIZE`, `PROTOCOL_OPTIONS`, `MAC_OPTIONS`,
`MEDIA_CONNECT_STATUS`, `VENDOR_DRIVER_VERSION`, `XMIT_OK`, `RCV_OK`,
`XMIT_ERROR`, `RCV_ERROR`, `RCV_NO_BUFFER`, and 802.3 `PERMANENT_ADDRESS`,
`CURRENT_ADDRESS`, `MULTICAST_LIST`, `MAXIMUM_LIST_SIZE`,
`RCV_ERROR_ALIGNMENT`, `XMIT_ONE_COLLISION`, `XMIT_MORE_COLLISIONS`.

## Not supported

- TCP/IP checksum and large-send offload: the NT4/Win95 TCP/IP stacks have
  no NDIS offload negotiation, so nothing is advertised.
- Wake-on-LAN and power management (no NDIS 3 power contract).
- Jumbo frames, multiple VLANs per adapter, classic (non-C+) RTL8139 chips.
- Windows 98/ME/2000 and later are not targets (untested; NDIS 3 binaries
  may or may not load there).

## Error log markers

Initialization failures write one NDIS error-log entry (NT4: Event Viewer,
System log, source = adapter instance) with six data DWORDs:
`8139d0NN` (failing step), the NDIS status, then four diagnostic words
(zero unless listed below). NT4's netevent message describes the error
class (out of resources, adapter not found, ...).

| NN | Step | Typical cause |
|----|------|---------------|
| 01 | Allocate adapter memory | Out of memory |
| 02 | Open configuration | Registry problem |
| 03 | `ConnectionType` | Value above 4 |
| 04 | `VlanId` | Value above 4094 |
| 05 | `NetworkAddress` | Not 12 hex digits, multicast or all-zero |
| 06 | PCI slot | `SlotNumber` above 255, or no `10EC:8139` at `SlotNumber` |
| 07 | Resources | `NdisMPciAssignResources` failed |
| 08 | I/O port resource | No 256-byte I/O BAR (memory-only configuration) |
| 09 | Interrupt resource | No IRQ assigned (check BIOS "PnP OS"/IRQ settings) |
| 0a | Register I/O range | Port range conflict |
| 0b | PCI command | I/O decode and bus mastering could not be enabled through the slot calls |
| 0c | Identify | Not a C+ chip (adapter-not-found), or chip not responding (hardware failure) |
| 0d | Map registers | Bus-master DMA unavailable |
| 0e | Shared memory | Not enough contiguous DMA memory below 4 GB |
| 0f | Start | Reset/initialization timed out |
| 10 | Interrupt | `NdisMRegisterInterrupt` failed (IRQ conflict) |

Diagnostic words (Event Viewer *Words* view, after `8139d0NN` and status):

- Step 06: `SlotNumber` and the vendor/device ID read there (`813910ec`
  expected; `ffffffff` = nothing at that slot).
- Step 0c: I/O base, the chip's `TxConfig` register (a C+ reads
  `748xxxxx`/`74cxxxxx`; other values are classic RTL8139 variants, which
  this driver does not support) and `SlotNumber`.

## Known risks (please report)

These are interface assumptions not yet confirmed on the targets:

- Windows 95 builds other than OSR2.5 (`NDIS.VXD` 4.00.1111, tested in
  QEMU), and 86Box's RTL8139C+ model under Windows 95.
- NT4: the `UTILITY.INF` subroutines `GetPCIInformation`,
  `IsNetCardAlreadyInstalled`, `AddHardwareComponent` behaving as the
  script expects.
- Timer calibration: microsecond delays use the chip's timer register at
  the 33 MHz PCI clock.

If the chip never leaves reset during halt and its PCI bus mastering cannot
be switched off and read back as off, the driver deliberately leaks its DMA
buffers (about 200 KB) instead of freeing memory the card might still write.

## Reporting results

Useful details: OS and service pack, physical card or QEMU (version and
command line), what Setup displayed, Event Viewer entries (Detail view, the
"Data" words, *Words* format), whether the adapter appears in
`ipconfig /all` (NT4) or `winipcfg` (Win95), and whether `ping` to another
host works. For NT4, also an export (`regedit /e`) of
`HKLM\SYSTEM\CurrentControlSet\Services\Rtl8139cp1`.
