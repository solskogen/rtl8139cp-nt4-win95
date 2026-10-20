/* SPDX-License-Identifier: MIT
 * Serialized NDIS 3.0 binary-compatible miniport for the RTL8139C+. The same
 * binary runs on Windows NT 4.0 and Windows 95. */
#include "ndis_miniport.h"
#include "portio.h"
#include "rtl8139.h"

#define RTL_DRIVER_MARKER UINT32_C(0x8139d000)
#define RTL_IO_SIZE 256u
#define CONTROL_SIZE 4096u
#define CONTROL_RX_RING 1024u
#define CONTROL_TALLY 2048u
#define CHUNK_BUFFERS 8u
#define CHUNK_COUNT (2u * RTL_RING_COUNT / CHUNK_BUFFERS)
#define CHUNK_SIZE (CHUNK_BUFFERS * RTL_BUFFER_SIZE)
#define MAX_MULTICAST 32u
#define MAX_LOOKAHEAD 1500u
#define HEADER_SIZE 14u
#define DPC_ROUNDS 8u
#define RX_BUDGET 64u
#define VENDOR_DRIVER_VERSION 0x00010000u
#define PACKET_FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
                        NDIS_PACKET_TYPE_ALL_MULTICAST |                       \
                        NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS)
#define ENABLED_INTERRUPTS (RTL_INT_RX_OK | RTL_INT_RX_ERROR | RTL_INT_TX_OK |  \
                            RTL_INT_TX_ERROR | RTL_INT_RX_UNAVAILABLE |        \
                            RTL_INT_LINK_CHANGE | RTL_INT_RX_FIFO |            \
                            RTL_INT_SYSTEM_ERROR)

/* Error log detail: RTL_DRIVER_MARKER | step, followed by the status. */
enum init_step {
    STEP_ALLOCATE = 1,
    STEP_CONFIGURATION,
    STEP_MEDIA,
    STEP_VLAN,
    STEP_NETWORK_ADDRESS,
    STEP_SLOT,
    STEP_RESOURCES,
    STEP_PORT_RESOURCE,
    STEP_INTERRUPT_RESOURCE,
    STEP_PORT_RANGE,
    STEP_PCI_COMMAND,
    STEP_IDENTIFY,
    STEP_MAP_REGISTERS,
    STEP_SHARED_MEMORY,
    STEP_START,
    STEP_INTERRUPT
};

struct shared_block {
    void *virtual_address;
    uint64_t physical;
    uint32_t size;
    ndis_boolean cached;
};

struct receive_context {
    const uint8_t *frame;
    unsigned size;
};

/* Tally counters reported through OIDs; the chip clears them on reset. */
struct counters {
    uint32_t rx_missed;
    uint32_t rx_alignment;
    uint32_t tx_one_collision;
    uint32_t tx_multi_collision;
};

struct adapter {
    ndis_handle handle;
    struct rtl_device device;
    struct ndis_miniport_interrupt interrupt;
    struct shared_block control;
    struct shared_block chunks[CHUNK_COUNT];
    struct receive_context rx;
    struct counters counter_base;
    uint32_t io;
    void *io_mapped;
    uint32_t interrupt_level;
    uint32_t interrupt_vector;
    uint32_t diag[4];
    uint32_t slot;
    int shutdown_registered;
    int port_registered;
    int map_registers;
    int interrupt_registered;
    int hardware_started;
    int dma_unsafe;
    int interrupts_allowed;
    int needs_reset;
    int send_blocked;
    volatile uint16_t interrupt_mask;
    unsigned link_mbps;
    unsigned hang_tail;
    unsigned hang_ticks;
    uint32_t tx_rejected;
    uint32_t rx_dropped;
    uint32_t filter;
    uint32_t lookahead;
    unsigned multicast_count;
    uint8_t multicast[MAX_MULTICAST * 6];
    uint8_t permanent[6];
    uint8_t scratch[RTL_BUFFER_SIZE];
};

static void copy(void *dst, const void *src, unsigned count)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (count--)
        *d++ = *s++;
}

static void zero(void *dst, unsigned count)
{
    uint8_t *d = dst;
    while (count--)
        *d++ = 0;
}

static uint32_t load32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

/* Event data words: marker|step, status, then four step-specific
 * diagnostic words (see docs/DRIVER.md). */
static void log_error(ndis_handle handle, uint32_t code, enum init_step step,
                      ndis_status status, const uint32_t diag[4])
{
    NdisWriteErrorLogEntry(handle, code, 6, RTL_DRIVER_MARKER | (uint32_t)step,
                           status, diag[0], diag[1], diag[2], diag[3]);
}

static uint8_t bus_read8(void *c, unsigned r)
{
    return port_in8(((struct adapter *)c)->io + r);
}

