/* SPDX-License-Identifier: MIT
 * Host tests for src/ndis/miniport.c against a fake NDIS and an RTL8139C+
 * register/DMA model. Built once per target (NT4 and Win95 resource paths). */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndis_miniport.h"
#include "portio.h"
#include "rtl8139.h"

#define IO 0xc000u
#define IRQ 11u
#define VECTOR 0x3bu /* translated vector differs from the level */
#define SLOT 3u
#define MARKER UINT32_C(0x8139d000)

/* ---- RTL8139C+ model ---- */

struct frame {
    uint8_t data[RTL_BUFFER_SIZE];
    unsigned size;
};

static struct {
    uint8_t regs[256];
    uint32_t tctr;
    unsigned tx_index, rx_index;
    int gone, tx_stuck, reset_stuck, cplus;
    unsigned resets, autoloads;
    uint8_t eeprom[6];
    struct rtl_tally tally;
    struct frame sent[256];
    unsigned sent_count;
} chip;

static const uint8_t eeprom_mac[6] = {0x00, 0xe0, 0x4c, 0x81, 0x39, 0x01};

struct shared {
    void *va;
    uint32_t pa, size;
    int cached;
};

static struct shared shared[64];
static unsigned shared_live, shared_calls, shared_fail_at, shared_misalign_at;
static uint32_t next_pa = 0x00100000;

static void *translate(uint32_t bus, uint32_t size)
{
    unsigned i;
    for (i = 0; i < 64; ++i) {
        struct shared *s = &shared[i];
        if (s->va && bus >= s->pa && bus - s->pa + size <= s->size)
            return (uint8_t *)s->va + (bus - s->pa);
    }
    fprintf(stderr, "unmapped DMA address %08x\n", bus);
    abort();
}

static uint32_t reg32(unsigned r)
{
    return (uint32_t)chip.regs[r] | (uint32_t)chip.regs[r + 1] << 8 |
           (uint32_t)chip.regs[r + 2] << 16 | (uint32_t)chip.regs[r + 3] << 24;
}

static uint16_t reg16(unsigned r)
{
    return (uint16_t)(chip.regs[r] | chip.regs[r + 1] << 8);
}

static void set16(unsigned r, uint16_t v)
{
    chip.regs[r] = (uint8_t)v;
    chip.regs[r + 1] = (uint8_t)(v >> 8);
}

static void set32(unsigned r, uint32_t v)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        chip.regs[r + i] = (uint8_t)(v >> (8 * i));
}

static void raise_status(uint16_t bits)
{
    set16(RTL_INTERRUPT_STATUS, reg16(RTL_INTERRUPT_STATUS) | bits);
}

static void chip_power_on(void)
{
    memset(&chip, 0, sizeof(chip));
    chip.cplus = 1;
    memcpy(chip.eeprom, eeprom_mac, 6);
    set32(RTL_TX_CONFIG, 0x74800000);
    set16(RTL_MEDIA_STATUS, 0); /* link up, 100 Mbps */
}

static void process_tx(void)
{
    struct rtl_descriptor *ring;
    if (chip.tx_stuck)
        return;
    ring = translate(reg32(RTL_TX_RING), RTL_RING_COUNT * 16);
    while (ring[chip.tx_index].status & RTL_OWN) {
        struct rtl_descriptor *d = &ring[chip.tx_index];
        unsigned size = d->status & RTL_LENGTH_MASK;
        struct frame *f = &chip.sent[chip.sent_count++ % 256];
        assert((d->status & (RTL_FS | RTL_LS)) == (RTL_FS | RTL_LS));
        memcpy(f->data, translate(d->address_low, size), size);
        f->size = size;
        d->status &= ~RTL_OWN;
        chip.tx_index = (chip.tx_index + 1) % RTL_RING_COUNT;
        raise_status(RTL_INT_TX_OK);
    }
}

static int inject_rx(const uint8_t *data, unsigned size)
{
    struct rtl_descriptor *ring = translate(reg32(RTL_RX_RING), RTL_RING_COUNT * 16);
    struct rtl_descriptor *d = &ring[chip.rx_index];
    if (!(d->status & RTL_OWN)) {
        raise_status(RTL_INT_RX_UNAVAILABLE);
        return 0;
    }
    memcpy(translate(d->address_low, size + 4), data, size);
    d->status = (d->status & RTL_EOR) | RTL_FS | RTL_LS | (size + 4);
    chip.rx_index = (chip.rx_index + 1) % RTL_RING_COUNT;
    raise_status(RTL_INT_RX_OK);
    return 1;
}

static unsigned offset(uint32_t port, unsigned width)
{
    assert(port >= IO && port + width <= IO + 256);
    return port - IO;
}

uint8_t port_in8(uint32_t port)
{
    unsigned r = offset(port, 1);
    return chip.gone ? 0xff : chip.regs[r];
}

uint16_t port_in16(uint32_t port)
{
    unsigned r = offset(port, 2);
    return chip.gone ? 0xffff : reg16(r);
}

uint32_t port_in32(uint32_t port)
{
    unsigned r = offset(port, 4);
    if (chip.gone)
        return UINT32_MAX;
    if (r == RTL_TIMER)
        return chip.tctr += 50;
    return reg32(r);
}

void port_out8(uint32_t port, uint8_t v)
{
    unsigned r = offset(port, 1);
    if (chip.gone)
        return;
    if (r == RTL_COMMAND && (v & 0x10)) {
        ++chip.resets;
        if (chip.reset_stuck) {
            chip.regs[r] = 0x10;
            return;
        }
        set16(RTL_INTERRUPT_MASK, 0);
        set16(RTL_INTERRUPT_STATUS, 0);
        set16(RTL_CPLUS_COMMAND, 0);
        chip.regs[r] = 0;
        chip.tx_index = chip.rx_index = 0;
        memset(&chip.tally, 0, sizeof(chip.tally));
    } else if (r == RTL_CONFIG_LOCK && (v & 0xc0) == 0x40) {
        ++chip.autoloads;
        memcpy(chip.regs, chip.eeprom, 6);
        chip.regs[r] = 0;
    } else if (r == RTL_TX_POLL) {
        if (v & 0x40)
            process_tx();
    } else {
        chip.regs[r] = v;
    }
}

void port_out16(uint32_t port, uint16_t v)
{
    unsigned r = offset(port, 2);
    if (chip.gone)
        return;
    if (r == RTL_INTERRUPT_STATUS)
        set16(r, reg16(r) & ~v);
    else
        set16(r, v);
}

