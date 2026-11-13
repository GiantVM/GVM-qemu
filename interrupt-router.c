/*
 * GVM two-node control channel, derived from the shared-memory prototype.
 * Guest RAM is shared separately; only I/O and coordination use this channel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/sockets.h"
#include "io/channel-socket.h"
#include "system/kvm.h"
#include "system/runstate.h"
#include <linux/kvm.h>

#include "system/system.h"

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "trace/trace-root.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"

#include "hw/intc/ioapic.h"
#include "hw/i386/kvm/clock.h"
#include "hw/core/cpu.h"

#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/cpus.h"

#include "interrupt-router.h"
#include "system/gvm.h"
#include "hw/core/boards.h"

#define GVM_ROUTER_DEFAULT_PORT 40000
#define GVM_ROUTER_CPU_INDEX_ANY -1

/* Completion reply for IRQ route updates; retain the existing wire value. */
#define GVM_ROUTER_IRQ_ROUTES_ACK 99
#define GVM_ROUTER_CONNECT_RETRY_US 100000

struct GvmRouterAddress {
    char host[20];
    char port[10];
};
static char **router_hosts;
static uint32_t router_hosts_num;

static QEMUFile **req_files;
static QEMUFile **rsp_files;
static QIOChannelSocket *router_listener;
static guint router_watch_id;
static GSList *router_connections;
static bool router_stopping;

struct GvmRouterLoopArgs {
    QEMUFile *req_file;
    QEMUFile *rsp_file;
    QIOChannel *channel;
};

typedef struct GvmRouterConnection {
    struct GvmRouterLoopArgs io;
    QemuThread thread;
} GvmRouterConnection;

static QemuMutex gvm_router_io_mutex;
static bool router_ready;

bool gvm_router_is_ready(void)
{
    return qatomic_read(&router_ready) && !qatomic_read(&router_stopping);
}

static int router_parse_iplist(const char *cluster_iplist)
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

enum GvmRouterMessageType {
    GVM_ROUTER_PIO = 1,
    GVM_ROUTER_MMIO = 2,
    GVM_ROUTER_SIPI = 5,
    GVM_ROUTER_X2APIC = 6,
    GVM_ROUTER_APIC_BASE = 7,
    GVM_ROUTER_IOAPIC_INI = 10,
    GVM_ROUTER_IOAPIC = 11,
    GVM_ROUTER_KVMCLOCK = 12,
    GVM_ROUTER_INI_ROUTER = 13,
    GVM_ROUTER_SHUTDOWN = 14,
    GVM_ROUTER_RESET = 15,
};

static void router_handle_pio(uint16_t port, MemTxAttrs attrs, void *data,
                              int direction, int size, uint32_t count)
{
    int i;
    uint8_t *ptr = data;

    for (i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs, ptr, size,
                         direction == KVM_EXIT_IO_OUT);
        ptr += size;
    }
}