static uint16_t bus_read16(void *c, unsigned r)
{
    return port_in16(((struct adapter *)c)->io + r);
}

static uint32_t bus_read32(void *c, unsigned r)
{
    return port_in32(((struct adapter *)c)->io + r);
}

static void bus_write8(void *c, unsigned r, uint8_t v)
{
    port_out8(((struct adapter *)c)->io + r, v);
}

static void bus_write16(void *c, unsigned r, uint16_t v)
{
    port_out16(((struct adapter *)c)->io + r, v);
}

static void bus_write32(void *c, unsigned r, uint32_t v)
{
    port_out32(((struct adapter *)c)->io + r, v);
}

/* TCTR counts PCI clocks (33 MHz). The read cap bounds the wait even if the
 * counter does not advance or the bus clock is slower. */
static void bus_stall(void *c, unsigned us)
{
    struct adapter *a = c;
    uint32_t target = us * 33u;
    uint32_t cap = us * 64u + 64u;
    port_out32(a->io + RTL_TIMER, 0);
    while (cap-- && port_in32(a->io + RTL_TIMER) < target)
        ;
}

static void bus_barrier(void *c)
{
    (void)c;
    cpu_barrier();
}

static ndis_boolean NDIS_CALL sync_mask(void *context)
{
    struct adapter *a = context;
    uint16_t mask = a->interrupts_allowed ? ENABLED_INTERRUPTS : 0;
    a->interrupt_mask = mask;
    if (a->port_registered)
        port_out16(a->io + RTL_INTERRUPT_MASK, mask);
    return 1;
}

/* Applies the mask implied by interrupts_allowed, synchronized with the ISR. */
static void update_mask(struct adapter *a)
{
    if (a->interrupt_registered)
        NdisMSynchronizeWithInterrupt(&a->interrupt, sync_mask, a);
    else
        sync_mask(a);
}

static void *ethernet_filter(struct adapter *a)
{
    void *filter;
    copy(&filter, (const uint8_t *)a->handle + NDIS_MINIPORT_BLOCK_ETHDB,
         sizeof(filter));
    return filter;
}

/* Running totals: hardware counters restart at every chip reset. */
static void read_counters(struct adapter *a, struct counters *out)
{
    struct rtl_tally t;
    *out = a->counter_base;
    if (rtl_dump_tally(&a->device, &t) == RTL_OK) {
        out->rx_missed += t.rx_missed;
        out->rx_alignment += t.rx_alignment;
        out->tx_one_collision += t.tx_one_collision;
        out->tx_multi_collision += t.tx_multi_collision;
    }
}

static void refresh_link(struct adapter *a)
{
    unsigned mbps;
    if (rtl_link(&a->device, &mbps) == RTL_OK)
        a->link_mbps = mbps;
    else
        a->needs_reset = 1;
}

/* PCI command register (offset 4): I/O decode and bus mastering. */
static int pci_read_command(struct adapter *a, uint16_t *command)
{
    *command = 0;
    return NdisReadPciSlotInformation(a->handle, a->slot, 4, command, 2) == 2 &&
           *command != UINT16_MAX;
}

static int pci_command(struct adapter *a, uint16_t set, uint16_t clear)
{
    uint16_t command;
    if (!pci_read_command(a, &command))
        return 0;
    command = (uint16_t)((command | set) & ~clear);
    return NdisWritePciSlotInformation(a->handle, a->slot, 4, &command, 2) == 2;
}

/* Revokes bus mastering and confirms it by reading the command back. */
static int bus_master_off(struct adapter *a)
{
    uint16_t command;
    return pci_command(a, 0, 4) && pci_read_command(a, &command) &&
           !(command & 4);
}

/* Stops all DMA. If neither a chip reset nor revoking bus mastering can be
 * confirmed, the DMA memory is marked unsafe and is leaked by release()
 * rather than handed back while the card may still write to it. */
static void quiesce(struct adapter *a)
{
    a->interrupts_allowed = 0;
    update_mask(a);
    if (!a->hardware_started)
        return;
    a->dma_unsafe = rtl_stop(&a->device) != RTL_OK && !bus_master_off(a);
    a->hardware_started = 0;
}

static void release(struct adapter *a)
{
    unsigned i;
    if (a->interrupt_registered) {
        NdisMDeregisterInterrupt(&a->interrupt);
        a->interrupt_registered = 0;
    }
    if (a->shutdown_registered) {
        NdisMDeregisterAdapterShutdownHandler(a->handle);
        a->shutdown_registered = 0;
    }
    for (i = 0; i < CHUNK_COUNT && !a->dma_unsafe; ++i) {
        struct shared_block *s = &a->chunks[i];
        if (s->virtual_address)
            NdisMFreeSharedMemory(a->handle, s->size, s->cached,
                                  s->virtual_address, s->physical);
        s->virtual_address = 0;
    }
    if (a->control.virtual_address && !a->dma_unsafe)
        NdisMFreeSharedMemory(a->handle, a->control.size, a->control.cached,
                              a->control.virtual_address, a->control.physical);
    a->control.virtual_address = 0;
    if (a->map_registers && !a->dma_unsafe)
        NdisMFreeMapRegisters(a->handle);
    a->map_registers = 0;
    if (a->port_registered)
        NdisMDeregisterIoPortRange(a->handle, a->io, RTL_IO_SIZE, a->io_mapped);
    a->port_registered = 0;
}

