#include "qemu/osdep.h"
#include <linux/kvm.h>

#include "system/system.h"

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"

#include "hw/i386/apic.h"
#include "hw/intc/ioapic.h"
#include "hw/i386/kvm/clock.h"
#include "hw/core/cpu.h"

#include "include/system/memory.h"
#include "system/address-spaces.h"
#include "system/runstate.h"

#include "interrupt-router.h"
#include "qom/object_interfaces.h"
#include "hw/boards.h"
#include "hw/i386/apic_internal.h"

#define ROUTER_BUFFER_SIZE 1024

#define ROUTER_DEFAULT_PORT 40000
#define ROUTER_MAX_INDEX 63
#define CPU_INDEX_ANY -1

struct router_address {
    char host[20];
    char port[10];
    int target;
};
static char **router_hosts = NULL;
static uint32_t router_hosts_num = 0;

static QEMUFile **req_files = NULL;
static QEMUFile **rsp_files = NULL;

QemuMutex ipi_mutex;
QemuMutex io_forwarding_mutex;


int parse_cluster_iplist(const char *cluster_iplist)
{
    GPtrArray *hosts;
    gchar **tokens;
    int i;

    if (cluster_iplist == NULL) {
        return -EINVAL;
    }

    if (router_hosts != NULL) {
        return -EALREADY;
    }

    hosts = g_ptr_array_new_with_free_func(g_free);
    tokens = g_strsplit_set(cluster_iplist, " \t\n\r\v\f", -1);

    for (i = 0; tokens[i] != NULL; i++) {
        struct in_addr address;

        if (tokens[i][0] == '\0') {
            continue;
        }

        if (inet_pton(AF_INET, tokens[i], &address) != 1) {
            g_strfreev(tokens);
            g_ptr_array_free(hosts, TRUE);
            return -EINVAL;
        }

        g_ptr_array_add(hosts, g_strdup(tokens[i]));
    }

    g_strfreev(tokens);

    if (hosts->len == 0) {
        g_ptr_array_free(hosts, TRUE);
        return -EINVAL;
    }

    router_hosts_num = hosts->len;
    router_hosts = (char **)g_ptr_array_free(hosts, FALSE);
    return 0;
}

char **get_cluster_iplist(uint32_t *len)
{
    *len = router_hosts_num;
    return router_hosts;
}



enum forward_type {
    PIO = 1,
    MMIO,
    LAPIC,
    SPECIAL_INT,
    SIPI,
    ICR,
    X2APIC,
    APIC_BASE,
    INIT_LEVEL_DEASSERT,
    FIXED_INT,
    IOAPIC_INI,
    IOAPIC,
    KVMCLOCK,
    INI_ROUTER,
    /* IO_ROUTER_EXIT */
    SHUTDOWN,
    RESET,
    EXIT,
};

static void kvm_handle_remote_io(uint16_t port, MemTxAttrs attrs, void *data, int direction,
                          int size, uint32_t count)
{
    int i;
    uint8_t *ptr = data;

    for (i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs,
                         ptr, size,
                         direction == KVM_EXIT_IO_OUT);
        ptr += size;
    }
}

