/* SPDX-License-Identifier: MIT */
#ifndef RTL8139_H
#define RTL8139_H

#include <stddef.h>
#include <stdint.h>

#define RTL_RING_COUNT 64u
#define RTL_BUFFER_SIZE 1536u
#define RTL_MAX_FRAME 1514u
#define RTL_MAX_TAGGED_FRAME 1518u
#define RTL_MIN_FRAME 60u

#define RTL_OWN UINT32_C(0x80000000)
#define RTL_EOR UINT32_C(0x40000000)
#define RTL_FS UINT32_C(0x20000000)
#define RTL_LS UINT32_C(0x10000000)
#define RTL_RX_ERROR UINT32_C(0x00100000)
#define RTL_TX_ERROR UINT32_C(0x00800000)
#define RTL_LENGTH_MASK UINT32_C(0x00001fff)

enum rtl_register {
    RTL_MAC = 0x00,
    RTL_MULTICAST = 0x08,
    RTL_TALLY_COMMAND = 0x10,
    RTL_TX_RING = 0x20,
    RTL_TX_HIGH_RING = 0x28,
    RTL_COMMAND = 0x37,
    RTL_INTERRUPT_MASK = 0x3c,
    RTL_INTERRUPT_STATUS = 0x3e,
    RTL_TX_CONFIG = 0x40,
    RTL_RX_CONFIG = 0x44,
    RTL_TIMER = 0x48,
    RTL_MISSED = 0x4c,
    RTL_CONFIG_LOCK = 0x50, /* 9346CR: EEPROM mode and config-write unlock */
    RTL_MEDIA_STATUS = 0x58,
    RTL_BMCR = 0x62,
    RTL_ADVERTISE = 0x66,
    RTL_TX_POLL = 0xd9,
    RTL_CPLUS_COMMAND = 0xe0,
    RTL_INTERRUPT_MITIGATION = 0xe2,
    RTL_RX_RING = 0xe4,
    RTL_TX_THRESHOLD = 0xec
};

enum rtl_result {
    RTL_OK,
    RTL_INVALID,
    RTL_UNSUPPORTED,
    RTL_TIMEOUT,
    RTL_STOPPED,
    RTL_FULL,
    RTL_IO_ERROR
};

enum rtl_media {
    RTL_MEDIA_AUTO,
    RTL_MEDIA_10_HALF,
    RTL_MEDIA_10_FULL,
    RTL_MEDIA_100_HALF,
    RTL_MEDIA_100_FULL
};

enum rtl_filter {
    RTL_FILTER_DIRECTED = 1,
    RTL_FILTER_MULTICAST = 2,
    RTL_FILTER_ALL_MULTICAST = 4,
    RTL_FILTER_BROADCAST = 8,
    RTL_FILTER_PROMISCUOUS = 32
};

enum rtl_interrupt {
    RTL_INT_RX_OK = 0x0001,
    RTL_INT_RX_ERROR = 0x0002,
    RTL_INT_TX_OK = 0x0004,
    RTL_INT_TX_ERROR = 0x0008,
    RTL_INT_RX_UNAVAILABLE = 0x0010,
    RTL_INT_LINK_CHANGE = 0x0020,
    RTL_INT_RX_FIFO = 0x0040,
    RTL_INT_SYSTEM_ERROR = 0x8000,
    RTL_INT_ALL = 0xe1ff /* every C+ ISR source (datasheet 6.7) */
};

struct rtl_descriptor {
    volatile uint32_t status;
    uint32_t vlan;
    uint32_t address_low;
    uint32_t address_high;
};

_Static_assert(sizeof(struct rtl_descriptor) == 16, "C+ descriptor size");
_Static_assert(offsetof(struct rtl_descriptor, address_low) == 8,
               "C+ descriptor address");

/* Datasheet 6.3 tally counter dump; 64 bytes, 64-byte aligned. */
struct rtl_tally {
    uint64_t tx_ok;
    uint64_t rx_ok;
    uint64_t tx_error;
    uint32_t rx_error;
    uint16_t rx_missed;
    uint16_t rx_alignment;
    uint32_t tx_one_collision;
    uint32_t tx_multi_collision;
    uint64_t rx_unicast;
    uint64_t rx_broadcast;
    uint32_t rx_multicast;
    uint16_t tx_abort;
    uint16_t tx_underrun;
};