static void NDIS_CALL adapter_shutdown(void *context)
{
    struct adapter *a = context;
    /* May run at high IRQL during bugcheck: no NDIS calls, no waits. */
    port_out16(a->io + RTL_INTERRUPT_MASK, 0);
    port_out8(a->io + RTL_COMMAND, 0x10);
}

static int read_integer(ndis_handle configuration, const uint16_t *name,
                        unsigned bytes, uint32_t *value)
{
    struct ndis_string keyword = {(uint16_t)bytes, (uint16_t)bytes, name};
    struct ndis_configuration_parameter *parameter = 0;
    ndis_status status;
    NdisReadConfiguration(&status, &parameter, configuration, &keyword,
                          NDIS_PARAMETER_INTEGER);
    if (status != NDIS_STATUS_SUCCESS || !parameter ||
        parameter->type != NDIS_PARAMETER_INTEGER)
        return 0;
    *value = parameter->data.integer;
    return 1;
}

#define KEYWORD(name) name, (unsigned)(sizeof(name) - sizeof(name[0]))
static const uint16_t key_media[] = u"ConnectionType";
static const uint16_t key_vlan[] = u"VlanId";
static const uint16_t key_slot[] = u"SlotNumber";

struct settings {
    struct rtl_config config;
    int override;
};

static ndis_status read_settings(struct adapter *a, ndis_handle context,
                                 struct settings *s, uint32_t *code,
                                 enum init_step *step)
{
    ndis_handle configuration = 0;
    ndis_status status;
    uint32_t value;
    void *address = 0;
    uint32_t length = 0;
    NdisOpenConfiguration(&status, &configuration, context);
    if (status != NDIS_STATUS_SUCCESS) {
        *code = NDIS_ERROR_OUT_OF_RESOURCES;
        *step = STEP_CONFIGURATION;
        return status;
    }
    status = NDIS_STATUS_SUCCESS;
    *code = NDIS_ERROR_UNSUPPORTED_CONFIGURATION;
    if (read_integer(configuration, KEYWORD(key_media), &value)) {
        if (value > RTL_MEDIA_100_FULL) {
            *step = STEP_MEDIA;
            status = NDIS_STATUS_INVALID_DATA;
            goto done;
        }
        s->config.media = (enum rtl_media)value;
    }
    if (read_integer(configuration, KEYWORD(key_vlan), &value)) {
        if (value > 4094) {
            *step = STEP_VLAN;
            status = NDIS_STATUS_INVALID_DATA;
            goto done;
        }
        s->config.vlan_id = (uint16_t)value;
    }
    /* NT4: the PCI device number. Win95 resolves the slot from the device
     * node and ignores it, so it defaults to 0. */
    if (read_integer(configuration, KEYWORD(key_slot), &value)) {
        if (value > 0xff) {
            *step = STEP_SLOT;
            status = NDIS_STATUS_INVALID_DATA;
            goto done;
        }
        a->slot = value;
    }
    NdisReadNetworkAddress(&status, &address, &length, configuration);
    if (status == NDIS_STATUS_SUCCESS && length) {
        if (length != 6 || !address || !rtl_valid_mac(address)) {
            *code = NDIS_ERROR_NETWORK_ADDRESS;
            *step = STEP_NETWORK_ADDRESS;
            status = NDIS_STATUS_INVALID_DATA;
            goto done;
        }
        copy(s->config.mac, address, 6);
        s->override = 1;
    }
    status = NDIS_STATUS_SUCCESS;
done:
    NdisCloseConfiguration(configuration);
    return status;
}

