/* SPDX-License-Identifier: MIT */
#include "rtl8139.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint8_t regs[256];
    unsigned barriers;
    unsigned stalled;
    unsigned polls;
    unsigned cplus_order;
    unsigned command_order;
    unsigned ring_order;
    unsigned order;
    int reset_stuck;
    int autoload_stuck;
    int tally_stuck;
    int gone;            /* every read returns all ones */
    unsigned remove_on;  /* writing this register removes the chip */
    unsigned autoload_reads;
};

static _Alignas(256) struct rtl_descriptor rx_ring[RTL_RING_COUNT];
static _Alignas(256) struct rtl_descriptor tx_ring[RTL_RING_COUNT];
static _Alignas(64) struct rtl_tally tally;
static uint8_t rx_buffers[RTL_RING_COUNT][RTL_BUFFER_SIZE];
static uint8_t tx_buffers[RTL_RING_COUNT][RTL_BUFFER_SIZE];
static struct rtl_device device;
static struct model model;
static unsigned received;
static uint8_t last_frame[RTL_BUFFER_SIZE];
static unsigned last_size;

static uint8_t read8(void *ctx, unsigned reg)
{
    struct model *m = ctx;
    if (m->gone)
        return UINT8_MAX;
    if (reg == RTL_COMMAND && !m->reset_stuck && (m->regs[reg] & 0x10))
        m->regs[reg] = 0;
    if (reg == RTL_CONFIG_LOCK && (m->regs[reg] & 0xc0) == 0x40 &&
        !m->autoload_stuck && ++m->autoload_reads >= 3) {
        static const uint8_t eeprom_mac[6] = {0x00, 0xe0, 0x4c, 0x81, 0x39, 0x01};
        memcpy(m->regs + RTL_MAC, eeprom_mac, 6);
        m->regs[reg] = 0;
    }
    return m->regs[reg];
}

static uint16_t read16(void *ctx, unsigned reg)
{
    struct model *m = ctx;
    if (m->gone)
        return UINT16_MAX;
    return (uint16_t)((unsigned)m->regs[reg] | (unsigned)m->regs[reg + 1] << 8);
}

static uint32_t read32(void *ctx, unsigned reg)
{
    struct model *m = ctx;
    if (m->gone)
        return UINT32_MAX;
    return (uint32_t)m->regs[reg] | (uint32_t)m->regs[reg + 1] << 8 |
           (uint32_t)m->regs[reg + 2] << 16 | (uint32_t)m->regs[reg + 3] << 24;
}

static void store8(void *ctx, unsigned reg, uint8_t value)
{
    struct model *m = ctx;
    ++m->order;
    m->regs[reg] = value;
    if (m->remove_on && reg == m->remove_on)
        m->gone = 1;
    if (reg == RTL_COMMAND && value == 0x0c)
        m->command_order = m->order;
    if (reg == RTL_TX_POLL)
        ++m->polls;
}

/* IDR0-5 and MAR0-7 accept only 32-bit writes (datasheet 6.1). */
static void write8(void *ctx, unsigned reg, uint8_t value)
{
    assert(reg >= RTL_TALLY_COMMAND);
    store8(ctx, reg, value);
}

static void write16(void *ctx, unsigned reg, uint16_t value)
{
    struct model *m = ctx;
    if (reg == RTL_INTERRUPT_STATUS)
        value = read16(ctx, reg) & ~value;
    assert(reg >= RTL_TALLY_COMMAND);
    store8(ctx, reg, (uint8_t)value);
    store8(ctx, reg + 1, (uint8_t)(value >> 8));
    if (reg == RTL_CPLUS_COMMAND && value == 3)
        m->cplus_order = m->order;
}

static void write32(void *ctx, unsigned reg, uint32_t value)
{
    struct model *m = ctx;
    unsigned i;
    if (reg == RTL_TX_RING)
        m->ring_order = m->order + 1;
    if (reg == RTL_TALLY_COMMAND && (value & 0x08) && !m->tally_stuck) {
        assert((value & ~UINT32_C(0x3f)) == 0x140000);
        assert(read32(ctx, RTL_TALLY_COMMAND + 4) == 0);
        tally.tx_one_collision = 7;
        tally.rx_alignment = 3;
        value &= ~UINT32_C(0x08);
    }
    for (i = 0; i < 4; ++i)
        store8(ctx, reg + i, (uint8_t)(value >> (i * 8)));
}