_Static_assert(sizeof(struct rtl_tally) == 64, "tally counter size");
_Static_assert(offsetof(struct rtl_tally, tx_one_collision) == 32,
               "tally counter collision offset");
_Static_assert(offsetof(struct rtl_tally, tx_abort) == 60,
               "tally counter abort offset");

/* All DMA mappings must be coherent, pinned, non-overlapping, and below 4GB.
 * The platform serializes calls and provides a full DMA ordering barrier. */
struct rtl_dma {
    struct rtl_descriptor *rx_ring;
    struct rtl_descriptor *tx_ring;
    uint32_t rx_ring_bus;
    uint32_t tx_ring_bus;
    uint8_t *rx_buffer[RTL_RING_COUNT];
    uint8_t *tx_buffer[RTL_RING_COUNT];
    uint32_t rx_bus[RTL_RING_COUNT];
    uint32_t tx_bus[RTL_RING_COUNT];
    volatile struct rtl_tally *tally;
    uint32_t tally_bus;
};

struct rtl_bus {
    void *context;
    uint8_t (*read8)(void *, unsigned);
    uint16_t (*read16)(void *, unsigned);
    uint32_t (*read32)(void *, unsigned);
    void (*write8)(void *, unsigned, uint8_t);
    void (*write16)(void *, unsigned, uint16_t);
    void (*write32)(void *, unsigned, uint32_t);
    void (*stall_us)(void *, unsigned);
    void (*barrier)(void *);
};

struct rtl_config {
    uint8_t mac[6];
    enum rtl_media media;
    uint16_t vlan_id; /* zero disables the static access VLAN */
};

struct rtl_stats {
    uint32_t tx_ok;
    uint32_t tx_error;
    uint32_t rx_ok;
    uint32_t rx_error;
};

struct rtl_device {
    struct rtl_bus bus;
    struct rtl_dma dma;
    struct rtl_config config;
    struct rtl_stats stats;
    unsigned tx_head;
    unsigned tx_tail;
    unsigned tx_count;
    unsigned rx_head;
    unsigned running;
    unsigned faulted;
};

/* Buffer is borrowed only for the duration of this synchronous callback.
 * Reentry into the device, and retaining this pointer, are not permitted. */
typedef void (*rtl_receive_fn)(void *, const uint8_t *, unsigned);

int rtl_valid_mac(const uint8_t mac[6]);
/* Before rtl_init only: verifies the C+ engine, reloads the EEPROM (which
 * resets every register) and returns the factory station address. */
enum rtl_result rtl_identify(const struct rtl_bus *, uint8_t mac[6]);
/* Reports the PHY link from MSR; *mbps is 0 without link, else 10 or 100. */
enum rtl_result rtl_link(struct rtl_device *, unsigned *mbps);
enum rtl_result rtl_init(struct rtl_device *, const struct rtl_bus *,
                         const struct rtl_dma *, const struct rtl_config *);
enum rtl_result rtl_start(struct rtl_device *);
enum rtl_result rtl_stop(struct rtl_device *);
/* The platform owns the interrupt mask (start leaves it cleared) and must
 * synchronize these with its ISR. Status includes masked events; acknowledge
 * only a saved snapshot, then drain descriptor work before re-enabling. */
enum rtl_result rtl_read_interrupts(struct rtl_device *, uint16_t *);
enum rtl_result rtl_ack_interrupts(struct rtl_device *, uint16_t);
enum rtl_result rtl_set_filter(struct rtl_device *, uint32_t,
                               const uint8_t *, unsigned);
enum rtl_result rtl_send(struct rtl_device *, const uint8_t *, unsigned);
unsigned rtl_reap_tx(struct rtl_device *);
unsigned rtl_poll_rx(struct rtl_device *, unsigned, rtl_receive_fn, void *);
/* Bounded (about 1 ms) busy wait; the counters reset only with the chip. */
enum rtl_result rtl_dump_tally(struct rtl_device *, struct rtl_tally *);

#endif
