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

For TCP DSM, pair this `main` code with GVM-kernel
[`upstream/7.1-rc3`](https://github.com/GiantVM/GVM-kernel/tree/upstream/7.1-rc3).
This kernel branch includes the remote AP startup and 4KB DSM mapping fixes;
the original kernel `main` does not include these fixes.
Enable both `CONFIG_KVM_DSM_IRQ_FORWARD` and `CONFIG_KVM_DSM` in the host
kernel. The memory capability is `KVM_CAP_X86_DSM=249`; interrupt exits are
`KVM_EXIT_DSM_SEND_IRQ=45`, `KVM_EXIT_DSM_X2_ICR=46`, and
`KVM_EXIT_DSM_APIC_BASE=47`. Both headers must agree on these numbers and
the ioctl payload layouts. The QEMU 10.1.2 version string alone does not
identify this ABI.

The shared-memory pair instead uses GVM-kernel `shared-memory/v7.2-rc2`
and GVM-qemu `shared-memory/v10.1.2`. Its interrupt capability is 250 and its
unified exit reason is 44. QEMU enables that capability explicitly and
decodes `dsm_irq_forward`, rather than the legacy APIC-base payload.

For TCP DSM, omit `-shm-path` and supply the node addresses with
`-local-cpu ... iplist=...`. The two nodes must run matching QEMU builds.
The build command above disables RDMA; this pairing describes TCP DSM.