void port_out32(uint32_t port, uint32_t v)
{
    unsigned r = offset(port, 4);
    if (chip.gone)
        return;
    if (r == RTL_TIMER) {
        chip.tctr = 0;
    } else if (r == RTL_TALLY_COMMAND && (v & 8)) {
        assert(reg32(RTL_TALLY_COMMAND + 4) == 0);
        memcpy(translate(v & ~UINT32_C(0x3f), 64), &chip.tally, 64);
        set32(r, v & ~UINT32_C(8));
    } else if (r == RTL_TX_CONFIG) {
        /* Hardware version bits are read-only. */
        set32(r, (reg32(r) & UINT32_C(0x7cc00000)) | (v & ~UINT32_C(0x7cc00000)));
    } else {
        set32(r, v);
    }
}

void cpu_barrier(void)
{
}

static int interrupt_line(void)
{
    return (reg16(RTL_INTERRUPT_STATUS) & reg16(RTL_INTERRUPT_MASK)) != 0;
}

/* ---- PCI configuration ---- */

static uint8_t pci_space[256];
static int pci_write_ignored; /* bridge drops command-register writes */

static void pci_reset(void)
{
    memset(pci_space, 0, sizeof(pci_space));
    pci_space[0] = 0xec;
    pci_space[1] = 0x10;
    pci_space[2] = 0x39;
    pci_space[3] = 0x81;
    pci_space[8] = 0x20;
    pci_space[0x10] = (uint8_t)(IO | 1);
    pci_space[0x11] = (uint8_t)(IO >> 8);
}

static uint16_t pci_command_value(void)
{
    return (uint16_t)(pci_space[4] | pci_space[5] << 8);
}

/* ---- fake NDIS ---- */

static struct ndis3_characteristics chars;
static uint32_t chars_length;
static void *adapter;
static uint8_t block[512];
static int filter_object;
static unsigned memory_live, port_live, map_live, interrupt_live, shutdown_live;
static unsigned interrupt_fail, port_fail, memory_fail, config_closes;
static unsigned wrapper_fail, register_fail, wrappers_terminated;
static unsigned config_fail, assign_fail, assign_null, map_fail;
static uint32_t interrupt_vector, interrupt_level;
static void (NDIS_CALL *shutdown_fn)(void *);
static void *shutdown_context;
static unsigned resources_available, indications, indication_completes;
static struct frame indicated[64];
static uint32_t last_error[8];
static unsigned errors;
static int transfer_offset = -1;
static uint8_t transfer_data[1600];
static uint32_t transfer_count, transferred;
static ndis_status transfer_status;
static int transfer_null_va;
static void *last_receive;
static uint8_t address_override[8];
static uint32_t address_length;

struct setting {
    const char *name;
    int present;
    uint32_t value;
} settings[] = {
    {"ConnectionType", 0, 0}, {"VlanId", 0, 0}, {"SlotNumber", 0, 0}
};

static void set_config(const char *name, int present, uint32_t value)
{
    unsigned i;
    for (i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (!strcmp(settings[i].name, name)) {
            settings[i].present = present;
            settings[i].value = value;
            return;
        }
    }
    abort();
}

void NdisInitializeWrapper(ndis_handle *wrapper, void *driver, void *registry,
                           void *reserved)
{
    (void)driver, (void)registry, (void)reserved;
    *wrapper = wrapper_fail ? 0 : (void *)1;
}

ndis_status NdisMRegisterMiniport(ndis_handle wrapper,
                                  struct ndis3_characteristics *c, uint32_t length)
{
    assert(wrapper == (void *)1);
    chars = *c;
    chars_length = length;
    return register_fail ? NDIS_STATUS_FAILURE : NDIS_STATUS_SUCCESS;
}

void NdisTerminateWrapper(ndis_handle wrapper, void *reserved)
{
    assert(wrapper == (void *)1 && !reserved);
    ++wrappers_terminated;
}

void __attribute__((cdecl)) NdisWriteErrorLogEntry(ndis_handle handle,
                                                   uint32_t code,
                                                   uint32_t count, ...)
{
    va_list ap;
    unsigned i;
    assert(handle == block && count == 6);
    va_start(ap, count);
    last_error[0] = code;
    for (i = 1; i <= count; ++i)
        last_error[i] = va_arg(ap, uint32_t);
    va_end(ap);
    ++errors;
}

void NdisOpenConfiguration(ndis_status *status, ndis_handle *config,
                           ndis_handle context)
{
    assert(context == (void *)2);
    *config = (void *)3;
    *status = config_fail ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
}

void NdisReadConfiguration(ndis_status *status,
                           struct ndis_configuration_parameter **parameter,
                           ndis_handle config, const struct ndis_string *keyword,
                           uint32_t type)
{
    static struct ndis_configuration_parameter p;
    char name[64];
    unsigned i;
    assert(config == (void *)3 && type == NDIS_PARAMETER_INTEGER);
    assert(keyword->length < sizeof(name) * 2 && !(keyword->length & 1));
    for (i = 0; i < keyword->length / 2u; ++i)
        name[i] = (char)keyword->buffer[i];
    name[i] = 0;
    *status = NDIS_STATUS_FAILURE;
    for (i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (!strcmp(settings[i].name, name)) {
            if (settings[i].present) {
                p.type = NDIS_PARAMETER_INTEGER;
                p.data.integer = settings[i].value;
                *parameter = &p;
                *status = NDIS_STATUS_SUCCESS;
            }
            return;
        }
    }
    fprintf(stderr, "unexpected keyword %s\n", name);
    abort();
}

void NdisCloseConfiguration(ndis_handle config)
{
    assert(config == (void *)3);
    ++config_closes;
}

void NdisReadNetworkAddress(ndis_status *status, void **address,
                            uint32_t *length, ndis_handle config)
{
    assert(config == (void *)3);
    *status = address_length ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
    *address = address_override;
    *length = address_length;
}

uint32_t NdisReadPciSlotInformation(ndis_handle h, uint32_t slot, uint32_t off,
                                    void *buffer, uint32_t length)
{
    assert(h == block && off + length <= 256);
    if (slot == SLOT)
        memcpy(buffer, pci_space + off, length);
    else
        memset(buffer, 0xff, length);
    return length;
}

uint32_t NdisWritePciSlotInformation(ndis_handle h, uint32_t slot, uint32_t off,
                                     void *buffer, uint32_t length)
{
    assert(h == block && slot == SLOT && off == 4 && length == 2);
    if (!pci_write_ignored)
        memcpy(pci_space + off, buffer, length);
    return length;
}

static struct {
    struct ndis_resource_list list;
    struct ndis_resource_descriptor more[3];
} resources;

static void resources_reset(void)
{
    memset(&resources, 0, sizeof(resources));
    resources.list.count = 2;
    resources.list.descriptors[0].type = NDIS_RESOURCE_INTERRUPT;
    resources.list.descriptors[0].u.interrupt.level = IRQ;
    resources.list.descriptors[0].u.interrupt.vector = VECTOR;
    resources.more[0].type = NDIS_RESOURCE_PORT;
    resources.more[0].u.port.start_low = IO;
    resources.more[0].u.port.length = 256;
}