static void *io_router_loop(void *arg)
{
    printf("enter router_loop\n");
    fflush(stdout);
    uint8_t type;
    int cpu_index;
    Error *err = NULL;

    /* PIO */
    uint16_t port;
    MemTxAttrs attrs;
    int direction;
    int size;
    uint32_t count;

    /* MMIO */
    hwaddr addr;
    bool is_write;
    int len;

    /* Interrupts */
    uint32_t val; /* LAPIC */
    uint32_t val2;
    uint32_t dest_id;
    int mask; /* SPECIAL_INT */
    int vector_num; /* SIPI & FIXED_INT */
    int trigger_mode; /* FIXED_INT */
    int isrv; /* IOAPIC */

    uint64_t kvmclock;
    __u64 x2data;

    int host;
    uint32_t base_index;
    __u64 base_data;
    CPUState *router_cpu = NULL;

    void *data;
    struct io_router_loop_arg *argp = (struct io_router_loop_arg *)arg;
    QEMUFile *req_file = argp->req_file;
    QEMUFile *rsp_file = argp->rsp_file;

    while(1) {
        type = qemu_get_be16(req_file);
        if(!type) {
            break;
        }

        cpu_index = qemu_get_sbe32(req_file);
        if (cpu_index != CPU_INDEX_ANY) {
            router_cpu = qemu_get_cpu(cpu_index);
            /* current_cpu is thread-local and is required by APIC/MMIO
             * handlers reached from the router thread. */
            current_cpu = router_cpu;
        }
        switch(type)
        {
            case PIO:
                /* AP forward to BSP */

                /*
                 * indicate which cpu we are currently operating on, especially
                 * for apic_mem_readl / apic_mem_writel => cpu_get_current_apic
                 */
                port = qemu_get_be16(req_file);
                qemu_get_buffer(req_file, (uint8_t *)&attrs, sizeof(attrs));
                direction = qemu_get_sbe32(req_file);
                size = qemu_get_sbe32(req_file);
                count = qemu_get_be32(req_file);
                data = malloc(count * size);
                qemu_get_buffer(req_file, data, count * size);

                kvm_handle_remote_io(port, attrs, data, direction, size, count);

                if (direction == KVM_EXIT_IO_IN) {
                    qemu_put_buffer(rsp_file, data, count * size);
                    qemu_fflush(rsp_file);
                }
                free(data);
                break;

            case MMIO:
                /* AP forward to BSP */
                addr = qemu_get_be64(req_file);
                qemu_get_buffer(req_file, (uint8_t *)&attrs, sizeof(attrs));
                len = qemu_get_sbe32(req_file);
                is_write = qemu_get_sbe32(req_file) ? true : false;
                data = malloc(len);
                qemu_get_buffer(req_file, data, len);

                address_space_rw(&address_space_memory, addr, attrs, data, len, is_write);

               if (!is_write) {
                    qemu_put_buffer(rsp_file, data, len);
                    qemu_fflush(rsp_file);
                }

                free(data);
                break;

            case LAPIC:
                /* Any CPU broadcast to others */
                addr = qemu_get_be64(req_file);
                val = qemu_get_be32(req_file);
                apic_lapic_write(router_cpu, addr, val);
                break;
            case SPECIAL_INT:
                /* Any CPU send to target CPUs of a multicast/broadcast of SMI/NMI/INIT */
                mask = qemu_get_sbe32(req_file);
                cpu_interrupt(router_cpu, mask);
                break;
            case X2APIC:
                x2data = qemu_get_be64(req_file);
                int ret = kvm_x2apic_handle(cpu_index, x2data);
                if (ret < 0) {
                    error_report("KVM DSM x2APIC injection for vCPU %d "
                                 "failed: %s",
                                 cpu_index, strerror(-ret));
                }
                break;
            case SIPI:
            case ICR:
                /* Forward an APIC ICR write to the target CPU. */
                val = qemu_get_sbe32(req_file);
                val2 = qemu_get_sbe32(req_file);
                dest_id = qemu_get_be32(req_file);
                ret = kvm_dipi_forwarding(cpu_index, val, val2, dest_id);
                if (ret < 0) {
                    error_report("KVM DSM IPI injection for vCPU %d failed: %s",
                                 cpu_index, strerror(-ret));
                }
                break;
            case APIC_BASE:
                host = qemu_get_sbe32(req_file);
                base_index = qemu_get_be32(req_file);
                base_data = qemu_get_be64(req_file);
                ret = kvm_apic_base_handle(cpu_index, host, base_index, base_data);
                if (ret < 0) {
                    error_report("KVM DSM APIC base update for vCPU %d "
                                 "failed: %s",
                                 cpu_index, strerror(-ret));
                }
                break;
            case INIT_LEVEL_DEASSERT:
                /* Any CPU send to target CPUs of a multicast/broadcast of INIT Level De-assert */
                apic_init_level_deassert(router_cpu);
                break;
            case FIXED_INT:
                /* Any CPU send to target CPU(s) a lowest-priority/multicast/broadcast interrupt */
                vector_num = qemu_get_sbe32(req_file);
                trigger_mode = qemu_get_sbe32(req_file);
                apic_set_irq_detour(router_cpu, vector_num, trigger_mode);
                break;
            case IOAPIC_INI:
                val = qemu_get_sbe32(req_file);
                val2 = qemu_get_sbe32(req_file);
                kvm_ioapic_irq_handle(val, val2);
                break;
            case IOAPIC:
                /* AP forward to BSP */
                isrv = qemu_get_sbe32(req_file);
                ioapic_eoi_broadcast(isrv);
                break;

            case INI_ROUTER:
                /* BSP forward to AP */
                uint32_t nr = qemu_get_be32(req_file);   
                uint32_t flags = qemu_get_be32(req_file); 

                struct kvm_irq_routing *routing = malloc(sizeof(*routing) + nr * sizeof(struct kvm_irq_routing_entry));
                routing->nr = nr;
                routing->flags = flags;

                for (int i = 0; i < nr; i++) {
                    struct kvm_irq_routing_entry *entry = &(routing->entries[i]);
                    entry->gsi = qemu_get_be32(req_file);
                    entry->type = qemu_get_be32(req_file);
                    entry->flags = qemu_get_be32(req_file);
                    entry->pad = qemu_get_be32(req_file);  

                    switch (entry->type) {
                    case KVM_IRQ_ROUTING_IRQCHIP: {
                        entry->u.irqchip.irqchip = qemu_get_be32(req_file);
                        entry->u.irqchip.pin = qemu_get_be32(req_file);
                        break;
                        }
                    case KVM_IRQ_ROUTING_MSI: {
                        entry->u.msi.address_lo = qemu_get_be32(req_file);
                        entry->u.msi.address_hi = qemu_get_be32(req_file);
                        entry->u.msi.data = qemu_get_be32(req_file);
                        entry->u.msi.pad = qemu_get_be32(req_file);
                        break;
                        }
                    default:
                        printf("unknown router type[%u]\n", entry->type);
                        exit(1);
                    }
                }
                kvm_ioapic_router_handle(routing);
                qemu_put_be32(rsp_file, 99);
                qemu_fflush(rsp_file);
                free(routing);
                break;

            case KVMCLOCK:
                kvmclock = kvmclock_getclock();
                qemu_put_be64(rsp_file, kvmclock);
                qemu_fflush(rsp_file);
                break;

            case SHUTDOWN:
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
                break;
            case RESET:
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                break;
            case EXIT:
                exit(0);
                break;

            default:
                printf("unknown type[%u]\n", type);
                exit(1);
        }

        if (type >= SHUTDOWN) {
            break;
        }
    }
    if (argp->channel) {
        qio_channel_close(argp->channel, &err);
    }
    return NULL;
}


