# GiantVM QEMU

GiantVM is a many-to-one virtualization framework built atop the QEMU/KVM hypervisor that consolidates multiple physical servers into a unified virtual machine
Our core technical approach provides a Single System Image without requiring any modifications to the Guest OS or userspace applications.

## Introduction

The `main` branch hosts the DSM (Distributed Shared Memory) version of GiantVM, which can be evaluated over TCP/IP.

The `shared-memory/v10.1.2` branch hosts our code that utilizes shared memory as the underlying communication and memory access mechanism for GiantVM.

To boot GiantVM, a dedicated GiantVM kernel is also required.

```text
https://github.com/GiantVM/GVM-kernel
```

## Installation


### Step 1. Obtain the GiantVM QEMU Source

Clone the GiantVM QEMU repository from GitHub:

```bash
git clone https://github.com/GiantVM/GVM-qemu.git
```

### Step 2. Build GiantVM QEMU


The following packages are required for compilation. Install them as needed:

```bash
sudo apt install -y python3-venv python3-pip python3-setuptools
sudo apt install ninja-build
sudo apt install -y libglib2.0-dev libpixman-1-dev
sudo apt install -y flex bison
```

Enter the GiantVM QEMU source directory:

```bash
cd GVM-qemu
```

Configure QEMU for the x86_64 system emulator with KVM and VNC support:

```bash
./configure \
    --target-list=x86_64-softmmu \
    --enable-kvm \
    --enable-vnc \
    --disable-rdma \
    --disable-werror
```

Build QEMU with parallel jobs. Replace `N` with the number of available CPU cores:

```bash
make -jN
```

After the build completes, verify that the x86_64 QEMU binary was generated:

```bash
./build/qemu-system-x86_64 --version
```

The expected output is `QEMU emulator version 10.1.2`

### Optional: Build a Static Runtime Package

From the QEMU source directory, build the static binary separately:

```bash
mkdir -p build-static
cd build-static
../configure \
    --target-list=x86_64-softmmu \
    --static \
    --without-default-features \
    --enable-kvm \
    --enable-vnc \
    --enable-pixman \
    --disable-rdma \
    --disable-werror
make -jN qemu-system-x86_64
file qemu-system-x86_64
cd ..
```

The `file` output should include `statically linked` or `static-pie linked`.
Package this binary with the `pc-bios` directory from the same source tree:

```bash
mkdir -p ../gvm-qemu-dsm-v10.1.2
cp build-static/qemu-system-x86_64 ../gvm-qemu-dsm-v10.1.2/
cp -a pc-bios ../gvm-qemu-dsm-v10.1.2/
tar -czf ../gvm-qemu-dsm-v10.1.2.tar.gz \
    -C .. gvm-qemu-dsm-v10.1.2
```

## Kernel/QEMU ABI pairing

For TCP DSM, pair this QEMU code with GVM-kernel
[`upstream/7.1-rc3-dsm-fixed`](https://github.com/GiantVM/GVM-kernel/tree/upstream/7.1-rc3-dsm-fixed).
In addition to the remote AP startup and 4KB DSM mapping fixes, this kernel
branch waits for DSM listener initialization, reports listener failures to
userspace, supports prompt listener cleanup and restart, avoids busy polling
in the receive path, separates DSM metadata from MMU SPTE state, and preserves
dirty tracking for writes outside a running-vCPU context. The original kernel
`main` and `upstream/7.1-rc3` do not contain the complete fix set.
Enable both `CONFIG_KVM_DSM_IRQ_FORWARD` and `CONFIG_KVM_DSM` in the host
kernel. The memory capability is `KVM_CAP_X86_DSM=249`; interrupt exits are
`KVM_EXIT_DSM_SEND_IRQ=45`, `KVM_EXIT_DSM_X2_ICR=46`, and
`KVM_EXIT_DSM_APIC_BASE=47`. Both headers must agree on these numbers and
the ioctl payload layouts. The QEMU 10.1.2 version string alone does not
identify this ABI.

## TCP DSM stability notes

For TCP DSM, supply the node addresses with
`-local-cpu ... iplist=...`. All nodes must run the same QEMU revision and
use kernels with matching DSM configurations and ABI definitions. The build
command above disables RDMA; this pairing describes TCP DSM.

This QEMU revision handles positive `KVM_DSM_MEMPIN` access masks correctly,
waits for distributed router listeners, shuts down remote vCPU placeholders
and KVM descriptors in a defined order, and keeps AP-side PCI and device state
coherent. It also filters redundant distributed ICR traffic and improves
cross-node guest clock alignment.

The clock alignment reduces large TSC jumps observed during distributed guest
startup, but it does not guarantee that Linux will retain TSC as a stable
clocksource. A guest may still mark TSC unstable and use `kvm-clock`; that
fallback is not by itself a DSM/TCP functional failure.

## Current stability baseline

The current TCP DSM baseline is the `upstream/tcp` revision paired with
`upstream/7.1-rc3-dsm-fixed` above. Its acceptance boundary is functional:
with two QEMU nodes and two local vCPUs per node, a four-vCPU guest must reach
a shell with CPUs `0-3` online and without a fatal KVM run error, page-state or
MMU lifetime failure. Slow startup and a TSC fallback to `kvm-clock` are
recorded, but are not by themselves reasons to change the DSM protocol or
clock algorithm.

The baseline deliberately does not include READ owner retention, fast-path
page-lock redesign, KTCP concurrency changes, or high-frequency diagnostic
instrumentation. Those are separate follow-up work. Startup timeout or guest
service adjustments used by a demo must remain reversible and must not hide a
crash, memory corruption, or unrecoverable deadlock.