static void *io_router_loop(void *arg)
{
    uint16_t type;
    int cpu_index;
    int ret;

    /* GVM_ROUTER_PIO */
    uint16_t port;
    MemTxAttrs attrs;
    int direction;
    int size;
    uint32_t count;

    /* GVM_ROUTER_MMIO */
    hwaddr addr;
    bool is_write;
    int len;

    /* Interrupts */
    uint32_t val;
    uint32_t val2;
    uint32_t dest_id;
    int isrv; /* GVM_ROUTER_IOAPIC */

    uint64_t kvmclock;
    uint64_t x2data;

    int host;
    uint32_t base_index;
    uint64_t base_data;

    void *data;
    struct GvmRouterLoopArgs *argp = (struct GvmRouterLoopArgs *)arg;
    QEMUFile *req_file = argp->req_file;
    QEMUFile *rsp_file = argp->rsp_file;

    trace_gvm_router_loop();

    while (1) {
        type = qemu_get_be16(req_file);
        if (!type || qatomic_read(&router_stopping)) {
            break;
        }

        cpu_index = qemu_get_sbe32(req_file);
        if (cpu_index != GVM_ROUTER_CPU_INDEX_ANY) {
            current_cpu = qemu_get_cpu(cpu_index);
        }
        switch (type) {
        case GVM_ROUTER_PIO:
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

            router_handle_pio(port, attrs, data, direction, size, count);

            if (direction == KVM_EXIT_IO_IN) {
                qemu_put_buffer(rsp_file, data, count * size);
                qemu_fflush(rsp_file);
            }
            free(data);
            break;

        case GVM_ROUTER_MMIO:
            /* AP forward to BSP */
            addr = qemu_get_be64(req_file);
            qemu_get_buffer(req_file, (uint8_t *)&attrs, sizeof(attrs));
            len = qemu_get_sbe32(req_file);
            is_write = qemu_get_sbe32(req_file) ? true : false;
            data = malloc(len);
            qemu_get_buffer(req_file, data, len);

            address_space_rw(&address_space_memory, addr, attrs, data, len,
                             is_write);

            if (!is_write) {
                qemu_put_buffer(rsp_file, data, len);
                qemu_fflush(rsp_file);
            }

            free(data);
            break;
        case GVM_ROUTER_X2APIC:
            x2data = qemu_get_be64(req_file);
            ret = gvm_kvm_inject_x2apic(cpu_index, x2data);
            if (ret < 0) {
                error_report("KVM DSM x2APIC injection for vCPU %d "
                             "failed: %s",
                             cpu_index, strerror(-ret));
            }
            break;
        case GVM_ROUTER_SIPI:
            /* Any CPU sends to multicast/broadcast SIPI targets. */
            val = qemu_get_sbe32(req_file);
            val2 = qemu_get_sbe32(req_file);
            dest_id = qemu_get_be32(req_file);
            ret = gvm_kvm_inject_ipi(cpu_index, val, val2, dest_id);
            if (ret < 0) {
                error_report("KVM DSM IPI injection for vCPU %d failed: %s",
                             cpu_index, strerror(-ret));
            }
            break;
        case GVM_ROUTER_APIC_BASE:
            host = qemu_get_sbe32(req_file);
            base_index = qemu_get_be32(req_file);
            base_data = qemu_get_be64(req_file);
            ret = gvm_kvm_set_apic_base(cpu_index, host, base_index, base_data);
            if (ret < 0) {
                error_report("KVM DSM APIC base update for vCPU %d "
                             "failed: %s",
                             cpu_index, strerror(-ret));
            }
            break;
        case GVM_ROUTER_IOAPIC_INI:
            val = qemu_get_sbe32(req_file);
            val2 = qemu_get_sbe32(req_file);
            gvm_kvm_set_ioapic_irq(val, val2);
            break;
        case GVM_ROUTER_IOAPIC:
            /* AP forward to BSP */
            isrv = qemu_get_sbe32(req_file);
            ioapic_eoi_broadcast(isrv);
            break;

        case GVM_ROUTER_INI_ROUTER:
            /* BSP forward to AP */
            uint32_t nr = qemu_get_be32(req_file);
            uint32_t flags = qemu_get_be32(req_file);

            struct kvm_irq_routing *routing = malloc(
                sizeof(*routing) + nr * sizeof(struct kvm_irq_routing_entry));
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
                    error_report("GVM: unsupported IRQ routing type %u",
                                 entry->type);
                    exit(1);
                }
            }
            gvm_kvm_set_irq_routes(routing);
            qemu_put_be32(rsp_file, GVM_ROUTER_IRQ_ROUTES_ACK);
            qemu_fflush(rsp_file);
            free(routing);
            break;

        case GVM_ROUTER_KVMCLOCK:
            kvmclock = gvm_kvm_get_clock();
            qemu_put_be64(rsp_file, kvmclock);
            qemu_fflush(rsp_file);
            break;

        case GVM_ROUTER_SHUTDOWN:
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            break;
        case GVM_ROUTER_RESET:
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            break;

        default:
            error_report("GVM: unknown I/O router message type %u", type);
            exit(1);
        }

        if (type >= GVM_ROUTER_SHUTDOWN) {
            break;
        }
    }
    return NULL;
}

