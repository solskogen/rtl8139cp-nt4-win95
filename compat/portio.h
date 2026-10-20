/* SPDX-License-Identifier: MIT
 * x86 port I/O for ring-0 NDIS miniports on NT4 and Win95. NT4's DDK maps
 * NdisRaw*Port* to HAL; using the instructions directly keeps both drivers
 * importing only NDIS.SYS. NdisMRegisterIoPortRange returns the bus port
 * number itself on x86. Host tests supply these functions instead. */
#ifndef RTL_PORTIO_H
#define RTL_PORTIO_H

#include <stdint.h>

#if defined(RTL_HOST_TEST)
uint8_t port_in8(uint32_t);
uint16_t port_in16(uint32_t);
uint32_t port_in32(uint32_t);
void port_out8(uint32_t, uint8_t);
void port_out16(uint32_t, uint16_t);
void port_out32(uint32_t, uint32_t);
void cpu_barrier(void);
#else
static inline uint8_t port_in8(uint32_t port)
{
    uint8_t v;
    __asm__ volatile("inb %w1, %0" : "=a"(v) : "Nd"((uint16_t)port));
    return v;
}

static inline uint16_t port_in16(uint32_t port)
{
    uint16_t v;
    __asm__ volatile("inw %w1, %0" : "=a"(v) : "Nd"((uint16_t)port));
    return v;
}

static inline uint32_t port_in32(uint32_t port)
{
    uint32_t v;
    __asm__ volatile("inl %w1, %0" : "=a"(v) : "Nd"((uint16_t)port));
    return v;
}

static inline void port_out8(uint32_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %w1" : : "a"(v), "Nd"((uint16_t)port));
}

static inline void port_out16(uint32_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %w1" : : "a"(v), "Nd"((uint16_t)port));
}

static inline void port_out32(uint32_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %w1" : : "a"(v), "Nd"((uint16_t)port));
}

/* Full ordering for coherent DMA memory and port I/O; valid on i486. */
static inline void cpu_barrier(void)
{
    __asm__ volatile("lock; addl $0, (%%esp)" : : : "memory", "cc");
}
#endif

#endif