static void stall(void *ctx, unsigned us)
{
    struct model *m = ctx;
    m->stalled += us;
}

static void barrier(void *ctx)
{
    struct model *m = ctx;
    ++m->barriers;
}

static void received_frame(void *ctx, const uint8_t *frame, unsigned size)
{
    (void)ctx;
    assert(size <= RTL_MAX_TAGGED_FRAME);
    ++received;
    last_size = size;
    memcpy(last_frame, frame, size);
}

static struct rtl_dma make_dma(void)
{
    struct rtl_dma dma = {
        .rx_ring = rx_ring, .tx_ring = tx_ring,
        .rx_ring_bus = 0x100000, .tx_ring_bus = 0x101000,
        .tally = &tally, .tally_bus = 0x140000
    };
    unsigned i;
    for (i = 0; i < RTL_RING_COUNT; ++i) {
        dma.rx_buffer[i] = rx_buffers[i];
        dma.tx_buffer[i] = tx_buffers[i];
        dma.rx_bus[i] = 0x110000 + i * RTL_BUFFER_SIZE;
        dma.tx_bus[i] = 0x130000 + i * RTL_BUFFER_SIZE;
    }
    return dma;
}

static struct rtl_bus make_bus(void)
{
    struct rtl_bus bus = {
        &model, read8, read16, read32, write8, write16, write32, stall, barrier
    };
    return bus;
}

static void setup(uint16_t vlan)
{
    struct rtl_bus bus = make_bus();
    struct rtl_dma dma = make_dma();
    struct rtl_config cfg = {.mac = {2, 0, 0, 0, 0, 1}, .vlan_id = vlan};
    static const uint8_t mac_regs[8] = {2, 0, 0, 0, 0, 1, 0, 0};
    memset(&model, 0, sizeof(model));
    memset(rx_buffers, 0, sizeof(rx_buffers));
    memset(tx_buffers, 0xaa, sizeof(tx_buffers));
    write32(&model, RTL_TX_CONFIG, 0x74800000);
    memset(model.regs + RTL_TX_HIGH_RING, 0xff, 8);
    received = 0;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_OK);
    assert(rtl_start(&device) == RTL_OK);
    assert(memcmp(model.regs + RTL_MAC, mac_regs, 8) == 0);
    assert(read32(&model, RTL_TX_HIGH_RING) == 0 &&
           read32(&model, RTL_TX_HIGH_RING + 4) == 0);
}

static void test_validation(void)
{
    struct rtl_bus bus = make_bus();
    struct rtl_dma dma = make_dma();
    struct rtl_config cfg = {.mac = {2, 0, 0, 0, 0, 1}};
    uint8_t zero_mac[6] = {0};
    assert(!rtl_valid_mac(zero_mac));
    assert(rtl_valid_mac(cfg.mac));
    cfg.mac[0] = 1;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    cfg.mac[0] = 2;
    cfg.vlan_id = 4095;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    cfg.vlan_id = 0;
    cfg.media = (enum rtl_media)-1;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    cfg.media = RTL_MEDIA_AUTO;
    dma.rx_ring_bus++;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    dma = make_dma();
    dma.tx_bus[63] = UINT32_MAX - 1;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    dma = make_dma();
    dma.rx_buffer[1] = NULL;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    dma = make_dma();
    dma.tally = NULL;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
    dma = make_dma();
    dma.tally_bus = 0x140001;
    assert(rtl_init(&device, &bus, &dma, &cfg) == RTL_INVALID);
}

