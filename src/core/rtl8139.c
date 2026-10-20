/* SPDX-License-Identifier: MIT */
#include "rtl8139.h"

static void copy_bytes(uint8_t *dst, const uint8_t *src, unsigned count)
{
    while (count--)
        *dst++ = *src++;
}

static int vlan_type(const uint8_t *type)
{
    return (type[0] == 0x81 && type[1] == 0) ||
           (type[0] == 0x88 && type[1] == 0xa8);
}

int rtl_valid_mac(const uint8_t mac[6])
{
    unsigned i;
    uint8_t any = 0;
    if (mac[0] & 1)
        return 0;
    for (i = 0; i < 6; ++i)
        any |= mac[i];
    return any != 0;
}

/* 10EC:8139 also matches the incompatible classic chips; the TxConfig
 * hardware version identifies the C+. */
static int is_cplus(uint32_t tx_config)
{
    return (tx_config & UINT32_C(0x7cc00000)) == UINT32_C(0x74800000);
}

static uint32_t multicast_hash(const uint8_t mac[6])
{
    uint32_t crc = UINT32_MAX;
    unsigned i, bit;
    for (i = 0; i < 6; ++i) {
        uint8_t octet = mac[i];
        for (bit = 0; bit < 8; ++bit) {
            unsigned feedback = ((crc >> 31) ^ octet) & 1u;
            crc <<= 1;
            if (feedback)
                crc ^= UINT32_C(0x04c11db7);
            octet >>= 1;
        }
    }
    return crc >> 26;
}

enum rtl_result rtl_identify(const struct rtl_bus *b, uint8_t mac[6])
{
    unsigned i;
    uint32_t tx_config = b->read32(b->context, RTL_TX_CONFIG);
    if (tx_config == UINT32_MAX)
        return RTL_IO_ERROR;
    if (!is_cplus(tx_config))
        return RTL_UNSUPPORTED;
    /* 9346CR auto-load takes about 2 ms and then returns to normal mode. */
    b->write8(b->context, RTL_CONFIG_LOCK, 0x40);
    for (i = 0; i < 2000; ++i) {
        uint8_t mode = b->read8(b->context, RTL_CONFIG_LOCK);
        if (mode == UINT8_MAX)
            return RTL_IO_ERROR;
        if (!(mode & 0xc0))
            break;
        b->stall_us(b->context, 10);
    }
    if (i == 2000)
        return RTL_TIMEOUT;
    for (i = 0; i < 6; ++i)
        mac[i] = b->read8(b->context, RTL_MAC + i);
    return RTL_OK;
}

enum rtl_result rtl_link(struct rtl_device *d, unsigned *mbps)
{
    uint8_t status;
    *mbps = 0;
    if (d->faulted)
        return RTL_IO_ERROR;
    status = d->bus.read8(d->bus.context, RTL_MEDIA_STATUS);
    if (status == UINT8_MAX)
        return RTL_IO_ERROR;
    if (!(status & 0x04)) /* LINKB is the inverse of link status */
        *mbps = status & 0x08 ? 10 : 100;
    return RTL_OK;
}

static int valid_bus_range(uint32_t address, uint32_t size)
{
    return address != 0 && address <= UINT32_MAX - (size - 1);
}

enum rtl_result rtl_init(struct rtl_device *d, const struct rtl_bus *bus,
                         const struct rtl_dma *dma, const struct rtl_config *cfg)
{
    unsigned i;
    if (!rtl_valid_mac(cfg->mac) || cfg->vlan_id > 4094 ||
        (unsigned)cfg->media > RTL_MEDIA_100_FULL ||
        !dma->rx_ring || !dma->tx_ring ||
        ((uintptr_t)dma->rx_ring & 255) || ((uintptr_t)dma->tx_ring & 255) ||
        (dma->rx_ring_bus & 255) || (dma->tx_ring_bus & 255) ||
        !valid_bus_range(dma->rx_ring_bus, RTL_RING_COUNT * 16) ||
        !valid_bus_range(dma->tx_ring_bus, RTL_RING_COUNT * 16) ||
        !dma->tally || ((uintptr_t)dma->tally & 63) || (dma->tally_bus & 63) ||
        !valid_bus_range(dma->tally_bus, 64))
        return RTL_INVALID;
    for (i = 0; i < RTL_RING_COUNT; ++i) {
        if (!dma->rx_buffer[i] || !dma->tx_buffer[i] ||
            !valid_bus_range(dma->rx_bus[i], RTL_BUFFER_SIZE) ||
            !valid_bus_range(dma->tx_bus[i], RTL_BUFFER_SIZE))
            return RTL_INVALID;
    }
    d->bus = *bus;
    d->dma = *dma;
    d->config = *cfg;
    d->stats = (struct rtl_stats){0};
    d->tx_head = d->tx_tail = d->tx_count = d->rx_head = 0;
    d->running = d->faulted = 0;
    return RTL_OK;
}

