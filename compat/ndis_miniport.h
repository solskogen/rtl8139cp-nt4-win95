/* SPDX-License-Identifier: MIT
 * NDIS 3.0 binary-compatible miniport ABI subset shared by NT4 and Win95:
 * registration (60-byte characteristics), packets, resources, OIDs.
 * Declarations are original; names, values, signatures and offsets were
 * verified against the public NT4 DDK ndis.h/ntddndis.h/netevent.h (x86
 * BINARY_COMPATIBLE section) and cross-checked with the NDIS 5 layout.
 * Only functions exported by ndis.sys are used: port I/O, stalls and DMA
 * barriers are implemented locally because the DDK maps them to HAL. */
#ifndef RTL_NDIS_MINIPORT_H
#define RTL_NDIS_MINIPORT_H

#include <stddef.h>
#include <stdint.h>

#if !defined(RTL_HOST_TEST) && (!defined(__i386__) || !defined(_WIN32))
#error This ABI is for 32-bit Windows only
#endif

#if defined(RTL_HOST_TEST)
#define NDIS_CALL
#define NDIS_IMPORT
#else
#define NDIS_CALL __attribute__((stdcall))
#define NDIS_IMPORT __attribute__((dllimport))
#endif

typedef void *ndis_handle;
typedef uint32_t ndis_status;
typedef uint8_t ndis_boolean;

struct ndis3_characteristics {
    uint8_t major;
    uint8_t minor;
    uint32_t reserved;
    ndis_boolean (NDIS_CALL *check_for_hang)(ndis_handle);
    void (NDIS_CALL *disable_interrupt)(ndis_handle);
    void (NDIS_CALL *enable_interrupt)(ndis_handle);
    void (NDIS_CALL *halt)(ndis_handle);
    void (NDIS_CALL *handle_interrupt)(ndis_handle);
    ndis_status (NDIS_CALL *initialize)(ndis_status *, uint32_t *, uint32_t *,
                                      uint32_t, ndis_handle, ndis_handle);
    void (NDIS_CALL *isr)(ndis_boolean *, ndis_boolean *, ndis_handle);
    ndis_status (NDIS_CALL *query)(ndis_handle, uint32_t, void *, uint32_t,
                                 uint32_t *, uint32_t *);
    ndis_status (NDIS_CALL *reconfigure)(ndis_status *, ndis_handle, ndis_handle);
    ndis_status (NDIS_CALL *reset)(ndis_boolean *, ndis_handle);
    ndis_status (NDIS_CALL *send)(ndis_handle, void *, uint32_t);
    ndis_status (NDIS_CALL *set)(ndis_handle, uint32_t, void *, uint32_t,
                               uint32_t *, uint32_t *);
    ndis_status (NDIS_CALL *transfer)(void *, uint32_t *, ndis_handle,
                                    ndis_handle, uint32_t, uint32_t);
};

#if !defined(RTL_HOST_TEST)
_Static_assert(sizeof(void *) == 4, "x86 pointer ABI");
_Static_assert(sizeof(struct ndis3_characteristics) == 60,
               "NDIS 3 miniport characteristics length");
_Static_assert(offsetof(struct ndis3_characteristics, check_for_hang) == 8,
               "NDIS 3 callback prefix");
_Static_assert(offsetof(struct ndis3_characteristics, transfer) == 56,
               "NDIS 3 callback suffix");
#endif

NDIS_IMPORT void NDIS_CALL NdisInitializeWrapper(ndis_handle *, void *, void *,
                                                void *);
NDIS_IMPORT ndis_status NDIS_CALL NdisMRegisterMiniport(
    ndis_handle, struct ndis3_characteristics *, uint32_t);
NDIS_IMPORT void NDIS_CALL NdisTerminateWrapper(ndis_handle, void *);
NDIS_IMPORT void __attribute__((cdecl)) NdisWriteErrorLogEntry(
    ndis_handle, uint32_t, uint32_t, ...);

ndis_status NDIS_CALL DriverEntry(void *, void *);