static int get_router_address(int target, struct router_address *addr)
{
    if (addr == NULL) {
        return -EINVAL;
    }

    if (target < 0 || target > ROUTER_MAX_INDEX) {
        return -EINVAL;
    }
    memcpy(addr->host, router_hosts[target], strlen(router_hosts[target]));
    sprintf(addr->port, "%d", ROUTER_DEFAULT_PORT + target);
    addr->target = target;
    return 0;
}
static gboolean io_router_accept_connection(QIOChannel *ioc,
                                                 GIOCondition condition,
                                                 gpointer user_data)
{
    QIOChannelSocket *sioc;
    QIOChannel *channel;
    Error *err = NULL;

    sioc = qio_channel_socket_accept(QIO_CHANNEL_SOCKET(ioc),
                                     &err);
    if (!sioc) {
        error_report("could not accept io router connection (%s)",
                     error_get_pretty(err));
        exit(1);
    }

    
    channel = QIO_CHANNEL(sioc);
    qio_channel_set_name(channel, "io-router-connection-channel");

    QemuThread *thread = g_malloc0(sizeof(QemuThread));
    struct io_router_loop_arg *arg = (struct io_router_loop_arg *)g_malloc0(sizeof(struct io_router_loop_arg));
    memset(arg, 0, sizeof(struct io_router_loop_arg));
    arg->req_file = qemu_file_new_input(channel);
    arg->rsp_file = qemu_file_get_return_path(arg->req_file);
    arg->channel = channel;

    qemu_thread_create(thread, "io-router-connection", io_router_loop,
               arg, QEMU_THREAD_JOINABLE);

    return true;
}