static enum rtl_result reset(struct rtl_device *d)
{
    unsigned retry;
    struct rtl_bus *b = &d->bus;
    b->write16(b->context, RTL_INTERRUPT_MASK, 0);
    b->write8(b->context, RTL_COMMAND, 0x10);
    for (retry = 0; retry < 1000; ++retry) {
        if (!(b->read8(b->context, RTL_COMMAND) & 0x10)) {
            b->write16(b->context, RTL_CPLUS_COMMAND, 0);
            b->barrier(b->context);
            return RTL_OK;
        }
        b->stall_us(b->context, 10);
    }
    return RTL_TIMEOUT;
}

enum rtl_result rtl_stop(struct rtl_device *d)
{
    enum rtl_result result;
    d->running = 0;
    result = reset(d);
    d->faulted = result != RTL_OK;
    /* On timeout the platform must disable PCI bus mastering and confirm
     * quiescence before releasing any of the DMA mappings. */
    return result;
}

enum rtl_result rtl_start(struct rtl_device *d)
{
    unsigned i;
    uint16_t bmcr;
    enum rtl_result result;
    struct rtl_bus *b = &d->bus;
    if (d->running || d->faulted)
        return RTL_INVALID;
    result = reset(d);
    if (result != RTL_OK) {
        d->faulted = 1;
        return result;
    }
    d->tx_head = d->tx_tail = d->tx_count = d->rx_head = 0;
    for (i = 0; i < RTL_RING_COUNT; ++i) {
        uint32_t end = i == RTL_RING_COUNT - 1 ? RTL_EOR : 0;
        struct rtl_descriptor *rx = &d->dma.rx_ring[i];
        struct rtl_descriptor *tx = &d->dma.tx_ring[i];
        rx->vlan = tx->vlan = 0;
        rx->address_low = d->dma.rx_bus[i];
        tx->address_low = d->dma.tx_bus[i];
        rx->address_high = tx->address_high = 0;
        tx->status = end;
        b->barrier(b->context);
        rx->status = RTL_OWN | end | RTL_BUFFER_SIZE;
    }
    b->barrier(b->context);
    b->write16(b->context, RTL_CPLUS_COMMAND, 3);
    /* The data sheet requires C+ enable, then command, then ring/config
     * registers. Keep receive filtering off until all addresses are valid. */
    b->write8(b->context, RTL_COMMAND, 0x0c);
    b->write32(b->context, RTL_TX_RING, d->dma.tx_ring_bus);
    b->write32(b->context, RTL_TX_RING + 4, 0);
    b->write32(b->context, RTL_TX_HIGH_RING, 0);
    b->write32(b->context, RTL_TX_HIGH_RING + 4, 0);
    b->write32(b->context, RTL_RX_RING, d->dma.rx_ring_bus);
    b->write32(b->context, RTL_RX_RING + 4, 0);
    b->write32(b->context, RTL_TX_CONFIG, UINT32_C(0x03000600));
    b->write32(b->context, RTL_RX_CONFIG, UINT32_C(0x0000a400));
    b->write16(b->context, RTL_INTERRUPT_MITIGATION, 0);
    b->write8(b->context, RTL_TX_THRESHOLD, 16);
    b->write32(b->context, RTL_MISSED, 0);
    b->write8(b->context, RTL_CONFIG_LOCK, 0xc0);
    /* IDR0-5 accept only 32-bit writes (datasheet 6.1). */
    b->write32(b->context, RTL_MAC,
               (uint32_t)d->config.mac[0] | (uint32_t)d->config.mac[1] << 8 |
                   (uint32_t)d->config.mac[2] << 16 |
                   (uint32_t)d->config.mac[3] << 24);
    b->write32(b->context, RTL_MAC + 4,
               (uint32_t)d->config.mac[4] | (uint32_t)d->config.mac[5] << 8);
    if (d->config.media == RTL_MEDIA_AUTO) {
        b->write16(b->context, RTL_ADVERTISE, 0x01e1);
        bmcr = 0x1200;
    } else {
        bmcr = d->config.media >= RTL_MEDIA_100_HALF ? 0x2000 : 0;
        if (d->config.media == RTL_MEDIA_10_FULL ||
            d->config.media == RTL_MEDIA_100_FULL)
            bmcr |= 0x0100;
    }
    b->write16(b->context, RTL_BMCR, bmcr);
    b->write8(b->context, RTL_CONFIG_LOCK, 0);
    b->write16(b->context, RTL_INTERRUPT_STATUS, 0xffff);
    b->barrier(b->context);
    d->running = 1;
    return RTL_OK;
}