ndis_status NdisMPciAssignResources(ndis_handle h, uint32_t slot,
                                    struct ndis_resource_list **list)
{
    assert(h == block && slot == SLOT);
    *list = assign_fail || assign_null ? 0 : &resources.list;
    return assign_fail ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
}

void NdisMSetAttributes(ndis_handle h, void *context, ndis_boolean bus_master,
                        uint32_t type)
{
    assert(h == block && bus_master && type == NDIS_INTERFACE_PCI);
    adapter = context;
}

ndis_status NdisMRegisterIoPortRange(void **mapped, ndis_handle h, uint32_t port,
                                     uint32_t length)
{
    assert(h == block && port == IO && length == 256);
    if (port_fail)
        return NDIS_STATUS_RESOURCES;
    ++port_live;
    *mapped = (void *)(uintptr_t)port;
    return NDIS_STATUS_SUCCESS;
}

void NdisMDeregisterIoPortRange(ndis_handle h, uint32_t port, uint32_t length,
                                void *mapped)
{
    assert(h == block && port == IO && length == 256 &&
           mapped == (void *)(uintptr_t)IO);
    --port_live;
}

ndis_status NdisMAllocateMapRegisters(ndis_handle h, uint32_t channel,
                                      ndis_boolean dma32, uint32_t count,
                                      uint32_t maximum)
{
    assert(h == block && !channel && dma32 && count == 1 && maximum == 1536);
    if (map_fail)
        return NDIS_STATUS_RESOURCES;
    ++map_live;
    return NDIS_STATUS_SUCCESS;
}

void NdisMFreeMapRegisters(ndis_handle h)
{
    assert(h == block && map_live == 1 && !shared_live);
    --map_live;
}

void NdisMAllocateSharedMemory(ndis_handle h, uint32_t length, ndis_boolean cached,
                               void **va, uint64_t *pa)
{
    unsigned i;
    uint32_t size = (length + 4095) & ~4095u;
    assert(h == block && map_live);
    *va = 0;
    if (++shared_calls == shared_fail_at)
        return;
    for (i = 0; shared[i].va; ++i)
        assert(i < 63);
    shared[i].va = aligned_alloc(4096, size);
    assert(shared[i].va);
    memset(shared[i].va, 0xa5, size);
    shared[i].pa = next_pa + (shared_calls == shared_misalign_at ? 4 : 0);
    shared[i].size = length;
    shared[i].cached = cached;
    next_pa += size + 4096;
    *va = shared[i].va;
    *pa = shared[i].pa;
    ++shared_live;
}

void NdisMFreeSharedMemory(ndis_handle h, uint32_t length, ndis_boolean cached,
                           void *va, uint64_t pa)
{
    unsigned i;
    assert(h == block);
    for (i = 0; i < 64; ++i) {
        if (shared[i].va == va) {
            assert(shared[i].pa == pa && shared[i].size == length &&
                   shared[i].cached == cached);
            free(va);
            shared[i].va = 0;
            --shared_live;
            return;
        }
    }
    abort();
}

ndis_status NdisMRegisterInterrupt(struct ndis_miniport_interrupt *i, ndis_handle h,
                                   uint32_t vector, uint32_t level,
                                   ndis_boolean request_isr, ndis_boolean shared_irq,
                                   uint32_t mode)
{
    assert(i && h == block && request_isr && shared_irq &&
           mode == NDIS_INTERRUPT_LEVEL_SENSITIVE);
    if (interrupt_fail)
        return NDIS_STATUS_FAILURE;
    interrupt_vector = vector;
    interrupt_level = level;
    ++interrupt_live;
    return NDIS_STATUS_SUCCESS;
}

void NdisMDeregisterInterrupt(struct ndis_miniport_interrupt *i)
{
    assert(i && interrupt_live == 1);
    assert(!reg16(RTL_INTERRUPT_MASK) || chip.gone);
    --interrupt_live;
}

ndis_boolean NdisMSynchronizeWithInterrupt(struct ndis_miniport_interrupt *i,
                                           ndis_synchronize_fn fn, void *context)
{
    assert(i && interrupt_live && context == adapter);
    return fn(context);
}

void NdisMRegisterAdapterShutdownHandler(ndis_handle h, void *context,
                                         ndis_shutdown_handler fn)
{
    assert(h == block);
    shutdown_fn = fn;
    shutdown_context = context;
    ++shutdown_live;
}

void NdisMDeregisterAdapterShutdownHandler(ndis_handle h)
{
    assert(h == block && shutdown_live == 1);
    --shutdown_live;
}

ndis_status NdisAllocateMemory(void **p, uint32_t length, uint32_t flags,
                               uint64_t highest)
{
    assert(!flags && highest == UINT64_MAX);
    if (memory_fail) {
        *p = 0;
        return NDIS_STATUS_RESOURCES;
    }
    *p = malloc(length);
    memset(*p, 0x5a, length);
    ++memory_live;
    return NDIS_STATUS_SUCCESS;
}

void NdisFreeMemory(void *p, uint32_t length, uint32_t flags)
{
    assert(p && length && !flags);
    free(p);
    --memory_live;
}

struct test_buffer {
    struct ndis_buffer link;
    void *va;
    uint32_t length;
};

void NdisQueryBuffer(struct ndis_buffer *b, void **va, uint32_t *length)
{
    struct test_buffer *t = (struct test_buffer *)b;
    *va = t->va;
    *length = t->length;
}

void NdisMSendResourcesAvailable(ndis_handle h)
{
    assert(h == block);
    ++resources_available;
}

void EthFilterDprIndicateReceive(void *filter, ndis_handle context,
                                 const void *address, const void *header,
                                 uint32_t header_size, const void *lookahead,
                                 uint32_t lookahead_size, uint32_t packet_size)
{
    struct frame *f = &indicated[indications++ % 64];
    assert(filter == &filter_object && address == header && header_size == 14);
    assert((const uint8_t *)lookahead == (const uint8_t *)header + 14);
    assert(lookahead_size == packet_size);
    memcpy(f->data, header, 14 + packet_size);
    f->size = 14 + packet_size;
    last_receive = context;
    if (transfer_offset >= 0) {
        struct test_buffer b2 = {{0}, transfer_data + 5, sizeof(transfer_data) - 5};
        struct test_buffer b1 = {{&b2.link}, transfer_null_va ? 0 : transfer_data, 5};
        struct ndis_packet packet = {0, 0, &b1.link};
        transferred = 0;
        transfer_status = chars.transfer(&packet, &transferred, adapter, context,
                                         (uint32_t)transfer_offset, transfer_count);
    }
}