static void test_initialization(void)
{
    unsigned i;
    setup(0);
    assert(model.cplus_order < model.command_order);
    assert(model.command_order < model.ring_order);
    assert(read32(&model, RTL_RX_RING) == 0x100000);
    assert(read16(&model, RTL_CPLUS_COMMAND) == 3);
    assert(read16(&model, RTL_BMCR) == 0x1200);
    assert(read16(&model, RTL_INTERRUPT_MASK) == 0);
    for (i = 0; i < RTL_RING_COUNT; ++i) {
        assert(rx_ring[i].status & RTL_OWN);
        assert(!!(rx_ring[i].status & RTL_EOR) == (i == RTL_RING_COUNT - 1));
        assert(!rx_ring[i].address_high);
        assert(tx_ring[i].status == (i == RTL_RING_COUNT - 1 ? RTL_EOR : 0));
    }
    assert(rtl_start(&device) == RTL_INVALID);
    assert(rtl_stop(&device) == RTL_OK);
    assert(!device.running);
    /* The model does not model read-only version bits. */
    write32(&model, RTL_TX_CONFIG, 0x74800000);
    assert(rtl_start(&device) == RTL_OK);
    model.reset_stuck = 1;
    assert(rtl_stop(&device) == RTL_TIMEOUT);
    assert(model.stalled == 10000);
    assert(device.faulted && !device.running);
    assert(rtl_start(&device) == RTL_INVALID);
    model.reset_stuck = 0;
    assert(rtl_stop(&device) == RTL_OK);
}

static void test_identify(void)
{
    struct rtl_bus bus = make_bus();
    uint8_t mac[6];
    unsigned mbps = 1;
    static const uint8_t expected[6] = {0x00, 0xe0, 0x4c, 0x81, 0x39, 0x01};
    memset(&model, 0, sizeof(model));
    write32(&model, RTL_TX_CONFIG, 0x74000000);
    assert(rtl_identify(&bus, mac) == RTL_UNSUPPORTED);
    assert(model.regs[RTL_CONFIG_LOCK] == 0); /* classic chip left untouched */
    write32(&model, RTL_TX_CONFIG, UINT32_MAX);
    assert(rtl_identify(&bus, mac) == RTL_IO_ERROR);
    write32(&model, RTL_TX_CONFIG, 0x74800000);
    assert(rtl_identify(&bus, mac) == RTL_OK);
    assert(!memcmp(mac, expected, 6));
    assert(model.autoload_reads == 3 && model.stalled == 20);
    model.autoload_stuck = 1;
    model.stalled = 0;
    assert(rtl_identify(&bus, mac) == RTL_TIMEOUT);
    assert(model.stalled == 20000);
    /* Removed while auto-loading. */
    model.autoload_stuck = 0;
    model.remove_on = RTL_CONFIG_LOCK;
    assert(rtl_identify(&bus, mac) == RTL_IO_ERROR);

    setup(0);
    model.regs[RTL_MEDIA_STATUS] = 0xd0;
    assert(rtl_link(&device, &mbps) == RTL_OK && mbps == 100);
    model.regs[RTL_MEDIA_STATUS] = 0xd8;
    assert(rtl_link(&device, &mbps) == RTL_OK && mbps == 10);
    model.regs[RTL_MEDIA_STATUS] = 0xd4;
    assert(rtl_link(&device, &mbps) == RTL_OK && mbps == 0);
    model.regs[RTL_MEDIA_STATUS] = 0xff;
    mbps = 1;
    assert(rtl_link(&device, &mbps) == RTL_IO_ERROR && mbps == 0);
}

static void test_tally(void)
{
    struct rtl_tally out;
    setup(0);
    assert(rtl_stop(&device) == RTL_OK);
    assert(rtl_dump_tally(&device, &out) == RTL_STOPPED);
    assert(rtl_start(&device) == RTL_OK);
    assert(rtl_dump_tally(&device, &out) == RTL_OK);
    assert(out.tx_one_collision == 7 && out.rx_alignment == 3);
    model.tally_stuck = 1;
    model.stalled = 0;
    assert(rtl_dump_tally(&device, &out) == RTL_TIMEOUT);
    assert(model.stalled == 1000);
    model.remove_on = RTL_TALLY_COMMAND;
    assert(rtl_dump_tally(&device, &out) == RTL_IO_ERROR);
}