enum rtl_result rtl_read_interrupts(struct rtl_device *d, uint16_t *pending)
{
    uint16_t status;
    *pending = 0;
    if (!d->running)
        return RTL_STOPPED;
    if (d->faulted)
        return RTL_IO_ERROR;
    status = d->bus.read16(d->bus.context, RTL_INTERRUPT_STATUS);
    if (status == UINT16_MAX) {
        d->faulted = 1;
        d->bus.write16(d->bus.context, RTL_INTERRUPT_MASK, 0);
        return RTL_IO_ERROR;
    }
    *pending = status & RTL_INT_ALL;
    return RTL_OK;
}

enum rtl_result rtl_ack_interrupts(struct rtl_device *d, uint16_t pending)
{
    if (!d->running)
        return RTL_STOPPED;
    if (d->faulted)
        return RTL_IO_ERROR;
    if (pending & RTL_INT_ALL) {
        d->bus.write16(d->bus.context, RTL_INTERRUPT_STATUS,
                       pending & RTL_INT_ALL);
        d->bus.barrier(d->bus.context);
    }
    return RTL_OK;
}

enum rtl_result rtl_set_filter(struct rtl_device *d, uint32_t flags,
                               const uint8_t *addresses, unsigned count)
{
    uint32_t hash[2] = {0, 0};
    uint32_t accept = 0;
    unsigned i;
    struct rtl_bus *b;
    if (count > 32 || (flags & ~UINT32_C(47)))
        return RTL_INVALID;
    if (!d->running)
        return RTL_STOPPED;
    if (d->faulted)
        return RTL_IO_ERROR;
    for (i = 0; i < count; ++i) {
        unsigned index;
        if (!(addresses[i * 6] & 1))
            return RTL_INVALID;
        index = multicast_hash(addresses + i * 6);
        hash[index >> 5] |= UINT32_C(1) << (index & 31);
    }
    if (flags & RTL_FILTER_DIRECTED)
        accept |= 2;
    if (flags & RTL_FILTER_BROADCAST)
        accept |= 8;
    if (flags & (RTL_FILTER_MULTICAST | RTL_FILTER_ALL_MULTICAST))
        accept |= 4;
    if (flags & RTL_FILTER_ALL_MULTICAST)
        hash[0] = hash[1] = UINT32_MAX;
    if (flags & RTL_FILTER_PROMISCUOUS) {
        accept = 15;
        hash[0] = hash[1] = UINT32_MAX;
    }
    b = &d->bus;
    b->write32(b->context, RTL_MULTICAST, hash[0]);
    b->write32(b->context, RTL_MULTICAST + 4, hash[1]);
    b->write32(b->context, RTL_RX_CONFIG, UINT32_C(0x0000a400) | accept);
    return RTL_OK;
}

enum rtl_result rtl_send(struct rtl_device *d, const uint8_t *frame, unsigned size)
{
    unsigned length, i, index;
    uint8_t *out;
    struct rtl_descriptor *desc;
    if (size < 14 || size > RTL_MAX_FRAME)
        return RTL_INVALID;
    if (!d->running)
        return RTL_STOPPED;
    if (d->faulted)
        return RTL_IO_ERROR;
    if (d->config.vlan_id && vlan_type(frame + 12))
        return RTL_INVALID;
    if (d->tx_count == RTL_RING_COUNT)
        return RTL_FULL;
    index = d->tx_head;
    desc = &d->dma.tx_ring[index];
    if (desc->status & RTL_OWN) {
        d->faulted = 1;
        return RTL_IO_ERROR;
    }
    d->bus.barrier(d->bus.context);
    out = d->dma.tx_buffer[index];
    if (d->config.vlan_id) {
        copy_bytes(out, frame, 12);
        out[12] = 0x81;
        out[13] = 0;
        out[14] = (uint8_t)(d->config.vlan_id >> 8);
        out[15] = (uint8_t)d->config.vlan_id;
        copy_bytes(out + 16, frame + 12, size - 12);
        length = size + 4;
    } else {
        copy_bytes(out, frame, size);
        length = size;
    }
    for (i = length; i < RTL_MIN_FRAME; ++i)
        out[i] = 0;
    if (length < RTL_MIN_FRAME)
        length = RTL_MIN_FRAME;
    desc->vlan = 0;
    d->bus.barrier(d->bus.context);
    desc->status = RTL_OWN | RTL_FS | RTL_LS | length |
                   (index == RTL_RING_COUNT - 1 ? RTL_EOR : 0);
    d->tx_head = (index + 1) % RTL_RING_COUNT;
    ++d->tx_count;
    d->bus.barrier(d->bus.context);
    d->bus.write8(d->bus.context, RTL_TX_POLL, 0x40);
    return RTL_OK;
}