static void connect_io_router_single(int index)
{
    QIOChannel *channel;
    SocketAddress *connect_addr;
    Error *err = NULL;

    connect_addr = g_new0(SocketAddress, 1);

    int ret;
    struct router_address addr;
    memset(&addr, 0, sizeof(addr));

    ret = get_router_address(index, &addr);
    if (ret) {
        printf("get_router_address failed, ret: %d\n", ret);
        return;
    }
    connect_addr->type = SOCKET_ADDRESS_TYPE_INET;
    connect_addr->u.inet = (InetSocketAddress) {
        .host = g_strdup(addr.host),
        .port = g_strdup(addr.port), /* NULL == Auto-select */
        .has_ipv4 = true,
        .ipv4 =true,
    };
    printf("connecting %s:%s\n", addr.host, addr.port);
    channel = QIO_CHANNEL(qio_channel_socket_new());
    qio_channel_set_name(QIO_CHANNEL(channel), "io-send");

    while (true) {
        if (qio_channel_socket_connect_sync(QIO_CHANNEL_SOCKET(channel),
                                            connect_addr,
                                            &err) == 0) {
            break;
        }
        err = NULL;
        usleep(100000);
    }

    qio_channel_set_delay(channel, false);
    req_files[index] = qemu_file_new_output(channel);
    rsp_files[index] = qemu_file_get_return_path(req_files[index]);
    printf("connecting io router done\n");
}

static void *qemu_io_router_thread_run(void *arg)
{
    IORouter *router = arg;
    router->thread_id = qemu_get_thread_id();

    int ret;
    SocketAddress *listen_addr;
    QIOChannelSocket *lioc;
    Error *local_err = NULL;
    listen_addr = g_new0(SocketAddress, 1);
    MachineState *ms = MACHINE(qdev_get_machine());
    int index = ms->local_cpu_start_index / ms->local_cpus;

    struct router_address addr;
    memset(&addr, 0, sizeof(addr));

    ret = get_router_address(index, &addr);
    if (ret) {
        printf("get_router_address failed, ret: %d\n", ret);
        return NULL;
    }
    listen_addr->type = SOCKET_ADDRESS_TYPE_INET;
    listen_addr->u.inet = (InetSocketAddress) {
        .host = g_strdup(addr.host),
        .port = g_strdup(addr.port), /* NULL == Auto-select */
        .has_ipv4 = true,
        .ipv4 =true,
    };
    printf("QEMU %d listen on TCP socket %s:%s\n", index, addr.host, addr.port);
    lioc = qio_channel_socket_new();
    qio_channel_set_name(QIO_CHANNEL(lioc), "io-router-listener-channel");

    if (qio_channel_socket_listen_sync(lioc, listen_addr, 1, &local_err) < 0) {
        printf("io-router listen error, exit...");
        object_unref(OBJECT(lioc));
        qapi_free_SocketAddress(listen_addr);
        exit(1);
    }

    qio_channel_add_watch(QIO_CHANNEL(lioc),
                          G_IO_IN,
                          io_router_accept_connection,
                          lioc,
                          (GDestroyNotify)object_unref);

    qapi_free_SocketAddress(listen_addr);
    return NULL;
}

