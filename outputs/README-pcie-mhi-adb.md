# PCIe MHI ADB experiment — RM505Q-AE

`kmod-pcie_mhi-6.12.112.1.3.8-r4.apk` is a separate build of the working
Quectel V1.3.8 driver for OpenWrt 25.12.6 / kernel 6.12.112 on the BPI-R4.

It adds only these two host-driver mappings:

- MHI channel 36/37 are declared as the `ADB` service;
- the existing generic MHI character-device driver binds that service, so a
  modem which offers it creates `/dev/mhi_ADB`.

It does **not** change the USB composition, `data_interface`, PCIe/MBIM mode,
APN, firewall, or cellular service.

## Important

This is a discovery build.  Seeing `/dev/mhi_ADB` confirms only that the host
driver has exposed the channel.  It does not prove the modem-side `adbd`
service is enabled.  Do not remove/reload the module while the modem is
running: install it, then reboot the router and wait for the normal delayed
MHI load.

Keep the previous working APK on your Linux PC as the rollback copy:

`kmod-pcie_mhi-6.12.112.1.3.8-r2.apk`

## Install

Copy the new APK to `/tmp` on the router, then run:

```sh
apk --no-network add --allow-untrusted --force-overwrite \
  /tmp/kmod-pcie_mhi-6.12.112.1.3.8-r4-adb-experimental.apk
sync
reboot
```

After the normal MHI delay, inspect only:

```sh
ls -l /dev/mhi_ADB
dmesg | grep -i -E 'mhi|adb' | tail -n 80
```

If `/dev/mhi_ADB` is absent, restore r2 the same way and reboot.  If it is
present, the next step is a small userspace bridge from that character device
to `adb connect` over localhost; do not send raw data to it manually.
