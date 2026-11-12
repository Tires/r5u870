# r5u870 V4L2 Driver

Ricoh R5U870 USB camera driver, ported from V4L1 to V4L2/videobuf2 for modern Linux kernels.

## Supported Hardware

| USB ID | Model | Firmware |
|--------|-------|----------|
| 05ca:1830 | Sony VGP-VCC2 (SZ) | r5u870_1830.fw |
| 05ca:1832 | Sony VGP-VCC3 | r5u870_1832.fw |
| 05ca:1833 | Sony VGP-VCC2 (VAIO AR1) | r5u870_1833.fw |
| 05ca:1834 | Sony VGP-VCC2 (VAIO AR) | r5u870_1834.fw |
| 05ca:1835 | Sony VGP-VCC5 | r5u870_1835.fw |
| 05ca:1836 | Sony VGP-VCC4 | r5u870_1836.fw |
| 05ca:1837 | Sony VGP-VCC4 (VFlip) | r5u870_1836.fw |
| 05ca:1839 | Sony VGP-VCC6 | r5u870_1836.fw |
| 05ca:183a | Sony VGP-VCC7 | r5u870_183a.fw |
| 05ca:183b | Sony VGP-VCC8 | r5u870_183b.fw |
| 05ca:183e | Sony VGP-VCC9 | r5u870_183e.fw |
| 05ca:1841 | Fujitsu F01 | r5u870_1841.fw |
| 05ca:1870 | HP Webcam 1000 / HP Pavilion Webcam | r5u870_1870*.fw |
| 05ca:1810 | HP Pavilion Webcam (UVC) | r5u870_1810.fw |
| 05ca:1812 | HP Pavilion Webcam (UVC, no FW) | — |

## Requirements

- Linux kernel >= 5.10 (tested on 7.0.0-42-generic)
- Firmware files in `/lib/firmware/` (see table above)
- `build-essential`, `linux-headers-$(uname -r)`

## Build

```bash
make clean && make
```

To build against a specific kernel:

```bash
make KVER=6.8.0-40-generic
# or
make KDIR=/usr/src/linux-6.8.0-40-generic
```

## Install

```bash
sudo make install
# or manually:
sudo insmod r5u870.ko
```

## Module Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `fw_name` | charp | (model table) | Override firmware filename (without `/lib/firmware/` prefix) |
| `dv1000` | int | 2 | HP dv1000 detect mode (0=no, 1=yes, 2=DMI) |

## Usage

```bash
# List devices
v4l2-ctl --list-devices

# Show all capabilities
v4l2-ctl --all

# Capture 10 frames to a raw file
v4l2-ctl --stream-mmap --stream-count=10 --stream-to=/tmp/test.raw

# Set brightness
v4l2-ctl --set-ctrl brightness=80
```

## Notes

- The camera is **not** UVC-compatible in the standard sense; `uvcvideo` will fail to probe.
- If `uvcvideo` auto-binds on enumeration, unbind it before loading `r5u870`:
  ```bash
  echo "1-2:1.0" > /sys/bus/usb/drivers/uvcvideo/unbind
  ```
- The original driver was removed from the mainline Linux kernel.
- The `usbcam_skel.c` file is a reference skeleton for minidriver development and is **not** compiled into the module.

## Project Structure

```
r5u870/
├── Makefile              # Top-level build (kernel module)
├── Kbuild                # Kernel build system file
├── r5u870_drv.c          # Main driver (model table, firmware, controls)
├── usbcam/
│   ├── usbcam.h          # Public API header
│   ├── usbcam_priv.h     # Private/internal header
│   ├── usbcam_dev.c      # V4L2 device registration
│   ├── usbcam_fops.c     # V4L2 file operations / ioctl handlers
│   ├── usbcam_buf.c      # videobuf2 buffer management + URB streaming
│   ├── usbcam_ctrl.c     # Control allocation and management
│   ├── usbcam_util.c     # Utility functions (altsetting, hexdump)
│   └── usbcam_skel.c     # Reference skeleton (not compiled)
└── README.md
```

## License

GPL-2.0

## Credits

- Original driver: Sam Revitch (2007), Alexander Hixon (2008)
- V4L2/videobuf2 port: Tires (2025)
