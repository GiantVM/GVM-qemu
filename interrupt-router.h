/*
 * Experimental GVM control channel.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GVM_ROUTER_H
#define GVM_ROUTER_H

#include "exec/hwaddr.h"
#include "exec/memattrs.h"

struct kvm_irq_routing;

#ifdef CONFIG_GVM
bool gvm_router_is_ready(void);
void gvm_router_start(void);
void gvm_router_stop(void);
void gvm_router_forward_mmio(hwaddr addr, MemTxAttrs attrs, uint8_t *data,
                             int len, bool is_write);
void gvm_router_forward_pio(uint16_t port, MemTxAttrs attrs, void *data,
                            int direction, int size, uint32_t count,
                            bool broadcast);
void gvm_router_forward_startup(int cpu_index, uint32_t val, uint32_t val2,
                                uint32_t dest_id);
void gvm_router_forward_x2apic(int cpu_index, uint64_t data);
void gvm_router_forward_apic_base(int cpu_index, bool host, uint32_t index,
                                  uint64_t data);
void gvm_router_forward_ioapic_irq(int irq, int level);
void gvm_router_forward_irq_routes(struct kvm_irq_routing *routing);
void gvm_router_forward_eoi(int vector);
void gvm_router_forward_shutdown(void);
void gvm_router_forward_reset(void);
void gvm_router_fetch_kvmclock(uint64_t *clock);
#else
static inline bool gvm_router_is_ready(void)
{
    return false;
}

static inline void gvm_router_start(void)
{
}

static inline void gvm_router_stop(void)
{
}

static inline void gvm_router_forward_mmio(hwaddr addr, MemTxAttrs attrs,
                                           uint8_t *data, int len,
                                           bool is_write)
{
}

static inline void gvm_router_forward_pio(uint16_t port, MemTxAttrs attrs,
                                          void *data, int direction, int size,
                                          uint32_t count, bool broadcast)
{
}

static inline void gvm_router_forward_startup(int cpu_index, uint32_t val,
                                              uint32_t val2, uint32_t dest_id)
{
}

static inline void gvm_router_forward_x2apic(int cpu_index, uint64_t data)
{
}

static inline void gvm_router_forward_apic_base(int cpu_index, bool host,
                                                uint32_t index, uint64_t data)
{
}

static inline void gvm_router_forward_ioapic_irq(int irq, int level)
{
}

static inline void
gvm_router_forward_irq_routes(struct kvm_irq_routing *routing)
{
}

static inline void gvm_router_forward_eoi(int vector)
{
}

static inline void gvm_router_forward_shutdown(void)
{
}

static inline void gvm_router_forward_reset(void)
{
}

static inline void gvm_router_fetch_kvmclock(uint64_t *clock)
{
}

#endif

#endif /* GVM_ROUTER_H */