static void connect_io_router(void)
{
    printf("connect_io_router...\n");

    MachineState *ms = MACHINE(qdev_get_machine());
    int index = ms->local_cpu_start_index / ms->local_cpus;

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        if (i != index) {
            connect_io_router_single(i);
        }
    }
    printf("connect_io_router done\n");

}

void disconnect_io_router(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    if (ms->local_cpus == ms->smp.cpus) {
        return;
    }

    QEMUFile *io_connect_file;
    QEMUFile *io_connect_return_file;

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        io_connect_return_file = rsp_files[i];

        if (io_connect_file && io_connect_return_file) {
            qemu_file_shutdown(io_connect_file);
            qemu_file_shutdown(io_connect_return_file);
        }
        g_free(router_hosts[i]);
    }
    g_free(req_files);
    g_free(rsp_files);
    g_free(router_hosts);
}

void start_io_router(void)
{
    IORouter router;
    router.thread_id = -1;
    int size;

    MachineState *ms = MACHINE(qdev_get_machine());
    qemu_mutex_init(&io_forwarding_mutex);
    qemu_mutex_init(&ipi_mutex);
    if (ms->local_cpus == ms->smp.cpus) {
        return;
    }

    if (router_hosts_num != ms->qemu_nums) {
        error_report("invalid number of cluster iplist");
        exit(1);
    }

    size = ms->qemu_nums * sizeof(QEMUFile *);
    req_files = (QEMUFile **)g_malloc0(size);
    rsp_files = (QEMUFile **)g_malloc0(size);


    qemu_thread_create(&(router.thread), "io-router-listener", qemu_io_router_thread_run,
                   &router, QEMU_THREAD_JOINABLE);

    connect_io_router();
}

/*
 * Currently we put all devices on QEMU 0, so we only need to forward PIOs on
 * APs to BSP. (except for PIOs to PCI configuration space)
 * TODO: support the conceptial "Virtual PCI-e Bus". @jinzhang
 */