static int get_router_address(int target_node, struct GvmRouterAddress *addr)
{
    if (addr == NULL) {
        return -EINVAL;
    }

    if (target_node < 0 || target_node >= router_hosts_num) {
        return -EINVAL;
    }
    g_strlcpy(addr->host, router_hosts[target_node], sizeof(addr->host));
    snprintf(addr->port, sizeof(addr->port), "%d",
             GVM_ROUTER_DEFAULT_PORT + target_node);
    return 0;
}
static gboolean io_router_accept_connection(QIOChannel *ioc,
                                            GIOCondition condition,
                                            gpointer user_data)
{
    QIOChannelSocket *sioc;
    QIOChannel *channel;
    Error *err = NULL;
    GvmRouterConnection *connection;

    if (qatomic_read(&router_stopping)) {
        return G_SOURCE_REMOVE;
    }

    sioc = qio_channel_socket_accept(QIO_CHANNEL_SOCKET(ioc), &err);
    if (!sioc) {
        error_report("could not accept io router connection (%s)",
                     error_get_pretty(err));
        exit(1);
    }

    channel = QIO_CHANNEL(sioc);
    qio_channel_set_name(channel, "io-router-connection-channel");

    connection = g_new0(GvmRouterConnection, 1);
    struct GvmRouterLoopArgs *arg = &connection->io;
    arg->req_file = qemu_file_new_input(channel);
    arg->rsp_file = qemu_file_get_return_path(arg->req_file);
    arg->channel = channel;

    router_connections = g_slist_prepend(router_connections, connection);
    qemu_thread_create(&connection->thread, "io-router-connection",
                       io_router_loop, arg, QEMU_THREAD_JOINABLE);

    return true;
}

static void connect_io_router_single(int node_index)
{
    QIOChannel *channel;
    SocketAddress *connect_addr;
    Error *err = NULL;

    connect_addr = g_new0(SocketAddress, 1);

    int ret;
    struct GvmRouterAddress addr;
    memset(&addr, 0, sizeof(addr));

    ret = get_router_address(node_index, &addr);
    if (ret) {
        error_report("GVM: cannot resolve router address for node %d: %d",
                     node_index, ret);
        return;
    }
    connect_addr->type = SOCKET_ADDRESS_TYPE_INET;
    connect_addr->u.inet = (InetSocketAddress){
        .host = g_strdup(addr.host),
        .port = g_strdup(addr.port), /* NULL == Auto-select */
        .has_ipv4 = true,
        .ipv4 = true,
    };
    info_report("GVM: connecting to node %d at %s:%s", node_index, addr.host,
                addr.port);
    channel = QIO_CHANNEL(qio_channel_socket_new());
    qio_channel_set_name(QIO_CHANNEL(channel), "io-send");

    while (true) {
        if (qio_channel_socket_connect_sync(QIO_CHANNEL_SOCKET(channel),
                                            connect_addr, &err) == 0) {
            break;
        }
        error_free(err);
        err = NULL;
        usleep(GVM_ROUTER_CONNECT_RETRY_US);
    }

    qio_channel_set_delay(channel, false);
    req_files[node_index] = qemu_file_new_output(channel);
    rsp_files[node_index] = qemu_file_get_return_path(req_files[node_index]);
    object_unref(channel);
    qapi_free_SocketAddress(connect_addr);
    trace_gvm_router_connected(node_index);
}

static void *qemu_io_router_thread_run(void *arg)
{
    int ret;
    SocketAddress *listen_addr;
    QIOChannelSocket *lioc;
    Error *local_err = NULL;
    listen_addr = g_new0(SocketAddress, 1);
    MachineState *ms = MACHINE(qdev_get_machine());
    int node_index = ms->local_cpu_start_index / ms->local_cpus;

    struct GvmRouterAddress addr;
    memset(&addr, 0, sizeof(addr));

    ret = get_router_address(node_index, &addr);
    if (ret) {
        error_report("GVM: cannot resolve router address for node %d: %d",
                     node_index, ret);
        return NULL;
    }
    listen_addr->type = SOCKET_ADDRESS_TYPE_INET;
    listen_addr->u.inet = (InetSocketAddress){
        .host = g_strdup(addr.host),
        .port = g_strdup(addr.port), /* NULL == Auto-select */
        .has_ipv4 = true,
        .ipv4 = true,
    };
    info_report("GVM: node %d listening on TCP socket %s:%s", node_index,
                addr.host, addr.port);
    lioc = qio_channel_socket_new();
    qio_channel_set_name(QIO_CHANNEL(lioc), "io-router-listener-channel");

    if (qio_channel_socket_listen_sync(lioc, listen_addr, 1, &local_err) < 0) {
        error_report_err(local_err);
        object_unref(OBJECT(lioc));
        qapi_free_SocketAddress(listen_addr);
        exit(1);
    }

    router_listener = lioc;
    router_watch_id = qio_channel_add_watch(
        QIO_CHANNEL(lioc), G_IO_IN, io_router_accept_connection,
        object_ref(lioc), (GDestroyNotify)object_unref);

    qapi_free_SocketAddress(listen_addr);
    return NULL;
}