unsigned rtl_reap_tx(struct rtl_device *d)
{
    unsigned count = 0;
    if (!d->running || d->faulted)
        return 0;
    while (d->tx_count) {
        uint32_t status = d->dma.tx_ring[d->tx_tail].status;
        if (status & RTL_OWN)
            break;
        d->bus.barrier(d->bus.context);
        if (status & RTL_TX_ERROR)
            ++d->stats.tx_error;
        else
            ++d->stats.tx_ok;
        d->tx_tail = (d->tx_tail + 1) % RTL_RING_COUNT;
        --d->tx_count;
        ++count;
    }
    return count;
}

unsigned rtl_poll_rx(struct rtl_device *d, unsigned budget,
                     rtl_receive_fn receive, void *context)
{
    unsigned count = 0;
    if (!d->running || d->faulted)
        return 0;
    while (count < budget) {
        unsigned index = d->rx_head;
        struct rtl_descriptor *desc = &d->dma.rx_ring[index];
        uint32_t status = desc->status;
        unsigned size = status & RTL_LENGTH_MASK;
        uint8_t *frame = d->dma.rx_buffer[index];
        if (status & RTL_OWN)
            break;
        d->bus.barrier(d->bus.context);
        if ((status & (RTL_FS | RTL_LS | RTL_RX_ERROR)) != (RTL_FS | RTL_LS) ||
            size < 18 || size > RTL_MAX_TAGGED_FRAME + 4) {
            ++d->stats.rx_error;
        } else {
            int accepted;
            size -= 4; /* descriptor length includes the Ethernet FCS */
            if (!d->config.vlan_id) {
                /* Tagged frames beyond the advertised maximum are dropped. */
                accepted = size <= RTL_MAX_FRAME;
            } else {
                /* Access port: only untagged-inside frames of our VLAN. */
                unsigned tag = (unsigned)frame[14] << 8 | frame[15];
                accepted = size >= 18 && frame[12] == 0x81 && frame[13] == 0 &&
                           (tag & 0xfff) == d->config.vlan_id &&
                           !vlan_type(frame + 16);
                if (accepted) {
                    unsigned j;
                    for (j = 12; j < size - 4; ++j)
                        frame[j] = frame[j + 4];
                    size -= 4;
                }
            }
            if (accepted) {
                receive(context, frame, size);
                ++d->stats.rx_ok;
            }
        }
        desc->vlan = 0;
        d->bus.barrier(d->bus.context);
        desc->status = RTL_OWN | RTL_BUFFER_SIZE |
                       (index == RTL_RING_COUNT - 1 ? RTL_EOR : 0);
        d->bus.barrier(d->bus.context);
        d->rx_head = (index + 1) % RTL_RING_COUNT;
        ++count;
    }
    return count;
}

enum rtl_result rtl_dump_tally(struct rtl_device *d, struct rtl_tally *out)
{
    unsigned retry;
    struct rtl_bus *b = &d->bus;
    if (!d->running)
        return RTL_STOPPED;
    if (d->faulted)
        return RTL_IO_ERROR;
    b->barrier(b->context);
    b->write32(b->context, RTL_TALLY_COMMAND + 4, 0);
    b->write32(b->context, RTL_TALLY_COMMAND, d->dma.tally_bus | 0x08);
    for (retry = 0; retry < 100; ++retry) {
        uint32_t command = b->read32(b->context, RTL_TALLY_COMMAND);
        if (command == UINT32_MAX)
            return RTL_IO_ERROR;
        if (!(command & 0x08)) {
            b->barrier(b->context);
            *out = *(const struct rtl_tally *)(uintptr_t)d->dma.tally;
            return RTL_OK;
        }
        b->stall_us(b->context, 10);
    }
    return RTL_TIMEOUT;
}
