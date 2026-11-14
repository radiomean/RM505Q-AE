# RM505Q-AE router recovery kit

This kit restores the router-side parts of the tested BPI-R4 / RM505Q-AE
deployment after a clean reflash of the **same** OpenWrt build.

It does not flash or modify the modem.  QManager itself lives in the modem's
`/usrdata` storage and is therefore outside this router recovery package.

## Required compatibility

The included MHI kernel-module APK is only for:

- OpenWrt 25.12.6
- `mediatek/filogic`, `aarch64_cortex-a53`
- kernel `6.12.112-1-f0a7348f9cb959e1bf4d0447669b031e`

Do not install that kernel APK after flashing any other build or kernel.

## Recovery order

1. Install the usual LuCI/MBIM prerequisites from the matching OpenWrt image
   or feeds (`luci-base`, `rpcd`, `ucode`, and the normal MBIM tools).
2. Ensure the stock `kmod-mhi-pci-generic` is absent.
3. Install the matching custom driver APK locally with `apk --no-network add
   --allow-untrusted`.
4. Install `luci-app-quectel-mhi_1.2.1-r1_aarch64_cortex-a53.apk` locally with
   the same command.
5. Reboot once.  The MHI driver intentionally waits 90 seconds before it is
   inserted.  Enter the APN and credentials in LuCI's Quectel MHI page.

The LuCI APK restores:

- `mhi-at`, diagnostic/controls pages and the Quectel-CM dialler;
- the delayed MHI driver loader with MBIM enabled;
- the cellular interface/WAN-zone wiring when absent;
- persistent local ADB bridge and LAN-only QManager proxy;
- the authenticated, one-command **Modem ADB Console** in LuCI;
- QManager URL: `https://192.168.8.1:8443/` after the modem is ready.

It deliberately does not contain a SIM APN, username, or password.