void pio_forwarding(uint16_t port, MemTxAttrs attrs, void *data, int direction,
                          int size, uint32_t count, bool broadcast)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file;
    QEMUFile *io_connect_return_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    if (broadcast) {
        int i;
        for (i = 0; i < ms->qemu_nums; i++) {
            io_connect_file = req_files[i];
            io_connect_return_file = rsp_files[i];

            if (io_connect_file && io_connect_return_file) {
                qemu_put_be16(io_connect_file, PIO);
                /*
                 * indicate which cpu we are currently operating on, especially
                 * for apic_mem_readl / apic_mem_writel => cpu_get_current_apic
                 */
                qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
                qemu_put_be16(io_connect_file, port);
                qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
                qemu_put_sbe32(io_connect_file, direction);
                qemu_put_sbe32(io_connect_file, size);
                qemu_put_be32(io_connect_file, count);
                qemu_put_buffer(io_connect_file, data, count * size);

                qemu_fflush(io_connect_file);

                if (direction == KVM_EXIT_IO_IN) {
                    qemu_get_buffer(io_connect_return_file, data, count * size);
                }

            }
        }
    } else {
        /* @unicast: AP -> BSP (i.e. QEMU[0]) */
        io_connect_file = req_files[0];
        io_connect_return_file = rsp_files[0];

        qemu_put_be16(io_connect_file, PIO);
        /*
         * indicate which cpu we are currently operating on, especially for
         * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
         */
        qemu_put_sbe32(io_connect_file, current_cpu->cpu_index);
        qemu_put_be16(io_connect_file, port);
        qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
        qemu_put_sbe32(io_connect_file, direction);
        qemu_put_sbe32(io_connect_file, size);
        qemu_put_be32(io_connect_file, count);
        qemu_put_buffer(io_connect_file, data, count * size);

        qemu_fflush(io_connect_file);

        if (direction == KVM_EXIT_IO_IN) {
            qemu_get_buffer(io_connect_return_file, data, count * size);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/*
 * @unicast: AP -> BSP (i.e. QEMU[0])
 *
 * Currently we put all devices on QEMU 0, so we only need to forward MMIOs on APs to BSP.
 * TODO: support the conceptial "Virtual PCI-e Bus". @jinzhang
 */
void mmio_forwarding(hwaddr addr, MemTxAttrs attrs, uint8_t *data, int len, bool is_write)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file = req_files[0];
    QEMUFile *io_connect_return_file = rsp_files[0];

    qemu_put_be16(io_connect_file, MMIO);
    /*
     * indicate which cpu we are currently operating on, especially for
     * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
     */
    qemu_put_sbe32(io_connect_file, current_cpu->cpu_index);
    qemu_put_be64(io_connect_file, addr);
    qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
    qemu_put_sbe32(io_connect_file, len);
    if (is_write) {
        qemu_put_sbe32(io_connect_file, 1);
    }
    else {
        qemu_put_sbe32(io_connect_file, 0);
    }
    qemu_put_buffer(io_connect_file, data, len);

    qemu_fflush(io_connect_file);


    if (!is_write) {
        qemu_get_buffer(io_connect_return_file, data, len);
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void lapic_forwarding(int cpu_index, hwaddr addr, uint32_t val)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, LAPIC);
            /*
             * indicate which cpu we are currently operating on, especially for
             * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
             */
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_be64(io_connect_file, addr);
            qemu_put_be32(io_connect_file, val);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void special_interrupt_forwarding(int cpu_index, int mask)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file = req_files[cpu_index / ms->local_cpus];

    qemu_put_be16(io_connect_file, SPECIAL_INT);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_put_sbe32(io_connect_file, mask);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

static int icr_target_node(uint32_t val, uint32_t dest_id, int local_cpus)
{
    uint32_t dest_mode = (val >> 11) & 1;
    uint32_t shorthand = (val >> 18) & 3;

    if (dest_mode == 0 && shorthand == 0 &&
        dest_id != 0xff && dest_id != 0xffffffff) {
        return dest_id / local_cpus;
    }

    return -1;
}

/* Forward an ICR write.  Non-physical or broadcast destinations may span
 * multiple QEMU instances, so those messages are sent to every peer. */
void icr_forwarding(int cpu_index, uint32_t reg, uint32_t val,
                    uint32_t dest_id)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file;
    int source_node = cpu_index / ms->local_cpus;
    int target_node = icr_target_node(val, dest_id, ms->local_cpus);

    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == source_node || (target_node >= 0 && i != target_node)) {
            continue;
        }
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, ICR);
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_be32(io_connect_file, reg);
            qemu_put_be32(io_connect_file, val);
            qemu_put_be32(io_connect_file, dest_id);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* Forward a SIPI reached through the userspace APIC path. */
void startup_forwarding(int source_cpu_index, int target_cpu_index,
                        int vector_num)
{
    icr_forwarding(source_cpu_index, 0x300, /* APIC_ICR */
                   APIC_DM_SIPI | (vector_num & APIC_VECTOR_MASK),
                   target_cpu_index);
}

void x2apic_forwarding(int cpu_index, __u64 data)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file;

    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == (cpu_index / ms->local_cpus)) {
            continue;
        }
        io_connect_file = req_files[i];
        if (io_connect_file) {
        qemu_put_be16(io_connect_file, X2APIC);
        /* Indicate which CPU we want to forward this interrupt to */
        qemu_put_sbe32(io_connect_file, cpu_index);
        qemu_put_be64(io_connect_file, data);
        qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

void apic_base_forwarding(int cpu_index, bool host, uint32_t index, __u64 data)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file;

    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == (cpu_index / ms->local_cpus)) {
            continue;
        }
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, APIC_BASE);
            /* Indicate which CPU we want to forward this interrupt to */
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_sbe32(io_connect_file, (int)host);
            qemu_put_be32(io_connect_file, index);
            qemu_put_be64(io_connect_file, data);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void init_level_deassert_forwarding(int cpu_index)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file = req_files[cpu_index / ms->local_cpus];

    qemu_put_be16(io_connect_file, INIT_LEVEL_DEASSERT);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void irq_forwarding(int cpu_index, int vector_num, int trigger_mode)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file = req_files[cpu_index / ms->local_cpus];

    printf("cpu_index:%d ,vector:%d, trigger:%d\n", cpu_index, vector_num, trigger_mode);
    fflush(stdout);

    qemu_put_be16(io_connect_file, FIXED_INT);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_put_sbe32(io_connect_file, vector_num);
    qemu_put_sbe32(io_connect_file, trigger_mode);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