static void test_transmit(void)
{
    uint8_t frame[RTL_MAX_FRAME] = {0};
    unsigned i, round, reaped = 0;
    setup(0);
    frame[12] = 8;
    assert(rtl_send(&device, frame, 13) == RTL_INVALID);
    assert(rtl_send(&device, frame, RTL_MAX_FRAME + 1) == RTL_INVALID);
    for (round = 0; round < 3; ++round) {
        for (i = 0; i < RTL_RING_COUNT; ++i) {
            unsigned size = i == 63 ? RTL_MAX_FRAME : 14;
            assert(rtl_send(&device, frame, size) == RTL_OK);
            assert(!memcmp(tx_buffers[i], frame, size));
            assert((tx_ring[i].status & RTL_LENGTH_MASK) ==
                   (size < 60 ? 60 : size));
            if (size < 60) {
                unsigned j;
                for (j = size; j < 60; ++j)
                    assert(tx_buffers[i][j] == 0);
            }
        }
        assert(rtl_send(&device, frame, 60) == RTL_FULL);
        assert(rtl_reap_tx(&device) == 0);
        for (i = 0; i < RTL_RING_COUNT; ++i) {
            tx_ring[i].status &= ~RTL_OWN;
            if (i == 5)
                tx_ring[i].status |= RTL_TX_ERROR;
        }
        reaped += rtl_reap_tx(&device);
        assert(rtl_reap_tx(&device) == 0);
        assert(!device.tx_count);
    }
    assert(reaped == 192 && model.polls == 192);
    assert(device.stats.tx_ok == 189 && device.stats.tx_error == 3);
    assert(rtl_stop(&device) == RTL_OK);
    assert(rtl_send(&device, frame, 60) == RTL_STOPPED);
}

static void test_receive(void)
{
    unsigned i, round;
    setup(0);
    for (round = 0; round < 3; ++round) {
        for (i = 0; i < RTL_RING_COUNT; ++i) {
            memset(rx_buffers[i], (int)i, 64);
            rx_ring[i].status = RTL_FS | RTL_LS | 64;
            if (i == 63)
                rx_ring[i].status |= RTL_EOR;
        }
        rx_ring[5].status |= RTL_RX_ERROR;
        rx_ring[6].status = RTL_FS | 64; /* fragmented packet */
        rx_ring[7].status = RTL_FS | RTL_LS | 17;
        rx_ring[8].status = RTL_FS | RTL_LS | 8191;
        assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 1);
        assert(device.rx_head == 1);
        assert(rtl_poll_rx(&device, 1000, received_frame, NULL) == 63);
        assert(device.rx_head == 0);
        assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 0);
        for (i = 0; i < RTL_RING_COUNT; ++i)
            assert(rx_ring[i].status & RTL_OWN);
    }
    assert(received == 180 && device.stats.rx_error == 12);
    assert(last_size == 60 && last_frame[0] == 63);
}

