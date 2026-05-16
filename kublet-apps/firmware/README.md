# Firmware

This folder contains a backup of the firmware binaries previously shared in the kublet discord channel. No guarantees that these will work, this is just to document previously shared communication before the project went dark. This may be usefull to recover in case the Kublet gets bricked.

Download esptool with pip or use the uv project files in this directory:

```
pip install esptool
```

or 

```
uv sync
```

Flash Firmware:
```
esptool.py -p /dev/cu.usbserial-0001 -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x10000 firmware.bin
```

Flash bootloader and firmware:
```
esptool.py -p /dev/cu.usbserial-0001 -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

Flash dev firmware:
```
esptool.py -p /dev/cu.usbserial-0001 -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x8000 dev_partitions.bin 0x10000 dev_firmware.bin
````

Note: You need to first store wifi credential in kublet, and that can only be done if an app has been uploaded at least once from the mobile app.