void ioapic_irq_forwarding(int irq, int level)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[0];
    MachineState *ms = MACHINE(qdev_get_machine());
    for (int i = 1; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
           qemu_put_be16(io_connect_file, IOAPIC_INI);
           qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
           qemu_put_sbe32(io_connect_file, irq);
           qemu_put_sbe32(io_connect_file, level);
           qemu_fflush(io_connect_file);
        }
    }
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void ioapic_router_forwarding(struct kvm_irq_routing *routing) {
    qemu_mutex_lock(&io_forwarding_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());

    for (int peer = 1; peer < ms->qemu_nums; peer++) {
        QEMUFile *io_connect_file = req_files[peer];
        QEMUFile *io_connect_return_file = rsp_files[peer];

        if (!io_connect_file || !io_connect_return_file) {
            error_report("missing IO router connection for QEMU %d", peer);
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
            continue;
        }

        qemu_put_be16(io_connect_file, INI_ROUTER);
        qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
        qemu_put_be32(io_connect_file, routing->nr);
        qemu_put_be32(io_connect_file, routing->flags);

        for (int i = 0; i < routing->nr; i++) {
            struct kvm_irq_routing_entry *entry = &routing->entries[i];

            qemu_put_be32(io_connect_file, entry->gsi);
            qemu_put_be32(io_connect_file, entry->type);
            qemu_put_be32(io_connect_file, entry->flags);
            qemu_put_be32(io_connect_file, entry->pad);

            switch (entry->type) {
            case KVM_IRQ_ROUTING_IRQCHIP:
                qemu_put_be32(io_connect_file, entry->u.irqchip.irqchip);
                qemu_put_be32(io_connect_file, entry->u.irqchip.pin);
                break;
            case KVM_IRQ_ROUTING_MSI:
                qemu_put_be32(io_connect_file, entry->u.msi.address_lo);
                qemu_put_be32(io_connect_file, entry->u.msi.address_hi);
                qemu_put_be32(io_connect_file, entry->u.msi.data);
                qemu_put_be32(io_connect_file, entry->u.msi.pad);
                break;
            default:
                error_report("Unsupported routing type: %d", entry->type);
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
                break;
            }
        }

        qemu_fflush(io_connect_file);
        while (qemu_get_be32(io_connect_return_file) != 99) {
        }
    }
    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: AP -> BSP (i.e. QEMU[0]) */
void eoi_forwarding(int isrv)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[0];

    qemu_put_be16(io_connect_file, IOAPIC);
    qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
    qemu_put_sbe32(io_connect_file, isrv);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void shutdown_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, SHUTDOWN);
            qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void reset_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, RESET);
            qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void exit_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;
    MachineState *ms = MACHINE(qdev_get_machine());

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, EXIT);
            qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

void kvmclock_fetching(uint64_t *kvmclock)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file = req_files[0];
    QEMUFile *io_connect_return_file = rsp_files[0];

    qemu_put_be16(io_connect_file, KVMCLOCK);
    qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
    qemu_fflush(io_connect_file);

    *kvmclock = qemu_get_be64(io_connect_return_file);
    qemu_mutex_unlock(&io_forwarding_mutex);
}