static void test_vlan(void)
{
    uint8_t frame[RTL_MAX_FRAME] = {0};
    setup(4094);
    frame[12] = 8;
    assert(rtl_send(&device, frame, sizeof(frame)) == RTL_OK);
    assert((tx_ring[0].status & RTL_LENGTH_MASK) == RTL_MAX_TAGGED_FRAME);
    assert(tx_buffers[0][12] == 0x81 && tx_buffers[0][13] == 0);
    assert(tx_buffers[0][14] == 0x0f && tx_buffers[0][15] == 0xfe);
    assert(tx_buffers[0][16] == 8 && tx_buffers[0][17] == 0);
    frame[12] = 0x81;
    assert(rtl_send(&device, frame, sizeof(frame)) == RTL_INVALID);
    frame[12] = 0x88;
    frame[13] = 0xa8;
    assert(rtl_send(&device, frame, sizeof(frame)) == RTL_INVALID);
    memcpy(rx_buffers[0], tx_buffers[0], RTL_MAX_TAGGED_FRAME);
    rx_buffers[0][14] |= 0xe0; /* priority does not change VLAN membership */
    rx_ring[0].status = RTL_FS | RTL_LS | (RTL_MAX_TAGGED_FRAME + 4);
    assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 1);
    assert(received == 1 && last_size == RTL_MAX_FRAME);
    assert(last_frame[12] == 8);
    rx_ring[1].status = RTL_FS | RTL_LS | 64; /* untagged access traffic */
    assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 1);
    memcpy(rx_buffers[2], tx_buffers[0], RTL_MAX_TAGGED_FRAME);
    rx_buffers[2][15] = 1; /* different VLAN */
    rx_ring[2].status = RTL_FS | RTL_LS | (RTL_MAX_TAGGED_FRAME + 4);
    assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 1);
    assert(received == 1 && device.stats.rx_ok == 1);
    memcpy(rx_buffers[3], tx_buffers[0], RTL_MAX_TAGGED_FRAME);
    rx_buffers[3][16] = 0x88;
    rx_buffers[3][17] = 0xa8;
    rx_ring[3].status = RTL_FS | RTL_LS | (RTL_MAX_TAGGED_FRAME + 4);
    assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 1);
    rx_ring[4].status = RTL_FS | RTL_LS | 18;
    assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 1);
    assert(received == 1 && device.stats.rx_ok == 1 && !device.stats.rx_error);

    /* Without VLAN, nothing longer than the advertised 1514 bytes is
       delivered; a tagged frame within that limit passes unchanged. */
    memcpy(frame, tx_buffers[0], sizeof(frame));
    setup(0);
    memcpy(rx_buffers[0], frame, sizeof(frame));
    rx_ring[0].status = RTL_FS | RTL_LS | (RTL_MAX_TAGGED_FRAME + 4);
    memcpy(rx_buffers[1], frame, sizeof(frame));
    rx_ring[1].status = RTL_FS | RTL_LS | (RTL_MAX_FRAME + 4);
    assert(rtl_poll_rx(&device, 2, received_frame, NULL) == 2);
    assert(received == 1 && last_size == RTL_MAX_FRAME);
    assert(last_frame[12] == 0x81 && !device.stats.rx_error);
}

static void test_filters(void)
{
    const uint8_t group[6] = {1, 0, 0x5e, 0, 0, 1};
    uint8_t bad_group[6] = {2, 0, 0, 0, 0, 1};
    setup(0);
    /* CRC-32 of 01:00:5e:00:00:01, top six bits: hash bit 31. */
    assert(rtl_set_filter(&device, RTL_FILTER_MULTICAST, group, 1) == RTL_OK);
    assert(read32(&model, RTL_MULTICAST) == (UINT32_C(1) << 31));
    assert(read32(&model, RTL_RX_CONFIG) == 0xa404);
    assert(rtl_set_filter(&device, 16, NULL, 0) == RTL_INVALID);
    assert(rtl_set_filter(&device, 2, bad_group, 1) == RTL_INVALID);
    assert(rtl_set_filter(&device, RTL_FILTER_PROMISCUOUS, NULL, 0) == RTL_OK);
    assert(read32(&model, RTL_MULTICAST) == UINT32_MAX);
    assert(read32(&model, RTL_MULTICAST + 4) == UINT32_MAX);
    assert(read32(&model, RTL_RX_CONFIG) == 0xa40f);
    assert(rtl_set_filter(&device, RTL_FILTER_ALL_MULTICAST, NULL, 0) == RTL_OK);
    assert(read32(&model, RTL_MULTICAST) == UINT32_MAX);
    assert(read32(&model, RTL_MULTICAST + 4) == UINT32_MAX);
    assert(read32(&model, RTL_RX_CONFIG) == 0xa404);
    assert(rtl_set_filter(&device, 0, NULL, 0) == RTL_OK);
    assert(read32(&model, RTL_RX_CONFIG) == 0xa400);
}

static void raise_interrupt(uint16_t bits)
{
    uint16_t pending = read16(&model, RTL_INTERRUPT_STATUS) | bits;
    model.regs[RTL_INTERRUPT_STATUS] = (uint8_t)pending;
    model.regs[RTL_INTERRUPT_STATUS + 1] = (uint8_t)(pending >> 8);
}