void EthFilterDprIndicateReceiveComplete(void *filter)
{
    assert(filter == &filter_object);
    ++indication_completes;
}

/* ---- helpers ---- */

static void reset_world(void)
{
    void *filter = &filter_object;
    unsigned i;
    chip_power_on();
    pci_reset();
    resources_reset();
    memset(block, 0, sizeof(block));
    memcpy(block + NDIS_MINIPORT_BLOCK_ETHDB, &filter, sizeof(filter));
    adapter = 0;
    errors = 0;
    memset(last_error, 0, sizeof(last_error));
    shared_calls = shared_fail_at = shared_misalign_at = 0;
    interrupt_fail = port_fail = memory_fail = 0;
    config_fail = assign_fail = assign_null = map_fail = 0;
    transfer_null_va = 0;
    pci_write_ignored = 0;
    address_length = 0;
    indications = indication_completes = resources_available = 0;
    transfer_offset = -1;
    for (i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i)
        settings[i].present = 0;
    set_config("SlotNumber", 1, SLOT);
}

static ndis_status initialize(void)
{
    ndis_status open_error = 1;
    uint32_t selected = 99;
    uint32_t media[] = {6, 0, 2}; /* ArcnetRaw, 802_3, Fddi */
    ndis_status status = chars.initialize(&open_error, &selected, media, 3,
                                          block, (void *)2);
    assert(open_error == NDIS_STATUS_SUCCESS);
    if (status == NDIS_STATUS_SUCCESS)
        assert(selected == 1);
    return status;
}

static void assert_released(void)
{
    assert(!memory_live && !port_live && !map_live && !interrupt_live &&
           !shutdown_live && !shared_live);
}

static void halt(void)
{
    chars.halt(adapter);
    assert(!reg16(RTL_INTERRUPT_MASK));
    assert_released();
}

static uint32_t query32(uint32_t oid)
{
    uint32_t value = 0, written, needed;
    assert(chars.query(adapter, oid, &value, 4, &written, &needed) == NDIS_STATUS_SUCCESS);
    assert(written == 4 && needed == 0);
    return value;
}

static ndis_status set_oid(uint32_t oid, const void *data, uint32_t size)
{
    uint8_t copy[256];
    uint32_t read = 77, needed = 77;
    ndis_status status;
    assert(size <= sizeof(copy));
    memcpy(copy, data, size);
    status = chars.set(adapter, oid, copy, size, &read, &needed);
    if (status == NDIS_STATUS_SUCCESS)
        assert(read == size && needed == 0);
    else
        assert(read == 0);
    return status;
}

static ndis_status send_frame(const uint8_t *data, unsigned size)
{
    static uint8_t copy[4096];
    struct test_buffer b[3];
    struct ndis_packet packet = {0, 0, &b[0].link};
    unsigned first = size < 6 ? size : 6;
    unsigned second = size - first < 20 ? size - first : 20;
    assert(size <= sizeof(copy));
    memcpy(copy, data, size);
    b[0] = (struct test_buffer){{&b[1].link}, copy, first};
    b[1] = (struct test_buffer){{&b[2].link}, copy + first, second};
    b[2] = (struct test_buffer){{0}, copy + first + second, size - first - second};
    return chars.send(adapter, &packet, 0);
}

/* Fires the interrupt path as NDIS would: ISR at DIRQL, then the DPC. */
static int service_interrupt(void)
{
    ndis_boolean recognized = 9, queue = 9;
    if (!interrupt_line())
        return 0;
    chars.isr(&recognized, &queue, adapter);
    assert(recognized && queue);
    assert(!reg16(RTL_INTERRUPT_MASK));
    chars.handle_interrupt(adapter);
    return 1;
}

static void make_frame(uint8_t *f, unsigned size, uint8_t seed)
{
    unsigned i;
    memcpy(f, "\xff\xff\xff\xff\xff\xff", 6);
    memcpy(f + 6, "\x02\x00\x00\x00\x00\x09", 6);
    f[12] = 0x08;
    f[13] = 0x00;
    for (i = 14; i < size; ++i)
        f[i] = (uint8_t)(seed + i);
}

#define ENABLED 0x807fu

/* ---- tests ---- */

static void test_entry(void)
{
    wrapper_fail = 1;
    assert(DriverEntry((void *)10, (void *)11) == NDIS_STATUS_FAILURE);
    assert(!wrappers_terminated);
    wrapper_fail = 0;
    register_fail = 1;
    assert(DriverEntry((void *)10, (void *)11) == NDIS_STATUS_FAILURE);
    assert(wrappers_terminated == 1);
    register_fail = 0;
    assert(DriverEntry((void *)10, (void *)11) == NDIS_STATUS_SUCCESS);
    assert(wrappers_terminated == 1);
    assert(chars_length == sizeof(struct ndis3_characteristics) && chars.major == 3 && chars.minor == 0);
    assert(chars.initialize && chars.send && chars.transfer && chars.isr);
}

static void test_initialize(void)
{
    uint8_t mac[6];
    uint32_t value, written, needed;
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    assert(!errors && memory_live == 1 && port_live == 1 && map_live == 1);
    assert(interrupt_live == 1 && interrupt_vector == VECTOR && interrupt_level == IRQ);
    assert(shared_live == 17);
    assert(chip.autoloads == 1);
    assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);
    assert((pci_command_value() & 5) == 5);
    assert(!memcmp(chip.regs, eeprom_mac, 6));
    assert(reg16(RTL_CPLUS_COMMAND) == 3 && chip.regs[RTL_COMMAND] == 0x0c);
    assert((reg32(RTL_RX_CONFIG) & 0x3f) == 0);
    assert(reg16(RTL_BMCR) == 0x1200);
    assert(shutdown_live == 1 && shutdown_context == adapter);
    assert(config_closes >= 1);

    assert(chars.query(adapter, OID_802_3_PERMANENT_ADDRESS, mac, 6, &written,
                       &needed) == NDIS_STATUS_SUCCESS && written == 6);
    assert(!memcmp(mac, eeprom_mac, 6));
    assert(chars.query(adapter, OID_802_3_CURRENT_ADDRESS, mac, 5, &written,
                       &needed) == NDIS_STATUS_INVALID_LENGTH);
    assert(written == 0 && needed == 6);
    assert(chars.query(adapter, 0x12345678, &value, 4, &written, &needed) ==
           NDIS_STATUS_INVALID_OID);
    halt();
    assert(chip.resets >= 2);
}