static ndis_status get_resources(struct adapter *a, uint32_t *code,
                                 enum init_step *step)
{
    struct ndis_resource_list *list = 0;
    ndis_status status;
    uint32_t i, id = 0;
    int port = 0, interrupt = 0;
    /* diag: slot, PCI ID read there */
    *code = NDIS_ERROR_ADAPTER_NOT_FOUND;
    *step = STEP_SLOT;
    a->diag[0] = a->slot;
    if (NdisReadPciSlotInformation(a->handle, a->slot, 0, &id, 4) != 4 ||
        id != UINT32_C(0x813910ec)) {
        a->diag[1] = id;
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    *code = NDIS_ERROR_RESOURCE_CONFLICT;
    *step = STEP_RESOURCES;
    status = NdisMPciAssignResources(a->handle, a->slot, &list);
    if (status != NDIS_STATUS_SUCCESS)
        return status;
    if (!list)
        return NDIS_STATUS_FAILURE;
    for (i = 0; i < list->count; ++i) {
        const struct ndis_resource_descriptor *d = &list->descriptors[i];
        if (d->type == NDIS_RESOURCE_PORT && !port) {
            port = 1;
            *step = STEP_PORT_RESOURCE;
            if (d->u.port.start_high || d->u.port.length < RTL_IO_SIZE ||
                d->u.port.start_low > 0x10000 - RTL_IO_SIZE ||
                (d->u.port.start_low & (RTL_IO_SIZE - 1)))
                return NDIS_STATUS_FAILURE;
            a->io = d->u.port.start_low;
        } else if (d->type == NDIS_RESOURCE_INTERRUPT && !interrupt) {
            interrupt = 1;
            a->interrupt_level = d->u.interrupt.level;
            a->interrupt_vector = d->u.interrupt.vector;
        }
    }
    if (!port) {
        *step = STEP_PORT_RESOURCE;
        return NDIS_STATUS_FAILURE;
    }
    if (!interrupt) {
        *step = STEP_INTERRUPT_RESOURCE;
        return NDIS_STATUS_FAILURE;
    }
    return NDIS_STATUS_SUCCESS;
}

static ndis_status allocate_shared(struct adapter *a, struct shared_block *s,
                                   uint32_t size, ndis_boolean cached,
                                   uint32_t alignment)
{
    s->size = size;
    s->cached = cached;
    s->virtual_address = 0;
    s->physical = 0;
    NdisMAllocateSharedMemory(a->handle, size, cached, &s->virtual_address,
                              &s->physical);
    if (!s->virtual_address)
        return NDIS_STATUS_RESOURCES;
    if (!s->physical || s->physical > UINT32_MAX - (size - 1) ||
        (s->physical & (alignment - 1)) ||
        ((uintptr_t)s->virtual_address & (alignment - 1)))
        return NDIS_STATUS_FAILURE;
    return NDIS_STATUS_SUCCESS;
}

static ndis_status setup(struct adapter *a, ndis_handle context, uint32_t *code,
                         enum init_step *step)
{
    struct settings s;
    struct rtl_bus bus = {a, bus_read8, bus_read16, bus_read32, bus_write8,
                          bus_write16, bus_write32, bus_stall, bus_barrier};
    struct rtl_dma dma;
    ndis_status status;
    enum rtl_result r;
    unsigned i;
    zero(&s, sizeof(s));
    zero(&dma, sizeof(dma));
    status = read_settings(a, context, &s, code, step);
    if (status != NDIS_STATUS_SUCCESS)
        return status;
    status = get_resources(a, code, step);
    if (status != NDIS_STATUS_SUCCESS)
        return status;

    *code = NDIS_ERROR_RESOURCE_CONFLICT;
    *step = STEP_PORT_RANGE;
    status = NdisMRegisterIoPortRange(&a->io_mapped, a->handle, a->io, RTL_IO_SIZE);
    if (status != NDIS_STATUS_SUCCESS)
        return status;
    a->port_registered = 1;

    *code = NDIS_ERROR_ADAPTER_NOT_FOUND;
    *step = STEP_PCI_COMMAND;
    if (!pci_command(a, 5, 0))
        return NDIS_STATUS_ADAPTER_NOT_FOUND;

    *step = STEP_IDENTIFY;
    r = rtl_identify(&bus, a->permanent);
    if (r != RTL_OK) {
        /* diag: I/O base, TxConfig (C+ = 748xxxxx), slot */
        a->diag[0] = a->io;
        a->diag[1] = bus_read32(a, RTL_TX_CONFIG);
        a->diag[2] = a->slot;
    }
    if (r == RTL_UNSUPPORTED)
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    *code = NDIS_ERROR_HARDWARE_FAILURE;
    if (r != RTL_OK)
        return NDIS_STATUS_FAILURE;
    if (!s.override) {
        if (!rtl_valid_mac(a->permanent)) {
            *code = NDIS_ERROR_NETWORK_ADDRESS;
            return NDIS_STATUS_FAILURE;
        }
        copy(s.config.mac, a->permanent, 6);
    }

    *code = NDIS_ERROR_OUT_OF_RESOURCES;
    *step = STEP_MAP_REGISTERS;
    status = NdisMAllocateMapRegisters(a->handle, 0, 1, 1, RTL_BUFFER_SIZE);
    if (status != NDIS_STATUS_SUCCESS)
        return status;
    a->map_registers = 1;