static void connect_io_router(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    int node_index = ms->local_cpu_start_index / ms->local_cpus;

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        if (i != node_index) {
            connect_io_router_single(i);
        }
    }
}

static void router_shutdown_channel(QIOChannel *channel)
{
    QIOChannelSocket *socket = QIO_CHANNEL_SOCKET(channel);

    /* The peer may already have disconnected. Shutdown each socket once. */
    if (socket->fd >= 0 && shutdown(socket->fd, SHUT_RDWR) < 0 &&
        errno != ENOTCONN) {
        error_report("I/O router shutdown failed: %s", strerror(errno));
    }
}

void gvm_router_stop(void)
{
    GSList *entry;
    bool had_bql;
    uint32_t i;

    if (!gvm_enabled() || qatomic_xchg(&router_stopping, true)) {
        return;
    }

    if (router_watch_id) {
        g_source_remove(router_watch_id);
        router_watch_id = 0;
    }
    if (router_listener) {
        qio_channel_close(QIO_CHANNEL(router_listener), NULL);
        g_clear_pointer(&router_listener, object_unref);
    }

    for (i = 0; req_files && i < router_hosts_num; i++) {
        if (req_files[i]) {
            qemu_file_set_error(req_files[i], -EIO);
            qemu_file_set_error(rsp_files[i], -EIO);
            router_shutdown_channel(qemu_file_get_ioc(req_files[i]));
        }
    }
    for (entry = router_connections; entry; entry = entry->next) {
        GvmRouterConnection *connection = entry->data;

        router_shutdown_channel(connection->io.channel);
    }

    /* A worker finishing a device access can need the BQL. */
    had_bql = bql_locked();
    if (had_bql) {
        bql_unlock();
    }
    for (entry = router_connections; entry; entry = entry->next) {
        GvmRouterConnection *connection = entry->data;

        qemu_thread_join(&connection->thread);
    }
    if (had_bql) {
        bql_lock();
    }

    for (entry = router_connections; entry; entry = entry->next) {
        GvmRouterConnection *connection = entry->data;

        qemu_file_set_error(connection->io.req_file, -EIO);
        qemu_file_set_error(connection->io.rsp_file, -EIO);
        qemu_fclose(connection->io.req_file);
        qemu_fclose(connection->io.rsp_file);
        object_unref(connection->io.channel);
        g_free(connection);
    }
    g_clear_pointer(&router_connections, g_slist_free);
    for (i = 0; req_files && i < router_hosts_num; i++) {
        if (req_files[i]) {
            qemu_fclose(req_files[i]);
            qemu_fclose(rsp_files[i]);
        }
    }
    for (i = 0; i < router_hosts_num; i++) {
        g_free(router_hosts[i]);
    }
    g_clear_pointer(&req_files, g_free);
    g_clear_pointer(&rsp_files, g_free);
    g_clear_pointer(&router_hosts, g_free);
    router_hosts_num = 0;
}