static void test_queries(void)
{
    uint32_t list[64], written, needed;
    uint16_t version;
    char text[64];
    uint8_t vendor[4];
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    assert(chars.query(adapter, OID_GEN_SUPPORTED_LIST, list, 8, &written,
                       &needed) == NDIS_STATUS_INVALID_LENGTH);
    assert(needed == 33 * 4);
    assert(chars.query(adapter, OID_GEN_SUPPORTED_LIST, list, sizeof(list),
                       &written, &needed) == NDIS_STATUS_SUCCESS);
    assert(written == 33 * 4 && list[0] == OID_GEN_SUPPORTED_LIST);
    assert(query32(OID_GEN_HARDWARE_STATUS) == 0);
    assert(query32(OID_GEN_MEDIA_SUPPORTED) == 0);
    assert(query32(OID_GEN_MAXIMUM_FRAME_SIZE) == 1500);
    assert(query32(OID_GEN_MAXIMUM_TOTAL_SIZE) == 1514);
    assert(query32(OID_GEN_TRANSMIT_BUFFER_SPACE) == RTL_RING_COUNT * RTL_BUFFER_SIZE);
    assert(query32(OID_GEN_RECEIVE_BLOCK_SIZE) == RTL_BUFFER_SIZE);
    assert(query32(OID_GEN_LINK_SPEED) == 1000000);
    assert(query32(OID_GEN_MEDIA_CONNECT_STATUS) == 0);
    chip.regs[RTL_MEDIA_STATUS] = 0x04 | 0x08; /* no link */
    assert(query32(OID_GEN_MEDIA_CONNECT_STATUS) == 1);
    chip.regs[RTL_MEDIA_STATUS] = 0x08; /* 10 Mbps */
    assert(query32(OID_GEN_MEDIA_CONNECT_STATUS) == 0);
    assert(query32(OID_GEN_LINK_SPEED) == 100000);
    assert(query32(OID_GEN_MAC_OPTIONS) == 0x0f);
    assert(query32(OID_GEN_CURRENT_LOOKAHEAD) == 1500);
    assert(query32(OID_802_3_MAXIMUM_LIST_SIZE) == 32);
    assert(query32(OID_GEN_VENDOR_DRIVER_VERSION) == 0x00010000);
    assert(chars.query(adapter, OID_GEN_DRIVER_VERSION, &version, 2, &written,
                       &needed) == NDIS_STATUS_SUCCESS && written == 2 && version == 0x0300);
    assert(chars.query(adapter, OID_GEN_VENDOR_ID, vendor, 4, &written,
                       &needed) == NDIS_STATUS_SUCCESS);
    assert(!memcmp(vendor, "\x00\xe0\x4c\x00", 4));
    assert(chars.query(adapter, OID_GEN_VENDOR_DESCRIPTION, text, sizeof(text),
                       &written, &needed) == NDIS_STATUS_SUCCESS);
    assert(written == strlen(text) + 1 && strstr(text, "RTL8139C+"));

    chip.tally.tx_one_collision = 4;
    chip.tally.tx_multi_collision = 2;
    chip.tally.rx_alignment = 3;
    chip.tally.rx_missed = 9;
    assert(query32(OID_802_3_XMIT_ONE_COLLISION) == 4);
    assert(query32(OID_802_3_XMIT_MORE_COLLISIONS) == 2);
    assert(query32(OID_802_3_RCV_ERROR_ALIGNMENT) == 3);
    assert(query32(OID_GEN_RCV_NO_BUFFER) == 9);
    /* Counters survive a chip reset by accumulation. */
    {
        ndis_boolean addressing = 9;
        assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS && !addressing);
    }
    assert(chip.tally.tx_one_collision == 0);
    chip.tally.tx_one_collision = 1;
    assert(query32(OID_802_3_XMIT_ONE_COLLISION) == 5);
    halt();
}

static void test_filters(void)
{
    uint32_t filter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST;
    uint8_t list[33 * 6];
    uint32_t value;
    unsigned i;
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    assert(set_oid(OID_GEN_CURRENT_PACKET_FILTER, &filter, 4) == NDIS_STATUS_SUCCESS);
    assert((reg32(RTL_RX_CONFIG) & 0x3f) == 0x0a);
    assert(query32(OID_GEN_CURRENT_PACKET_FILTER) == filter);
    value = 0x10; /* SMT, unsupported */
    assert(set_oid(OID_GEN_CURRENT_PACKET_FILTER, &value, 4) == NDIS_STATUS_NOT_SUPPORTED);
    assert(query32(OID_GEN_CURRENT_PACKET_FILTER) == filter);
    assert(set_oid(OID_GEN_CURRENT_PACKET_FILTER, &value, 3) ==
           NDIS_STATUS_INVALID_LENGTH);
    value = NDIS_PACKET_TYPE_ALL_LOCAL | NDIS_PACKET_TYPE_DIRECTED |
            NDIS_PACKET_TYPE_MULTICAST;
    assert(set_oid(OID_GEN_CURRENT_PACKET_FILTER, &value, 4) == NDIS_STATUS_SUCCESS);
    assert((reg32(RTL_RX_CONFIG) & 0x3f) == 0x06);
    for (i = 0; i < 33; ++i)
        memcpy(list + i * 6, "\x01\x00\x5e\x00\x00\x00", 6), list[i * 6 + 5] = (uint8_t)i;
    assert(set_oid(OID_802_3_MULTICAST_LIST, list, 33 * 6) ==
           NDIS_STATUS_MULTICAST_FULL);
    assert(set_oid(OID_802_3_MULTICAST_LIST, list, 7) == NDIS_STATUS_INVALID_LENGTH);
    list[6] = 0x02;
    assert(set_oid(OID_802_3_MULTICAST_LIST, list, 12) == NDIS_STATUS_INVALID_DATA);
    list[6] = 0x01;
    assert(set_oid(OID_802_3_MULTICAST_LIST, list, 12) == NDIS_STATUS_SUCCESS);
    {
        /* CRC-32 hash bits of 01:00:5e:00:00:00 and :01 are 5 and 31. */
        uint8_t back[12];
        uint32_t written, needed;
        assert(reg32(RTL_MULTICAST) == (UINT32_C(1) << 5 | UINT32_C(1) << 31));
        assert(reg32(RTL_MULTICAST + 4) == 0);
        assert(chars.query(adapter, OID_802_3_MULTICAST_LIST, back, 12, &written,
                           &needed) == NDIS_STATUS_SUCCESS && written == 12);
        assert(!memcmp(back, list, 12));
    }
    value = 1501;
    assert(set_oid(OID_GEN_CURRENT_LOOKAHEAD, &value, 4) == NDIS_STATUS_INVALID_DATA);
    value = 256;
    assert(set_oid(OID_GEN_CURRENT_LOOKAHEAD, &value, 4) == NDIS_STATUS_SUCCESS);
    assert(query32(OID_GEN_CURRENT_LOOKAHEAD) == 256);
    assert(set_oid(OID_GEN_PROTOCOL_OPTIONS, &value, 4) == NDIS_STATUS_SUCCESS);
    assert(set_oid(OID_GEN_LINK_SPEED, &value, 4) == NDIS_STATUS_INVALID_OID);
    /* Filters are restored after reset. */
    {
        ndis_boolean addressing;
        set32(RTL_RX_CONFIG, 0);
        set32(RTL_MULTICAST, 0);
        set32(RTL_MULTICAST + 4, 0);
        assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
        assert((reg32(RTL_RX_CONFIG) & 0x3f) == 0x06);
        assert(reg32(RTL_MULTICAST) | reg32(RTL_MULTICAST + 4));
        assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);
    }
    halt();
}