    *step = STEP_SHARED_MEMORY;
    status = allocate_shared(a, &a->control, CONTROL_SIZE, 0, 256);
    if (status != NDIS_STATUS_SUCCESS)
        return status;
    for (i = 0; i < CHUNK_COUNT; ++i) {
        status = allocate_shared(a, &a->chunks[i], CHUNK_SIZE, 1, 8);
        if (status != NDIS_STATUS_SUCCESS)
            return status;
    }
    zero(a->control.virtual_address, CONTROL_SIZE);
    dma.tx_ring = a->control.virtual_address;
    dma.tx_ring_bus = (uint32_t)a->control.physical;
    dma.rx_ring = (struct rtl_descriptor *)((uint8_t *)a->control.virtual_address +
                                            CONTROL_RX_RING);
    dma.rx_ring_bus = (uint32_t)a->control.physical + CONTROL_RX_RING;
    dma.tally = (struct rtl_tally *)((uint8_t *)a->control.virtual_address +
                                     CONTROL_TALLY);
    dma.tally_bus = (uint32_t)a->control.physical + CONTROL_TALLY;
    for (i = 0; i < 2 * RTL_RING_COUNT; ++i) {
        struct shared_block *c = &a->chunks[i / CHUNK_BUFFERS];
        uint32_t offset = (i % CHUNK_BUFFERS) * RTL_BUFFER_SIZE;
        uint8_t *va = (uint8_t *)c->virtual_address + offset;
        uint32_t address = (uint32_t)c->physical + offset;
        if (i < RTL_RING_COUNT) {
            dma.rx_buffer[i] = va;
            dma.rx_bus[i] = address;
        } else {
            dma.tx_buffer[i - RTL_RING_COUNT] = va;
            dma.tx_bus[i - RTL_RING_COUNT] = address;
        }
    }

    *code = NDIS_ERROR_HARDWARE_FAILURE;
    *step = STEP_START;
    if (rtl_init(&a->device, &bus, &dma, &s.config) != RTL_OK)
        return NDIS_STATUS_FAILURE;
    a->hardware_started = 1;
    if (rtl_start(&a->device) != RTL_OK ||
        rtl_set_filter(&a->device, 0, 0, 0) != RTL_OK)
        return NDIS_STATUS_FAILURE;
    refresh_link(a);
    if (a->needs_reset)
        return NDIS_STATUS_FAILURE;

