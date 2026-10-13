# GiantVM QEMU Guide

This guide describes how to obtain GiantVM QEMU sources and build QEMU

## 1. Obtain the GiantVM QEMU Source

Clone the GiantVM QEMU repository from GitHub:

```bash
git clone --branch shared-memory/v10.1.2 https://github.com/GiantVM/GVM-qemu.git
```


## 2. Build GiantVM QEMU

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

### Optional Static Package

From the QEMU source directory, build a static runtime separately:

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
mkdir -p ../gvm-qemu-shm-v10.1.2
cp build-static/qemu-system-x86_64 ../gvm-qemu-shm-v10.1.2/
cp -a pc-bios ../gvm-qemu-shm-v10.1.2/
tar -czf ../gvm-qemu-shm-v10.1.2.tar.gz \
    -C .. gvm-qemu-shm-v10.1.2
```

The `file` output should include `statically linked` or `static-pie linked`.
Use a separate package name so the shared-memory runtime is identifiable.

## 3. Obtain the GiantVM Kernel

To boot GiantVM, a dedicated GiantVM kernel is also required.

```text
https://github.com/GiantVM/GVM-kernel/tree/shared-memory/v7.2-rc2
```

For a two-node shared-memory VM, pair this branch with kernel
`shared-memory/v7.2-rc2` (Linux 7.2-rc2), enable
`CONFIG_KVM_DSM_IRQ_FORWARD`, and boot that kernel on the host. Pass the same
`-shm-path` to both QEMU processes. The guest can use an ordinary Linux kernel.
Run both QEMU processes on the same host; `-shm-path` is a filesystem path
such as `/dev/shm/gvm-shared-memory`. Disable host APICv with
`kvm_intel.enable_apicv=0` and reboot, as described in the kernel README.

The 7.2-rc2 host uses capability `KVM_CAP_X86_DSM_IRQ_FORWARD` (250) and the
unified `KVM_EXIT_DSM_IRQ_FORWARD` (44) payload. QEMU must enable the capability
and inject each forwarded ICR on both the local node and remote nodes. At
startup QEMU prints `GVM: enabled unified DSM IRQ forwarding ABI (cap 250)`.
The older 7.1-rc3/7.1-rc6 ABI uses separate exit reasons 45, 46, and 47;
QEMU retains that path when capability 250 is absent. A QEMU version string
such as `10.1.2` alone does not identify a compatible GiantVM ABI.

Kernel `upstream/7.1-rc3` and QEMU `main` provide TCP DSM instead.
Do not use this shared-memory branch for TCP DSM: its DSM memory transport
is disabled.

The two-node boot and remote CPU execution checks were verified with the
default CPU model and the provided Linux 6.6.71 guest. An additional
`-cpu host` check brought all CPUs online but then hit TSC skew and an RCU
stall. Clock consistency and that CPU model need further investigation.