#define NDIS_STATUS_SUCCESS UINT32_C(0)
#define NDIS_STATUS_FAILURE UINT32_C(0xc0000001)
#define NDIS_STATUS_RESOURCES UINT32_C(0xc000009a)
#define NDIS_STATUS_NOT_SUPPORTED UINT32_C(0xc00000bb)
#define NDIS_STATUS_HARD_ERRORS UINT32_C(0x80010004)
#define NDIS_STATUS_ADAPTER_NOT_FOUND UINT32_C(0xc0010006)
#define NDIS_STATUS_MULTICAST_FULL UINT32_C(0xc0010009)
#define NDIS_STATUS_INVALID_LENGTH UINT32_C(0xc0010014)
#define NDIS_STATUS_INVALID_DATA UINT32_C(0xc0010015)
#define NDIS_STATUS_INVALID_OID UINT32_C(0xc0010017)
#define NDIS_STATUS_UNSUPPORTED_MEDIA UINT32_C(0xc0010019)

/* netevent.h EVENT_NDIS_* values used with NdisWriteErrorLogEntry. */
#define NDIS_ERROR_RESOURCE_CONFLICT UINT32_C(0xc0001388)
#define NDIS_ERROR_OUT_OF_RESOURCES UINT32_C(0xc0001389)
#define NDIS_ERROR_HARDWARE_FAILURE UINT32_C(0xc000138a)
#define NDIS_ERROR_ADAPTER_NOT_FOUND UINT32_C(0xc000138b)
#define NDIS_ERROR_INTERRUPT_CONNECT UINT32_C(0xc000138c)
#define NDIS_ERROR_NETWORK_ADDRESS UINT32_C(0xc0001390)
#define NDIS_ERROR_UNSUPPORTED_CONFIGURATION UINT32_C(0xc0001391)

enum {
    NDIS_PARAMETER_INTEGER = 0,
    NDIS_INTERFACE_PCI = 5,          /* INTERFACE_TYPE PCIBus */
    NDIS_INTERRUPT_LEVEL_SENSITIVE = 0,
    NDIS_RESOURCE_PORT = 1,          /* CmResourceTypePort */
    NDIS_RESOURCE_INTERRUPT = 2      /* CmResourceTypeInterrupt */
};

#define NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA 0x01u
#define NDIS_MAC_OPTION_RECEIVE_SERIALIZED 0x02u
#define NDIS_MAC_OPTION_TRANSFERS_NOT_PEND 0x04u
#define NDIS_MAC_OPTION_NO_LOOPBACK 0x08u

#define NDIS_PACKET_TYPE_DIRECTED 0x0001u
#define NDIS_PACKET_TYPE_MULTICAST 0x0002u
#define NDIS_PACKET_TYPE_ALL_MULTICAST 0x0004u
#define NDIS_PACKET_TYPE_BROADCAST 0x0008u
#define NDIS_PACKET_TYPE_PROMISCUOUS 0x0020u
#define NDIS_PACKET_TYPE_ALL_LOCAL 0x0080u

#define OID_GEN_SUPPORTED_LIST 0x00010101u
#define OID_GEN_HARDWARE_STATUS 0x00010102u
#define OID_GEN_MEDIA_SUPPORTED 0x00010103u
#define OID_GEN_MEDIA_IN_USE 0x00010104u
#define OID_GEN_MAXIMUM_LOOKAHEAD 0x00010105u
#define OID_GEN_MAXIMUM_FRAME_SIZE 0x00010106u
#define OID_GEN_LINK_SPEED 0x00010107u
#define OID_GEN_TRANSMIT_BUFFER_SPACE 0x00010108u
#define OID_GEN_RECEIVE_BUFFER_SPACE 0x00010109u
#define OID_GEN_TRANSMIT_BLOCK_SIZE 0x0001010au
#define OID_GEN_RECEIVE_BLOCK_SIZE 0x0001010bu
#define OID_GEN_VENDOR_ID 0x0001010cu
#define OID_GEN_VENDOR_DESCRIPTION 0x0001010du
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010eu
#define OID_GEN_CURRENT_LOOKAHEAD 0x0001010fu
#define OID_GEN_DRIVER_VERSION 0x00010110u
#define OID_GEN_MAXIMUM_TOTAL_SIZE 0x00010111u
#define OID_GEN_PROTOCOL_OPTIONS 0x00010112u
#define OID_GEN_MAC_OPTIONS 0x00010113u
#define OID_GEN_MEDIA_CONNECT_STATUS 0x00010114u
#define OID_GEN_VENDOR_DRIVER_VERSION 0x00010116u
#define OID_GEN_XMIT_OK 0x00020101u
#define OID_GEN_RCV_OK 0x00020102u
#define OID_GEN_XMIT_ERROR 0x00020103u
#define OID_GEN_RCV_ERROR 0x00020104u
#define OID_GEN_RCV_NO_BUFFER 0x00020105u
#define OID_802_3_PERMANENT_ADDRESS 0x01010101u
#define OID_802_3_CURRENT_ADDRESS 0x01010102u
#define OID_802_3_MULTICAST_LIST 0x01010103u
#define OID_802_3_MAXIMUM_LIST_SIZE 0x01010104u
#define OID_802_3_RCV_ERROR_ALIGNMENT 0x01020101u
#define OID_802_3_XMIT_ONE_COLLISION 0x01020102u
#define OID_802_3_XMIT_MORE_COLLISIONS 0x01020103u