    *code = NDIS_ERROR_INTERRUPT_CONNECT;
    *step = STEP_INTERRUPT;
    status = NdisMRegisterInterrupt(&a->interrupt, a->handle,
                                    a->interrupt_vector, a->interrupt_level, 1, 1,
                                    NDIS_INTERRUPT_LEVEL_SENSITIVE);
    if (status != NDIS_STATUS_SUCCESS)
        return status;
    a->interrupt_registered = 1;
    NdisMRegisterAdapterShutdownHandler(a->handle, a, adapter_shutdown);
    a->shutdown_registered = 1;
    a->interrupts_allowed = 1;
    update_mask(a);
    return NDIS_STATUS_SUCCESS;
}

static ndis_status NDIS_CALL initialize(ndis_status *open_error,
                                       uint32_t *selected, uint32_t *media,
                                       uint32_t count, ndis_handle handle,
                                       ndis_handle context)
{
    struct adapter *a = 0;
    ndis_status status;
    uint32_t code = NDIS_ERROR_OUT_OF_RESOURCES;
    enum init_step step = STEP_ALLOCATE;
    uint32_t i;
    *open_error = NDIS_STATUS_SUCCESS;
    for (i = 0; i < count && media[i] != 0; ++i) /* NdisMedium802_3 */
        ;
    if (i == count)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    *selected = i;
    status = NdisAllocateMemory((void **)&a, sizeof(*a), 0, UINT64_MAX);
    if (status != NDIS_STATUS_SUCCESS || !a) {
        static const uint32_t none[4];
        log_error(handle, code, step, status, none);
        return NDIS_STATUS_RESOURCES;
    }
    zero(a, sizeof(*a));
    a->handle = handle;
    a->lookahead = MAX_LOOKAHEAD;
    NdisMSetAttributes(handle, a, 1, NDIS_INTERFACE_PCI);
    status = setup(a, context, &code, &step);
    if (status != NDIS_STATUS_SUCCESS) {
        log_error(handle, code, step, status, a->diag);
        quiesce(a);
        release(a);
        NdisFreeMemory(a, sizeof(*a), 0);
        return status;
    }
    return NDIS_STATUS_SUCCESS;
}

static void NDIS_CALL halt(ndis_handle context)
{
    struct adapter *a = context;
    quiesce(a);
    release(a);
    NdisFreeMemory(a, sizeof(*a), 0);
}

static void NDIS_CALL disable_interrupt(ndis_handle context)
{
    struct adapter *a = context;
    a->interrupt_mask = 0;
    port_out16(a->io + RTL_INTERRUPT_MASK, 0);
}

static void NDIS_CALL enable_interrupt(ndis_handle context)
{
    sync_mask(context);
}

static void NDIS_CALL isr(ndis_boolean *recognized, ndis_boolean *queue,
                          ndis_handle context)
{
    struct adapter *a = context;
    uint16_t status;
    *recognized = *queue = 0;
    if (!a->interrupt_mask)
        return;
    status = port_in16(a->io + RTL_INTERRUPT_STATUS);
    if (status == UINT16_MAX || !(status & a->interrupt_mask))
        return;
    a->interrupt_mask = 0;
    port_out16(a->io + RTL_INTERRUPT_MASK, 0);
    *recognized = *queue = 1;
}

static void receive_frame(void *context, const uint8_t *frame, unsigned size)
{
    struct adapter *a = context;
    void *filter = ethernet_filter(a);
    if (!filter) {
        ++a->rx_dropped;
        return;
    }
    a->rx.frame = frame;
    a->rx.size = size;
    EthFilterDprIndicateReceive(filter, &a->rx, frame, frame, HEADER_SIZE,
                                frame + HEADER_SIZE, size - HEADER_SIZE,
                                size - HEADER_SIZE);
    a->rx.frame = 0;
}

static void resources_available(struct adapter *a)
{
    if (a->send_blocked && a->device.tx_count < RTL_RING_COUNT) {
        a->send_blocked = 0;
        NdisMSendResourcesAvailable(a->handle);
    }
}

static void NDIS_CALL handle_interrupt(ndis_handle context)
{
    struct adapter *a = context;
    unsigned round, received = 0;
    for (round = 0; round < DPC_ROUNDS; ++round) {
        uint16_t pending = 0;
        unsigned done, rx;
        if (rtl_read_interrupts(&a->device, &pending) != RTL_OK ||
            rtl_ack_interrupts(&a->device, pending) != RTL_OK) {
            a->needs_reset = 1;
            break;
        }
        if (pending & RTL_INT_SYSTEM_ERROR)
            a->needs_reset = 1;
        if (pending & RTL_INT_LINK_CHANGE)
            refresh_link(a);
        done = rtl_reap_tx(&a->device);
        rx = rtl_poll_rx(&a->device, RX_BUDGET, receive_frame, a);
        received += rx;
        if (a->needs_reset || (!pending && !done && !rx))
            break;
    }
    if (received && ethernet_filter(a))
        EthFilterDprIndicateReceiveComplete(ethernet_filter(a));
    resources_available(a);
    if (a->needs_reset)
        a->interrupts_allowed = 0;
    update_mask(a);
}

static ndis_status NDIS_CALL send(ndis_handle context, void *packet_pointer,
                                 uint32_t flags)
{
    struct adapter *a = context;
    struct ndis_packet *packet = packet_pointer;
    struct ndis_buffer *buffer;
    unsigned total = 0;
    enum rtl_result r;
    (void)flags;
    if (a->needs_reset || !a->device.running || a->device.faulted)
        return NDIS_STATUS_FAILURE;
    for (buffer = packet->head; buffer; buffer = buffer->next) {
        void *va = 0;
        uint32_t length = 0;
        NdisQueryBuffer(buffer, &va, &length);
        if (length > RTL_MAX_FRAME - total || (length && !va)) {
            ++a->tx_rejected;
            return NDIS_STATUS_FAILURE;
        }
        copy(a->scratch + total, va, length);
        total += length;
    }
    r = rtl_send(&a->device, a->scratch, total);
    if (r == RTL_FULL) {
        rtl_reap_tx(&a->device);
        r = rtl_send(&a->device, a->scratch, total);
        if (r == RTL_FULL) {
            a->send_blocked = 1;
            return NDIS_STATUS_RESOURCES;
        }
    }
    if (r == RTL_OK)
        return NDIS_STATUS_SUCCESS;
    ++a->tx_rejected; /* a faulted core is reported by check_for_hang */
    return NDIS_STATUS_FAILURE;
}

static ndis_status NDIS_CALL transfer(void *packet_pointer, uint32_t *transferred,
                                     ndis_handle context, ndis_handle receive,
                                     uint32_t offset, uint32_t count)
{
    struct ndis_packet *packet = packet_pointer;
    const struct receive_context *rx = receive;
    struct ndis_buffer *buffer;
    const uint8_t *source;
    unsigned available;
    (void)context;
    *transferred = 0;
    if (!rx->frame) /* only valid during an indication */
        return NDIS_STATUS_FAILURE;
    available = rx->size - HEADER_SIZE;
    if (offset > available)
        return NDIS_STATUS_FAILURE;
    if (count > available - offset)
        count = available - offset;
    source = rx->frame + HEADER_SIZE + offset;
    for (buffer = packet->head; buffer && count; buffer = buffer->next) {
        void *va = 0;
        uint32_t length = 0;
        NdisQueryBuffer(buffer, &va, &length);
        if (length > count)
            length = count;
        if (length && !va)
            return NDIS_STATUS_FAILURE;
        copy(va, source, length);
        source += length;
        count -= length;
        *transferred += length;
    }
    return NDIS_STATUS_SUCCESS;
}

static ndis_status restart(struct adapter *a)
{
    a->interrupts_allowed = 0;
    update_mask(a);
    read_counters(a, &a->counter_base);
    if (rtl_stop(&a->device) != RTL_OK) {
        if (!bus_master_off(a))
            a->dma_unsafe = 1;
        return NDIS_STATUS_HARD_ERRORS;
    }
    /* Restores bus mastering if an earlier failed reset revoked it. */
    if (!pci_command(a, 5, 0) || rtl_start(&a->device) != RTL_OK ||
        rtl_set_filter(&a->device, a->filter & PACKET_FILTERS, a->multicast,
                       a->multicast_count) != RTL_OK)
        return NDIS_STATUS_HARD_ERRORS;
    a->needs_reset = 0;
    a->hang_ticks = 0;
    refresh_link(a);
    if (a->needs_reset)
        return NDIS_STATUS_HARD_ERRORS;
    a->interrupts_allowed = 1;
    update_mask(a);
    resources_available(a);
    return NDIS_STATUS_SUCCESS;
}

static ndis_status NDIS_CALL reset(ndis_boolean *addressing, ndis_handle context)
{
    *addressing = 0;
    return restart(context);
}

static ndis_boolean NDIS_CALL check_for_hang(ndis_handle context)
{
    struct adapter *a = context;
    if (a->needs_reset || a->device.faulted || !a->device.running)
        return 1;
    if (rtl_reap_tx(&a->device))
        resources_available(a);
    refresh_link(a);
    if (a->needs_reset)
        return 1;
    /* Without link a reset cannot help; the ring backpressures instead. */
    if (a->link_mbps && a->device.tx_count && a->device.tx_tail == a->hang_tail) {
        if (++a->hang_ticks >= 3) {
            a->hang_ticks = 0;
            return 1;
        }
    } else {
        a->hang_ticks = 0;
        a->hang_tail = a->device.tx_tail;
    }
    return 0;
}

static const uint32_t supported_oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_MAC_OPTIONS, OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_XMIT_OK, OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS
};