static void test_send(void)
{
    uint8_t f[1600];
    unsigned i, start;
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    make_frame(f, 1514, 1);
    assert(send_frame(f, 1514) == NDIS_STATUS_SUCCESS);
    assert(chip.sent_count == 1 && chip.sent[0].size == 1514);
    assert(!memcmp(chip.sent[0].data, f, 1514));
    make_frame(f, 42, 2);
    assert(send_frame(f, 42) == NDIS_STATUS_SUCCESS);
    assert(chip.sent[1].size == 60 && !memcmp(chip.sent[1].data, f, 42));
    for (i = 42; i < 60; ++i)
        assert(chip.sent[1].data[i] == 0);
    assert(send_frame(f, 1515) == NDIS_STATUS_FAILURE);
    assert(send_frame(f, 10) == NDIS_STATUS_FAILURE);
    {
        /* An empty unmapped buffer is harmless; a non-empty one is rejected. */
        struct test_buffer nb[2];
        struct ndis_packet packet = {0, 0, &nb[0].link};
        nb[0] = (struct test_buffer){{&nb[1].link}, 0, 0};
        nb[1] = (struct test_buffer){{0}, f, 60};
        assert(chars.send(adapter, &packet, 0) == NDIS_STATUS_SUCCESS);
        nb[0].length = 14;
        assert(chars.send(adapter, &packet, 0) == NDIS_STATUS_FAILURE);
    }
    assert(query32(OID_GEN_XMIT_ERROR) == 3);
    assert(service_interrupt());
    assert(chip.sent_count == 3 && !memcmp(chip.sent[2].data, f, 60));
    assert(query32(OID_GEN_XMIT_OK) == 3);

    /* Ring exhaustion is flow-controlled, then released by the DPC. */
    chip.tx_stuck = 1;
    start = chip.sent_count;
    for (i = 0; i < RTL_RING_COUNT; ++i)
        assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);
    assert(send_frame(f, 60) == NDIS_STATUS_RESOURCES);
    assert(resources_available == 0);
    chip.tx_stuck = 0;
    process_tx();
    assert(chip.sent_count == start + RTL_RING_COUNT);
    assert(service_interrupt());
    assert(resources_available == 1);
    assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);
    assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);
    halt();

    /* A free descriptor still owned by the chip means a lost ring: reset. */
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    ((struct rtl_descriptor *)translate(reg32(RTL_TX_RING), 16))->status |= RTL_OWN;
    assert(send_frame(f, 60) == NDIS_STATUS_FAILURE);
    assert(chars.check_for_hang(adapter));
    {
        ndis_boolean addressing;
        assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
    }
    start = chip.sent_count;
    assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);
    assert(chip.sent_count == start + 1);
    halt();
}

static void test_receive(void)
{
    uint8_t f[1600];
    unsigned i;
    ndis_boolean recognized, queue;
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);

    /* Shared IRQ: not ours when nothing is pending. */
    chars.isr(&recognized, &queue, adapter);
    assert(!recognized && !queue);

    make_frame(f, 1514, 3);
    assert(inject_rx(f, 1514));
    make_frame(f, 60, 4);
    assert(inject_rx(f, 60));
    assert(service_interrupt());
    assert(indications == 2 && indication_completes == 1);
    assert(indicated[1].size == 60 && !memcmp(indicated[1].data, f, 60));
    make_frame(f, 1514, 3);
    assert(indicated[0].size == 1514 && !memcmp(indicated[0].data, f, 1514));
    assert(query32(OID_GEN_RCV_OK) == 2);
    assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);

    /* TransferData during the indication copies across buffer boundaries. */
    transfer_offset = 10;
    transfer_count = 1000;
    memset(transfer_data, 0, sizeof(transfer_data));
    assert(inject_rx(f, 1514));
    assert(service_interrupt());
    assert(transfer_status == NDIS_STATUS_SUCCESS && transferred == 1000);
    assert(!memcmp(transfer_data, f + 14 + 10, 1000));
    transfer_offset = 1490;
    transfer_count = 100;
    assert(inject_rx(f, 1514));
    assert(service_interrupt());
    assert(transfer_status == NDIS_STATUS_SUCCESS && transferred == 10);
    transfer_offset = 1501;
    assert(inject_rx(f, 1514));
    assert(service_interrupt());
    assert(transfer_status == NDIS_STATUS_FAILURE && transferred == 0);
    /* A non-empty buffer without a mapping is refused. */
    transfer_offset = 0;
    transfer_count = 10;
    transfer_null_va = 1;
    assert(inject_rx(f, 1514));
    assert(service_interrupt());
    assert(transfer_status == NDIS_STATUS_FAILURE && transferred == 0);
    transfer_null_va = 0;
    transfer_offset = -1;
    /* TransferData is only valid during the indication. */
    {
        struct test_buffer b = {{0}, transfer_data, 10};
        struct ndis_packet packet = {0, 0, &b.link};
        uint32_t n = 77;
        assert(chars.transfer(&packet, &n, adapter, last_receive, 0, 10) ==
               NDIS_STATUS_FAILURE && n == 0);
    }
    /* Before NDIS has created the filter database, frames are dropped. */
    {
        void *filter = 0;
        uint32_t dropped = query32(OID_GEN_RCV_NO_BUFFER);
        unsigned before = indications, completes = indication_completes;
        memcpy(block + NDIS_MINIPORT_BLOCK_ETHDB, &filter, sizeof(filter));
        assert(inject_rx(f, 60));
        assert(service_interrupt());
        assert(indications == before && indication_completes == completes);
        assert(query32(OID_GEN_RCV_NO_BUFFER) == dropped + 1);
        filter = &filter_object;
        memcpy(block + NDIS_MINIPORT_BLOCK_ETHDB, &filter, sizeof(filter));
    }

    /* A burst larger than the ring wraps several times. */
    for (i = 0; i < 300; ++i) {
        make_frame(f, 100, (uint8_t)i);
        assert(inject_rx(f, 100));
        if (i % 50 == 49)
            assert(service_interrupt());
    }
    assert(!service_interrupt());
    make_frame(f, 100, (uint8_t)299);
    assert(!memcmp(indicated[(indications - 1) % 64].data, f, 100));
    /* Receive errors are counted, not indicated. */
    {
        struct rtl_descriptor *ring = translate(reg32(RTL_RX_RING), 1024);
        unsigned before = indications;
        ring[chip.rx_index].status = (ring[chip.rx_index].status & RTL_EOR) |
                                     RTL_FS | RTL_LS | RTL_RX_ERROR | 64;
        chip.rx_index = (chip.rx_index + 1) % RTL_RING_COUNT;
        raise_status(RTL_INT_RX_ERROR);
        assert(service_interrupt());
        assert(indications == before && query32(OID_GEN_RCV_ERROR) == 1);
    }
    halt();
}