struct ndis_string {             /* UNICODE_STRING on both targets */
    uint16_t length;             /* bytes, excluding a terminator */
    uint16_t maximum_length;
    const uint16_t *buffer;
};

struct ndis_configuration_parameter {
    uint32_t type;
    union {
        uint32_t integer;
        struct ndis_string string;
    } data;
};

/* CM_PARTIAL_RESOURCE_DESCRIPTOR: 64-bit starts are split so that the
 * layout is identical in 64-bit host tests. */
struct ndis_resource_descriptor {
    uint8_t type;
    uint8_t share_disposition;
    uint16_t flags;
    union {
        struct {
            uint32_t start_low;
            uint32_t start_high;
            uint32_t length;
        } port;
        struct {
            uint32_t level;
            uint32_t vector;
            uint32_t affinity;
        } interrupt;
    } u;
};

struct ndis_resource_list {      /* CM_PARTIAL_RESOURCE_LIST */
    uint16_t version;
    uint16_t revision;
    uint32_t count;
    struct ndis_resource_descriptor descriptors[1];
};

_Static_assert(sizeof(struct ndis_resource_descriptor) == 16,
               "CM_PARTIAL_RESOURCE_DESCRIPTOR size");
_Static_assert(offsetof(struct ndis_resource_descriptor, u.interrupt.level) == 4,
               "CM interrupt level");
_Static_assert(offsetof(struct ndis_resource_descriptor, u.port.length) == 12,
               "CM port length");
_Static_assert(offsetof(struct ndis_resource_list, descriptors) == 8,
               "CM_PARTIAL_RESOURCE_LIST descriptors");

/* NDIS_BUFFER is an MDL (NT) or NDIS buffer (Win95); both start with Next.
 * Everything else about it is accessed through NdisQueryBuffer. */
struct ndis_buffer {
    struct ndis_buffer *next;
};

/* NDIS_PACKET begins with NDIS_PACKET_PRIVATE; NdisQueryPacket reads Head
 * directly even in binary-compatible miniports. */
struct ndis_packet {
    uint32_t physical_count;
    uint32_t total_length;
    struct ndis_buffer *head;
};

/* Opaque NDIS_MINIPORT_INTERRUPT (80 bytes in the NT4 DDK) with headroom. */
struct ndis_miniport_interrupt {
    _Alignas(8) uint8_t opaque[128];
};

/* NDIS_MINIPORT_BLOCK.EthDB, "EXPOSED via macros. Do not move" in the DDK:
 * 216 bytes on x86 in the NT4 DDK and in the NDIS 5 layout. The receive
 * indication macros dereference it, so it is part of the miniport ABI. */
#define NDIS_MINIPORT_BLOCK_ETHDB 216u

#if !defined(RTL_HOST_TEST)
_Static_assert(sizeof(struct ndis_configuration_parameter) == 12,
               "NDIS_CONFIGURATION_PARAMETER size");
_Static_assert(sizeof(struct ndis_string) == 8, "NDIS_STRING size");
_Static_assert(offsetof(struct ndis_packet, head) == 8, "NDIS_PACKET Head");
#endif