static const char vendor_description[] = "Realtek RTL8139C+ Fast Ethernet";

static ndis_status NDIS_CALL query(ndis_handle context, uint32_t oid,
                                   void *buffer, uint32_t size,
                                   uint32_t *written, uint32_t *needed)
{
    struct adapter *a = context;
    struct counters counters;
    uint32_t value = 0;
    uint16_t short_value;
    uint8_t vendor[4];
    const void *source = &value;
    uint32_t length = 4;
    *written = *needed = 0;
    switch (oid) {
    case OID_GEN_SUPPORTED_LIST:
        source = supported_oids;
        length = sizeof(supported_oids);
        break;
    case OID_GEN_HARDWARE_STATUS:
        value = a->needs_reset || a->device.faulted ? 4 /* NotReady */ : 0;
        break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
        value = 0; /* NdisMedium802_3 */
        break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        value = MAX_LOOKAHEAD;
        break;
    case OID_GEN_LINK_SPEED: /* 100 bps units */
        value = (a->link_mbps ? a->link_mbps : 100) * 10000u;
        break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
    case OID_GEN_RECEIVE_BUFFER_SPACE:
        value = RTL_RING_COUNT * RTL_BUFFER_SIZE;
        break;
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        value = RTL_BUFFER_SIZE;
        break;
    case OID_GEN_VENDOR_ID:
        copy(vendor, a->permanent, 3);
        vendor[3] = 0;
        source = vendor;
        break;
    case OID_GEN_VENDOR_DESCRIPTION:
        source = vendor_description;
        length = sizeof(vendor_description);
        break;
    case OID_GEN_CURRENT_PACKET_FILTER:
        value = a->filter;
        break;
    case OID_GEN_CURRENT_LOOKAHEAD:
        value = a->lookahead;
        break;
    case OID_GEN_DRIVER_VERSION:
        short_value = 0x0300;
        source = &short_value;
        length = 2;
        break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
        value = RTL_MAX_FRAME;
        break;
    case OID_GEN_MAC_OPTIONS:
        value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                NDIS_MAC_OPTION_RECEIVE_SERIALIZED |
                NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
        break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        refresh_link(a);
        value = a->link_mbps ? 0 : 1;
        break;
    case OID_GEN_VENDOR_DRIVER_VERSION:
        value = VENDOR_DRIVER_VERSION;
        break;
    case OID_GEN_XMIT_OK:
        value = a->device.stats.tx_ok;
        break;
    case OID_GEN_RCV_OK:
        value = a->device.stats.rx_ok;
        break;
    case OID_GEN_XMIT_ERROR:
        value = a->device.stats.tx_error + a->tx_rejected;
        break;
    case OID_GEN_RCV_ERROR:
        value = a->device.stats.rx_error;
        break;
    case OID_GEN_RCV_NO_BUFFER:
        read_counters(a, &counters);
        value = counters.rx_missed + a->rx_dropped;
        break;
    case OID_802_3_PERMANENT_ADDRESS:
        source = a->permanent;
        length = 6;
        break;
    case OID_802_3_CURRENT_ADDRESS:
        source = a->device.config.mac;
        length = 6;
        break;
    case OID_802_3_MULTICAST_LIST:
        source = a->multicast;
        length = a->multicast_count * 6;
        break;
    case OID_802_3_MAXIMUM_LIST_SIZE:
        value = MAX_MULTICAST;
        break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
        read_counters(a, &counters);
        value = counters.rx_alignment;
        break;
    case OID_802_3_XMIT_ONE_COLLISION:
        read_counters(a, &counters);
        value = counters.tx_one_collision;
        break;
    case OID_802_3_XMIT_MORE_COLLISIONS:
        read_counters(a, &counters);
        value = counters.tx_multi_collision;
        break;
    default:
        return NDIS_STATUS_INVALID_OID;
    }
    if (size < length) {
        *needed = length;
        return NDIS_STATUS_INVALID_LENGTH;
    }
    copy(buffer, source, length);
    *written = length;
    return NDIS_STATUS_SUCCESS;
}

