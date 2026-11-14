# RM505Q-AE PCIe MHI driver: install and AT test

## Built artifact

- Package: `kmod-pcie_mhi-6.12.112.1.3.8-r1.apk`
- Target: OpenWrt 25.12.6, `mediatek/filogic`, `aarch64_cortex-a53`
- Required kernel ABI: `6.12.112~f0a7348f9cb959e1bf4d0447669b031e-r1`
- SHA-256: `bf2e4546a5e52bf6caf6b35a5738c5fb9ac62d9d4036057d3f149ccdaa2a7fa7`

The package was compiled against the matching official SDK kernel headers. It
contains Quectel PCIe MHI driver V1.3.8 and claims the RM505Q-AE PCI ID
`17cb:0306`.

## Before touching the router

Use a serial console or another recovery path. Swapping this driver will
disconnect all modem interfaces and can interrupt the current management path.
The Quectel driver must never be loaded at the same time as the upstream
`mhi-pci-generic` driver.

On the router, first confirm this package is an exact ABI match:

```sh
uname -r
opkg status kernel 2>/dev/null | sed -n '1,12p'
ls -l /sys/bus/pci/devices/0003:01:00.0
```

`uname -r` must be `6.12.112`. `apk add` below also checks the full kernel ABI
dependency and must be allowed to fail rather than forcing installation.

Copy the APK to `/tmp`, verify it, and install it:

```sh
sha256sum /tmp/kmod-pcie_mhi-6.12.112.1.3.8-r1.apk
apk add --allow-untrusted /tmp/kmod-pcie_mhi-6.12.112.1.3.8-r1.apk
```

The shown checksum must match the one above. Do not use `--force-depends`.

## Swap the driver and open AT

Close ModemManager and any modem connection process first. Then, from the
router's local console:

```sh
dev=0003:01:00.0

# Confirm the generic driver owns the modem before unbinding it.
readlink /sys/bus/pci/devices/$dev/driver

echo "$dev" > /sys/bus/pci/drivers/mhi-pci-generic/unbind
modprobe pcie_mhi

# Normally the PCI ID table binds automatically. Check ownership.
readlink /sys/bus/pci/devices/$dev/driver
dmesg | tail -n 100
ls -l /dev/mhi_DUN /dev/mhi_* 2>/dev/null
```

The expected PCI driver name is `mhi_q`; the expected AT endpoint is
`/dev/mhi_DUN`. If the driver loads but does not bind automatically, bind it
explicitly:

```sh
echo "$dev" > /sys/bus/pci/drivers/mhi_q/bind
```

When `/dev/mhi_DUN` exists, use the vendor driver's intended interface:

```sh
busybox microcom /dev/mhi_DUN
```

At the interactive prompt, press Enter if needed, then enter `ATI` and press
Enter. A healthy modem should return Quectel identification text followed by
`OK`. `AT+CPIN?` is a safe second check.

## Recovery

If the vendor driver does not produce `/dev/mhi_DUN`, close the terminal,
unload it, reload the generic driver, and bind the modem back:

```sh
rmmod pcie_mhi
modprobe mhi-pci-generic
echo 0003:01:00.0 > /sys/bus/pci/drivers/mhi-pci-generic/bind
```

If either unbind or rebind gets stuck, power-cycle the modem/router through the
out-of-band recovery path. Do not use this experimental driver for routine
network service until the direct AT test is stable.

## Source changes

The recovered package source under `Documents/i-x20/outputs/pcie_mhi-openwrt-package`
was updated for Linux 6.12 API changes: `noop_llseek`, a const-correct MHI bus
match callback, an explicit hrtimer header, and `strscpy`. Vendor-only
missing-prototype and unused-function warnings remain visible but non-fatal;
all other compiler warnings stay fatal.