typedef void (NDIS_CALL *ndis_shutdown_handler)(void *);
typedef ndis_boolean (NDIS_CALL *ndis_synchronize_fn)(void *);

NDIS_IMPORT void NDIS_CALL NdisOpenConfiguration(ndis_status *, ndis_handle *,
                                                 ndis_handle);
NDIS_IMPORT void NDIS_CALL NdisReadConfiguration(
    ndis_status *, struct ndis_configuration_parameter **, ndis_handle,
    const struct ndis_string *, uint32_t);
NDIS_IMPORT void NDIS_CALL NdisCloseConfiguration(ndis_handle);
NDIS_IMPORT void NDIS_CALL NdisReadNetworkAddress(ndis_status *, void **,
                                                  uint32_t *, ndis_handle);
NDIS_IMPORT uint32_t NDIS_CALL NdisReadPciSlotInformation(
    ndis_handle, uint32_t, uint32_t, void *, uint32_t);
NDIS_IMPORT uint32_t NDIS_CALL NdisWritePciSlotInformation(
    ndis_handle, uint32_t, uint32_t, void *, uint32_t);
NDIS_IMPORT ndis_status NDIS_CALL NdisMPciAssignResources(
    ndis_handle, uint32_t, struct ndis_resource_list **);
NDIS_IMPORT void NDIS_CALL NdisMSetAttributes(ndis_handle, void *,
                                              ndis_boolean, uint32_t);
NDIS_IMPORT ndis_status NDIS_CALL NdisMRegisterIoPortRange(
    void **, ndis_handle, uint32_t, uint32_t);
NDIS_IMPORT void NDIS_CALL NdisMDeregisterIoPortRange(ndis_handle, uint32_t,
                                                      uint32_t, void *);
NDIS_IMPORT ndis_status NDIS_CALL NdisMAllocateMapRegisters(
    ndis_handle, uint32_t, ndis_boolean, uint32_t, uint32_t);
NDIS_IMPORT void NDIS_CALL NdisMFreeMapRegisters(ndis_handle);
NDIS_IMPORT void NDIS_CALL NdisMAllocateSharedMemory(
    ndis_handle, uint32_t, ndis_boolean, void **, uint64_t *);
NDIS_IMPORT void NDIS_CALL NdisMFreeSharedMemory(ndis_handle, uint32_t,
                                                 ndis_boolean, void *, uint64_t);
NDIS_IMPORT ndis_status NDIS_CALL NdisMRegisterInterrupt(
    struct ndis_miniport_interrupt *, ndis_handle, uint32_t, uint32_t,
    ndis_boolean, ndis_boolean, uint32_t);
NDIS_IMPORT void NDIS_CALL NdisMDeregisterInterrupt(
    struct ndis_miniport_interrupt *);
NDIS_IMPORT ndis_boolean NDIS_CALL NdisMSynchronizeWithInterrupt(
    struct ndis_miniport_interrupt *, ndis_synchronize_fn, void *);
NDIS_IMPORT void NDIS_CALL NdisMRegisterAdapterShutdownHandler(
    ndis_handle, void *, ndis_shutdown_handler);
NDIS_IMPORT void NDIS_CALL NdisMDeregisterAdapterShutdownHandler(ndis_handle);
NDIS_IMPORT ndis_status NDIS_CALL NdisAllocateMemory(void **, uint32_t,
                                                    uint32_t, uint64_t);
NDIS_IMPORT void NDIS_CALL NdisFreeMemory(void *, uint32_t, uint32_t);
NDIS_IMPORT void NDIS_CALL NdisQueryBuffer(struct ndis_buffer *, void **,
                                           uint32_t *);
NDIS_IMPORT void NDIS_CALL NdisMSendResourcesAvailable(ndis_handle);
NDIS_IMPORT void NDIS_CALL EthFilterDprIndicateReceive(
    void *, ndis_handle, const void *, const void *, uint32_t, const void *,
    uint32_t, uint32_t);
NDIS_IMPORT void NDIS_CALL EthFilterDprIndicateReceiveComplete(void *);

#endif