/* Arguments are validated, so this fails only on a stopped or faulted chip,
 * which check_for_hang already reports; reset reapplies the stored filter. */
static void apply_filter(struct adapter *a)
{
    (void)rtl_set_filter(&a->device, a->filter & PACKET_FILTERS, a->multicast,
                         a->multicast_count);
}

static ndis_status NDIS_CALL set(ndis_handle context, uint32_t oid, void *buffer,
                                 uint32_t size, uint32_t *read, uint32_t *needed)
{
    struct adapter *a = context;
    const uint8_t *data = buffer;
    uint32_t value, i;
    *read = *needed = 0;
    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_PROTOCOL_OPTIONS:
        if (size < 4) {
            *needed = 4;
            return NDIS_STATUS_INVALID_LENGTH;
        }
        value = load32(data);
        if (oid == OID_GEN_CURRENT_PACKET_FILTER) {
            if (value & ~(PACKET_FILTERS | NDIS_PACKET_TYPE_ALL_LOCAL))
                return NDIS_STATUS_NOT_SUPPORTED;
            a->filter = value;
            apply_filter(a);
        } else if (oid == OID_GEN_CURRENT_LOOKAHEAD) {
            if (value > MAX_LOOKAHEAD)
                return NDIS_STATUS_INVALID_DATA;
            a->lookahead = value;
        }
        *read = 4;
        return NDIS_STATUS_SUCCESS;
    case OID_802_3_MULTICAST_LIST:
        if (size % 6) {
            *needed = (size / 6 + 1) * 6;
            return NDIS_STATUS_INVALID_LENGTH;
        }
        if (size / 6 > MAX_MULTICAST)
            return NDIS_STATUS_MULTICAST_FULL;
        for (i = 0; i < size; i += 6)
            if (!(data[i] & 1))
                return NDIS_STATUS_INVALID_DATA;
        copy(a->multicast, data, size);
        a->multicast_count = size / 6;
        apply_filter(a);
        *read = size;
        return NDIS_STATUS_SUCCESS;
    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static struct ndis3_characteristics characteristics = {
    .major = 3,
    .minor = 0,
    .check_for_hang = check_for_hang,
    .disable_interrupt = disable_interrupt,
    .enable_interrupt = enable_interrupt,
    .halt = halt,
    .handle_interrupt = handle_interrupt,
    .initialize = initialize,
    .isr = isr,
    .query = query,
    .reset = reset,
    .send = send,
    .set = set,
    .transfer = transfer
};

ndis_status NDIS_CALL DriverEntry(void *driver, void *registry)
{
    ndis_handle wrapper = 0;
    ndis_status status;
    NdisInitializeWrapper(&wrapper, driver, registry, 0);
    if (!wrapper)
        return NDIS_STATUS_FAILURE;
    status = NdisMRegisterMiniport(wrapper, &characteristics,
                                  sizeof(characteristics));
    if (status != NDIS_STATUS_SUCCESS)
        NdisTerminateWrapper(wrapper, 0);
    return status;
}
