# GiantVM QEMU Guide

This guide describes how to obtain GiantVM QEMU sources and build QEMU

## 1. Obtain the GiantVM QEMU Source

Clone the GiantVM QEMU repository from GitHub:

```bash
git clone https://github.com/GiantVM/GVM-qemu.git
cd GVM-qemu
```


## 2. Build GiantVM QEMU

The following packages are required for compilation. Install them as needed:

```bash
sudo apt install -y python3-venv python3-pip python3-setuptools
sudo apt install ninja-build
sudo apt install -y libglib2.0-dev libpixman-1-dev
sudo apt install -y librdmacm-dev
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

## 3. Obtain the GiantVM Kernel

To boot GiantVM, a dedicated GiantVM kernel is also required.

```text
https://github.com/GiantVM/GVM-kernel
```