void gvm_router_start(void)
{
    QemuThread listener_thread;

    MachineState *ms = MACHINE(qdev_get_machine());
    if (!gvm_enabled()) {
        return;
    }
    qemu_mutex_init(&gvm_router_io_mutex);
    if (router_parse_iplist(ms->cluster_iplist) < 0) {
        error_report("Invalid GVM peer list");
        exit(EXIT_FAILURE);
    }

    if (router_hosts_num != ms->qemu_nums) {
        error_report("invalid number of cluster iplist");
        exit(1);
    }

    req_files = g_new0(QEMUFile *, ms->qemu_nums);
    rsp_files = g_new0(QEMUFile *, ms->qemu_nums);

    qemu_thread_create(&listener_thread, "io-router-listener",
                       qemu_io_router_thread_run, NULL, QEMU_THREAD_JOINABLE);
    qemu_thread_join(&listener_thread);

    connect_io_router();
    qatomic_set(&router_ready, true);
}

/*
 * Currently we put all devices on QEMU 0, so we only need to forward PIOs on
 * APs to BSP. (except for PIOs to PCI configuration space)
 * TODO: support the conceptial "Virtual PCI-e Bus". @jinzhang
 */
void gvm_router_forward_pio(uint16_t port, MemTxAttrs attrs, void *data,
                            int direction, int size, uint32_t count,
                            bool broadcast)
{
    qemu_mutex_lock(&gvm_router_io_mutex);

    QEMUFile *io_connect_file;
    QEMUFile *io_connect_return_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    if (broadcast) {
        int i;
        for (i = 0; i < ms->qemu_nums; i++) {
            io_connect_file = req_files[i];
            io_connect_return_file = rsp_files[i];

            if (io_connect_file && io_connect_return_file) {
                qemu_put_be16(io_connect_file, GVM_ROUTER_PIO);
                /*
                 * indicate which cpu we are currently operating on, especially
                 * for apic_mem_readl / apic_mem_writel => cpu_get_current_apic
                 */
                qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
                qemu_put_be16(io_connect_file, port);
                qemu_put_buffer(io_connect_file, (uint8_t *)&attrs,
                                sizeof(attrs));
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

        qemu_put_be16(io_connect_file, GVM_ROUTER_PIO);
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

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/*
 * @unicast: AP -> BSP (i.e. QEMU[0])
 *
 * Currently we put all devices on QEMU 0, so we only need to forward MMIOs on
 * APs to BSP.
 * TODO: support the conceptial "Virtual PCI-e Bus". @jinzhang
 */
void gvm_router_forward_mmio(hwaddr addr, MemTxAttrs attrs, uint8_t *data,
                             int len, bool is_write)
{
    qemu_mutex_lock(&gvm_router_io_mutex);

    QEMUFile *io_connect_file = req_files[0];
    QEMUFile *io_connect_return_file = rsp_files[0];

    qemu_put_be16(io_connect_file, GVM_ROUTER_MMIO);
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
    } else {
        qemu_put_sbe32(io_connect_file, 0);
    }
    qemu_put_buffer(io_connect_file, data, len);

    qemu_fflush(io_connect_file);

    if (!is_write) {
        qemu_get_buffer(io_connect_return_file, data, len);
    }

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/* @broadcast */

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void gvm_router_forward_startup(int cpu_index, uint32_t val, uint32_t val2,
                                uint32_t dest_id)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file;

    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == (cpu_index / ms->local_cpus)) {
            continue;
        }
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, GVM_ROUTER_SIPI);
            /* Indicate which CPU we want to forward this interrupt to */
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_be32(io_connect_file, val);
            qemu_put_be32(io_connect_file, val2);
            qemu_put_be32(io_connect_file, dest_id);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

void gvm_router_forward_x2apic(int cpu_index, uint64_t data)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file;

    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == (cpu_index / ms->local_cpus)) {
            continue;
        }
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, GVM_ROUTER_X2APIC);
            /* Indicate which CPU we want to forward this interrupt to */
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_be64(io_connect_file, data);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

void gvm_router_forward_apic_base(int cpu_index, bool host, uint32_t index,
                                  uint64_t data)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    MachineState *ms = MACHINE(qdev_get_machine());
    QEMUFile *io_connect_file;

    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == (cpu_index / ms->local_cpus)) {
            continue;
        }
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, GVM_ROUTER_APIC_BASE);
            /* Indicate which CPU we want to forward this interrupt to */
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_sbe32(io_connect_file, (int)host);
            qemu_put_be32(io_connect_file, index);
            qemu_put_be64(io_connect_file, data);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */

void gvm_router_forward_ioapic_irq(int irq, int level)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    /* Backend cleanup can raise IRQs after the router has been disconnected. */
    if (qatomic_read(&router_stopping)) {
        qemu_mutex_unlock(&gvm_router_io_mutex);
        return;
    }
    QEMUFile *io_connect_file;
    MachineState *ms = MACHINE(qdev_get_machine());
    for (int i = 1; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, GVM_ROUTER_IOAPIC_INI);
            qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
            qemu_put_sbe32(io_connect_file, irq);
            qemu_put_sbe32(io_connect_file, level);
            qemu_fflush(io_connect_file);
        }
    }
    qemu_mutex_unlock(&gvm_router_io_mutex);
}

void gvm_router_forward_irq_routes(struct kvm_irq_routing *routing)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    QEMUFile *io_connect_file = req_files[1];

    qemu_put_be16(io_connect_file, GVM_ROUTER_INI_ROUTER);
    qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
    qemu_put_be32(io_connect_file, routing->nr);
    qemu_put_be32(io_connect_file, routing->flags);

    for (int i = 0; i < routing->nr; i++) {
        struct kvm_irq_routing_entry *entry = &(routing->entries[i]);

        qemu_put_be32(io_connect_file, entry->gsi);
        qemu_put_be32(io_connect_file, entry->type);
        qemu_put_be32(io_connect_file, entry->flags);
        qemu_put_be32(io_connect_file, entry->pad);

        switch (entry->type) {
        case KVM_IRQ_ROUTING_IRQCHIP: {
            struct kvm_irq_routing_irqchip *irq = &(entry->u.irqchip);
            qemu_put_be32(io_connect_file, irq->irqchip);
            qemu_put_be32(io_connect_file, irq->pin);
            break;
        }
        case KVM_IRQ_ROUTING_MSI: {
            struct kvm_irq_routing_msi *msi = &(entry->u.msi);
            qemu_put_be32(io_connect_file, msi->address_lo);
            qemu_put_be32(io_connect_file, msi->address_hi);
            qemu_put_be32(io_connect_file, msi->data);
            qemu_put_be32(io_connect_file, msi->pad);
            break;
        }
        default:
            error_report("GVM: unsupported IRQ routing type %d", entry->type);
            break;
        }
    }

    qemu_fflush(io_connect_file);
    io_connect_file = rsp_files[1];
    if (qemu_get_be32(io_connect_file) != GVM_ROUTER_IRQ_ROUTES_ACK) {
        error_report("GVM IRQ route acknowledgement failed");
    }
    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/* @unicast: AP -> BSP (i.e. QEMU[0]) */
void gvm_router_forward_eoi(int isrv)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    QEMUFile *io_connect_file = req_files[0];

    qemu_put_be16(io_connect_file, GVM_ROUTER_IOAPIC);
    qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
    qemu_put_sbe32(io_connect_file, isrv);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/* @broadcast */
void gvm_router_forward_shutdown(void)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    QEMUFile *io_connect_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, GVM_ROUTER_SHUTDOWN);
            qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/* @broadcast */
void gvm_router_forward_reset(void)
{
    qemu_mutex_lock(&gvm_router_io_mutex);
    QEMUFile *io_connect_file;

    MachineState *ms = MACHINE(qdev_get_machine());

    int i;
    for (i = 0; i < ms->qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, GVM_ROUTER_RESET);
            qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&gvm_router_io_mutex);
}

/* @broadcast */

void gvm_router_fetch_kvmclock(uint64_t *kvmclock)
{
    qemu_mutex_lock(&gvm_router_io_mutex);

    QEMUFile *io_connect_file = req_files[0];
    QEMUFile *io_connect_return_file = rsp_files[0];

    qemu_put_be16(io_connect_file, GVM_ROUTER_KVMCLOCK);
    qemu_put_sbe32(io_connect_file, GVM_ROUTER_CPU_INDEX_ANY);
    qemu_fflush(io_connect_file);

    *kvmclock = qemu_get_be64(io_connect_return_file);
    qemu_mutex_unlock(&gvm_router_io_mutex);
}