static void test_link_and_errors(void)
{
    ndis_boolean recognized, queue, addressing;
    uint8_t f[64];
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    assert(!chars.check_for_hang(adapter));
    chip.regs[RTL_MEDIA_STATUS] = 0x04;
    raise_status(RTL_INT_LINK_CHANGE);
    assert(service_interrupt());
    assert(query32(OID_GEN_LINK_SPEED) == 1000000); /* last known default */
    chip.regs[RTL_MEDIA_STATUS] = 0;

    /* A transmit that never completes is reported as a hang. */
    make_frame(f, 60, 0);
    chip.tx_stuck = 1;
    assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);
    assert(!chars.check_for_hang(adapter));
    assert(!chars.check_for_hang(adapter));
    assert(chars.check_for_hang(adapter));
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
    assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);
    chip.regs[RTL_MEDIA_STATUS] = 0x04; /* no link: never a hang */
    assert(!chars.check_for_hang(adapter));
    assert(!chars.check_for_hang(adapter));
    assert(!chars.check_for_hang(adapter));
    assert(!chars.check_for_hang(adapter));
    chip.regs[RTL_MEDIA_STATUS] = 0;
    chip.tx_stuck = 0;
    process_tx();
    assert(!chars.check_for_hang(adapter));
    assert(service_interrupt());

    /* PCI system error forces a reset with interrupts left masked. */
    raise_status(RTL_INT_SYSTEM_ERROR);
    assert(service_interrupt());
    assert(!reg16(RTL_INTERRUPT_MASK));
    chars.enable_interrupt(adapter); /* NDIS re-enable must not unmask */
    assert(!reg16(RTL_INTERRUPT_MASK));
    assert(query32(OID_GEN_HARDWARE_STATUS) == 4);
    assert(send_frame(f, 60) == NDIS_STATUS_FAILURE);
    assert(chars.check_for_hang(adapter));
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
    assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);
    chars.disable_interrupt(adapter);
    assert(!reg16(RTL_INTERRUPT_MASK));
    /* Shared IRQ while masked: never claimed. */
    raise_status(RTL_INT_RX_OK);
    chars.isr(&recognized, &queue, adapter);
    assert(!recognized && !queue);
    chars.enable_interrupt(adapter);
    assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);
    assert(service_interrupt());

    /* A reset whose link re-read fails is not reported as recovered. */
    chip.regs[RTL_MEDIA_STATUS] = 0xff;
    assert(chars.check_for_hang(adapter));
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_HARD_ERRORS);
    assert(!reg16(RTL_INTERRUPT_MASK));
    assert(chars.check_for_hang(adapter));
    chip.regs[RTL_MEDIA_STATUS] = 0;
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
    assert(reg16(RTL_INTERRUPT_MASK) == ENABLED);

    /* Surprise removal: all-ones reads are never claimed. */
    chip.gone = 1;
    chars.isr(&recognized, &queue, adapter);
    assert(!recognized && !queue);
    chars.handle_interrupt(adapter);
    assert(chars.check_for_hang(adapter));
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_HARD_ERRORS);
    assert((pci_command_value() & 4) == 0);
    chip.gone = 0;
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
    assert((pci_command_value() & 5) == 5);
    assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);

    /* A reset that cannot restore bus mastering is not a recovery. */
    {
        uint8_t command[2] = {pci_space[4], pci_space[5]};
        pci_space[4] = pci_space[5] = 0xff;
        assert(chars.reset(&addressing, adapter) == NDIS_STATUS_HARD_ERRORS);
        pci_space[4] = command[0];
        pci_space[5] = command[1];
        assert(chars.reset(&addressing, adapter) == NDIS_STATUS_SUCCESS);
    }

    shutdown_fn(shutdown_context);
    assert(!reg16(RTL_INTERRUPT_MASK) && chip.resets);
    halt();
}

static void test_configuration(void)
{
    uint8_t f[64], mac[6];
    uint32_t written, needed;
    reset_world();
    set_config("ConnectionType", 1, 4);
    set_config("VlanId", 1, 0x123);
    memcpy(address_override, "\x02\x11\x22\x33\x44\x55", 6);
    address_length = 6;
    assert(initialize() == NDIS_STATUS_SUCCESS);
    assert(reg16(RTL_BMCR) == 0x2100);
    assert(!memcmp(chip.regs, address_override, 6));
    assert(chars.query(adapter, OID_802_3_CURRENT_ADDRESS, mac, 6, &written,
                       &needed) == NDIS_STATUS_SUCCESS && !memcmp(mac, address_override, 6));
    assert(chars.query(adapter, OID_802_3_PERMANENT_ADDRESS, mac, 6, &written,
                       &needed) == NDIS_STATUS_SUCCESS && !memcmp(mac, eeprom_mac, 6));
    make_frame(f, 60, 0);
    assert(send_frame(f, 60) == NDIS_STATUS_SUCCESS);
    assert(chip.sent[0].size == 64 && chip.sent[0].data[12] == 0x81 &&
           chip.sent[0].data[14] == 0x01 && chip.sent[0].data[15] == 0x23);
    halt();
}

static void expect_failure(ndis_status status, uint32_t code, unsigned step)
{
    assert(initialize() == status);
    assert(errors == 1 && last_error[0] == code);
    assert(last_error[1] == (MARKER | step));
    assert(!reg16(RTL_INTERRUPT_MASK));
    assert_released();
}

/* Frees DMA memory the driver deliberately leaked under a live card. */
static void reclaim_leaked(void)
{
    unsigned i;
    for (i = 0; i < 64; ++i) {
        free(shared[i].va);
        shared[i].va = 0;
    }
    shared_live = 0;
    map_live = 0;
    chip.reset_stuck = 0;
}