static void test_interrupts(void)
{
    uint8_t frame[60] = {0};
    uint16_t pending = UINT16_MAX;
    unsigned order, mbps = 1;
    setup(0);
    assert(rtl_read_interrupts(&device, &pending) == RTL_OK && !pending);
    raise_interrupt(RTL_INT_RX_OK | RTL_INT_TX_OK);
    assert(read16(&model, RTL_INTERRUPT_MASK) == 0);
    assert(rtl_read_interrupts(&device, &pending) == RTL_OK);
    assert(pending == (RTL_INT_RX_OK | RTL_INT_TX_OK));
    assert(read16(&model, RTL_INTERRUPT_STATUS) == pending);
    /* A new source after the snapshot must survive its acknowledgement. */
    raise_interrupt(RTL_INT_LINK_CHANGE);
    assert(rtl_ack_interrupts(&device, pending) == RTL_OK);
    assert(read16(&model, RTL_INTERRUPT_STATUS) == RTL_INT_LINK_CHANGE);
    /* Nothing to acknowledge (reserved bits only): no register write. */
    order = model.order;
    assert(rtl_ack_interrupts(&device, 0) == RTL_OK);
    assert(rtl_ack_interrupts(&device, 0x1000) == RTL_OK);
    assert(model.order == order);
    /* Status is reported whatever the mask. */
    write16(&model, RTL_INTERRUPT_MASK, RTL_INT_ALL);
    assert(rtl_read_interrupts(&device, &pending) == RTL_OK);
    assert(pending == RTL_INT_LINK_CHANGE);
    write16(&model, RTL_INTERRUPT_MASK, 0);
    assert(read16(&model, RTL_INTERRUPT_STATUS) == pending);
    assert(rtl_ack_interrupts(&device, pending) == RTL_OK);
    assert(rtl_read_interrupts(&device, &pending) == RTL_OK && !pending);
    /* Reserved bits are neither reported nor acknowledged. */
    raise_interrupt(0x0200 | RTL_INT_SYSTEM_ERROR);
    assert(rtl_read_interrupts(&device, &pending) == RTL_OK);
    assert(pending == RTL_INT_SYSTEM_ERROR);
    assert(rtl_ack_interrupts(&device, pending) == RTL_OK);
    assert(read16(&model, RTL_INTERRUPT_STATUS) == 0x0200);
    /* Surprise removal: all-ones status masks the chip and faults the core. */
    write16(&model, RTL_INTERRUPT_MASK, RTL_INT_RX_OK);
    raise_interrupt(UINT16_MAX);
    assert(rtl_read_interrupts(&device, &pending) == RTL_IO_ERROR);
    assert(!pending && device.faulted);
    assert(!read16(&model, RTL_INTERRUPT_MASK));
    assert(rtl_ack_interrupts(&device, RTL_INT_ALL) == RTL_IO_ERROR);
    /* Once faulted, nothing is read back even if registers look sane. */
    model.regs[RTL_INTERRUPT_STATUS] = model.regs[RTL_INTERRUPT_STATUS + 1] = 0;
    model.regs[RTL_MEDIA_STATUS] = 0xd0;
    assert(rtl_read_interrupts(&device, &pending) == RTL_IO_ERROR && !pending);
    assert(rtl_link(&device, &mbps) == RTL_IO_ERROR && mbps == 0);
    order = model.order;
    assert(rtl_send(&device, frame, sizeof(frame)) == RTL_IO_ERROR);
    assert(rtl_set_filter(&device, RTL_FILTER_DIRECTED, NULL, 0) == RTL_IO_ERROR);
    rx_ring[0].status = RTL_FS | RTL_LS | 64;
    assert(rtl_poll_rx(&device, 1, received_frame, NULL) == 0);
    device.tx_count = 1;
    assert(rtl_reap_tx(&device) == 0);
    assert(device.tx_count == 1 && !received);
    assert(model.order == order);
    assert(rtl_stop(&device) == RTL_OK);
    assert(rtl_read_interrupts(&device, &pending) == RTL_STOPPED && !pending);
    assert(rtl_ack_interrupts(&device, RTL_INT_RX_OK) == RTL_STOPPED);
    assert(rtl_set_filter(&device, RTL_FILTER_DIRECTED, NULL, 0) == RTL_STOPPED);
}

int main(void)
{
    test_validation();
    test_initialization();
    test_identify();
    test_tally();
    test_transmit();
    test_receive();
    test_vlan();
    test_filters();
    test_interrupts();
    puts("core: validation, reset, DMA rings, TX/RX wrap, VLAN, filters, interrupt registers passed");
    return 0;
}
