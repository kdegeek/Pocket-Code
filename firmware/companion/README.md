# Companion firmware recovery source

This directory preserves the recoverable ESP-IDF source used by the current
1.75C companion image. It was copied from the clean
`t3-companion-firmware` commit `78a795a493c0fe91f3b0a2e67fe1fe6d733eb12a`.

## Provenance

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C; physical 32 MB flash and
  8 MB PSRAM, with this image configured for a 16 MB logical flash layout.
- Toolchain: ESP-IDF 5.5.5, as pinned by `dependencies.lock` and
  `main/idf_component.yml`.
- The existing UI implementation is retained under `components/ui`.
- The saved build metadata reports historical app version `f412029-dirty`;
  its `t3_companion_firmware` ELF/project prefix matches the saved artifact.

The source is recovery material for a CodexBar adapter. Deploying the
WebSocket chunk fix requires an app-only flash of the built application
partition; bootloader and partition-table flashing are not required. This
source copy does not perform device operations.

The copy includes runtime components, board configuration, generic
`sdkconfig.defaults`, partition layout, managed-dependency lock data, host
tests, simulator, fixtures, and build/release tools. Device-specific
`sdkconfig`, generated build output, managed components, PNG snapshots,
historical/private documents, and `.superpowers` records were omitted. One
host-only regression fixture used a private LAN address in the source; the
copied fixture uses the documentation address `192.0.2.113` instead.

Licensing provenance: the runtime files are project-owned generated source.
ESP registry managed dependencies are referenced by the lock file and are not
vendored here; their upstream licenses apply when they are resolved for a
build. No private project documents were copied.

## Build and update an existing 1.75C

With ESP-IDF 5.5.5 installed and activated:

```sh
cd firmware/companion
idf.py set-target esp32s3
idf.py build
```

For a device already using this exact partition layout, confirm its USB identity
and active OTA slot first. Back up that slot before writing. For active `ota_0`
(the recovered device), its offset is `0x20000` and size is `0x700000`:

```sh
python -m esptool --chip esp32s3 --port DEVICE_PORT read_flash 0x20000 0x700000 ota0-before.bin
python -m esptool --chip esp32s3 --port DEVICE_PORT write_flash 0x20000 build/t3_companion_firmware.bin
```

This changes only the app slot. It preserves the existing partition table,
bootloader, remembered networks, and pairing in NVS. Do not use these offsets for
a device with a different partition layout or active slot. Avoid a full
`idf.py flash` when preserving the enrolled device.

For rollback on the same device/slot, write `ota0-before.bin` back at `0x20000`.
Keep the backup private; it is not part of the repository.

After reboot, `GET /api/companion/v1/provisioning` on the device should report
`phase: "live"` and a nonzero `snapshot_sequence` matching the adapter's
`lastSnapshotSequence` in `/healthz`. A successful hello alone does not prove
that the firmware applied the data.