static void test_failures(void)
{
    unsigned resets;
    ndis_boolean addressing;
    {
        ndis_status open_error = 1;
        uint32_t selected = 99, media[] = {6, 2};
        reset_world();
        assert(chars.initialize(&open_error, &selected, media, 2, block,
                                (void *)2) == NDIS_STATUS_UNSUPPORTED_MEDIA);
        assert(open_error == NDIS_STATUS_SUCCESS && selected == 99 && !errors);
        assert_released();
    }

    reset_world();
    memory_fail = 1;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_OUT_OF_RESOURCES, 1);

    reset_world();
    config_fail = 1;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_OUT_OF_RESOURCES, 2);

    reset_world();
    set_config("ConnectionType", 1, 5);
    expect_failure(NDIS_STATUS_INVALID_DATA, NDIS_ERROR_UNSUPPORTED_CONFIGURATION, 3);

    reset_world();
    set_config("VlanId", 1, 4095);
    expect_failure(NDIS_STATUS_INVALID_DATA, NDIS_ERROR_UNSUPPORTED_CONFIGURATION, 4);

    reset_world();
    memcpy(address_override, "\x01\x11\x22\x33\x44\x55", 6);
    address_length = 6;
    expect_failure(NDIS_STATUS_INVALID_DATA, NDIS_ERROR_NETWORK_ADDRESS, 5);

    reset_world();
    address_length = 5;
    expect_failure(NDIS_STATUS_INVALID_DATA, NDIS_ERROR_NETWORK_ADDRESS, 5);

    /* Diagnostics: slot and the PCI ID read there. */
    reset_world();
    set_config("SlotNumber", 1, 4);
    expect_failure(NDIS_STATUS_ADAPTER_NOT_FOUND, NDIS_ERROR_ADAPTER_NOT_FOUND, 6);
    assert(last_error[3] == 4 && last_error[4] == UINT32_MAX);

    reset_world();
    pci_space[2] = 0x29; /* 10EC:8129 */
    expect_failure(NDIS_STATUS_ADAPTER_NOT_FOUND, NDIS_ERROR_ADAPTER_NOT_FOUND, 6);
    assert(last_error[3] == SLOT && last_error[4] == 0x812910ecu);

    reset_world();
    set_config("SlotNumber", 1, 256);
    expect_failure(NDIS_STATUS_INVALID_DATA, NDIS_ERROR_UNSUPPORTED_CONFIGURATION, 6);

    /* A C+ whose PCI revision reads 10h is accepted (TxConfig decides). */
    reset_world();
    pci_space[8] = 0x10;
    assert(initialize() == NDIS_STATUS_SUCCESS && !errors);
    halt();

    reset_world();
    assign_fail = 1;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_RESOURCE_CONFLICT, 7);

    reset_world();
    assign_null = 1;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_RESOURCE_CONFLICT, 7);

    reset_world();
    resources.list.count = 1;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_RESOURCE_CONFLICT, 8);

    reset_world();
    resources.more[0].u.port.length = 128;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_RESOURCE_CONFLICT, 8);

    reset_world();
    resources.list.descriptors[0].type = 3; /* memory, no interrupt */
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_RESOURCE_CONFLICT, 9);

    reset_world();
    port_fail = 1;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_RESOURCE_CONFLICT, 10);

    reset_world();
    pci_space[4] = pci_space[5] = 0xff; /* command register unreadable */
    expect_failure(NDIS_STATUS_ADAPTER_NOT_FOUND, NDIS_ERROR_ADAPTER_NOT_FOUND, 11);

    reset_world();
    chip.gone = 1;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_HARDWARE_FAILURE, 12);
    assert(last_error[3] == IO && last_error[4] == UINT32_MAX);

    reset_world();
    chip.eeprom[0] = 1; /* multicast EEPROM address, no override */
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_NETWORK_ADDRESS, 12);

    reset_world();
    map_fail = 1;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_OUT_OF_RESOURCES, 13);

    reset_world();
    shared_misalign_at = 1;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_OUT_OF_RESOURCES, 14);

    reset_world();
    shared_misalign_at = 5;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_OUT_OF_RESOURCES, 14);

    reset_world();
    chip.regs[RTL_MEDIA_STATUS] = 0xff; /* link unreadable after start */
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_HARDWARE_FAILURE, 15);

    reset_world();
    chip.cplus = 0;
    set32(RTL_TX_CONFIG, 0x60000000); /* RTL8139C, not C+ */
    pci_space[8] = 0x10;
    expect_failure(NDIS_STATUS_ADAPTER_NOT_FOUND, NDIS_ERROR_ADAPTER_NOT_FOUND, 12);
    assert(last_error[3] == IO && last_error[4] == 0x60000000u);
    assert(last_error[5] == SLOT);
    assert(chip.autoloads == 0);

    reset_world();
    shared_fail_at = 1;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_OUT_OF_RESOURCES, 14);

    reset_world();
    shared_fail_at = 9;
    expect_failure(NDIS_STATUS_RESOURCES, NDIS_ERROR_OUT_OF_RESOURCES, 14);

    reset_world();
    interrupt_fail = 1;
    resets = chip.resets;
    expect_failure(NDIS_STATUS_FAILURE, NDIS_ERROR_INTERRUPT_CONNECT, 16);
    assert(chip.resets > resets);

    /* A chip that never leaves reset loses bus mastering before release. */
    reset_world();
    interrupt_fail = 1;
    chip.reset_stuck = 1;
    assert(initialize() != NDIS_STATUS_SUCCESS);
    assert((pci_command_value() & 4) == 0);
    assert_released();

    /* If neither reset nor revoking bus mastering can be confirmed, the
     * DMA memory is leaked instead of being freed under a live card. */
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    chip.reset_stuck = 1;
    pci_write_ignored = 1;
    chars.halt(adapter);
    assert(shared_live == 17 && map_live == 1);
    assert(!memory_live && !port_live && !interrupt_live && !shutdown_live);
    reclaim_leaked();

    /* The same holds when an NDIS reset is what fails to stop the chip. */
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    chip.reset_stuck = 1;
    pci_write_ignored = 1;
    assert(chars.reset(&addressing, adapter) == NDIS_STATUS_HARD_ERRORS);
    assert(!reg16(RTL_INTERRUPT_MASK));
    chars.halt(adapter);
    assert(shared_live == 17 && map_live == 1 && !memory_live);
    reclaim_leaked();

    /* After failures, a normal initialize still claims the adapter. */
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    halt();
    reset_world();
    assert(initialize() == NDIS_STATUS_SUCCESS);
    halt();
}

int main(void)
{
    test_entry();
    test_initialize();
    test_queries();
    test_filters();
    test_send();
    test_receive();
    test_link_and_errors();
    test_configuration();
    test_failures();
    puts("miniport: init/cleanup, OIDs, send/receive, ISR/DPC, reset passed");
    return 0;
}
