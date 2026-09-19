# Companion firmware recovery source

This directory preserves the recoverable ESP-IDF source used by the current
1.75C companion image. It was copied from the clean
`t3-companion-firmware` commit `78a795a493c0fe91f3b0a2e67fe1fe6d733eb12a`.

## Provenance

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C; physical 32 MB flash and
  8 MB PSRAM, with this image configured for a 16 MB logical flash layout.
- Toolchain: ESP-IDF 5.5.5, as pinned by `dependencies.lock` and
  `main/idf_component.yml`.
- The protocol/UI model remains under `components/ui`. The 1.75C now uses
  the approved Obsidian usage face in `components/obsidian_display`.
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

## Obsidian usage face

The 466×466 native LVGL view reproduces the approved Obsidian preview: a smoked
glass center, three illuminated weekly usage rings, large daily token total,
30-day token total, and colored Codex/OpenAI, Claude, and Grok glyphs. There is
no heading, status dot, or box around each percentage. Labels sit clear of the
inner ring. The provider order is Codex, Claude, Grok from outside inward and
left to right.

The background is baked from the approved browser preview; dynamic text and
gradient arcs are rendered on-device. Ring updates ease to their new value over
900 ms. Browser refraction and pointer-driven glass deformation are captured
in the background rather than running React on the ESP32. All fonts and image
assets are compiled into flash; the device downloads no assets.

The board composes each dirty rectangle once in a full-size RGB565 PSRAM buffer
(434,312 bytes), then presents it through a persistent 12-line internal DMA
buffer (11,184 bytes). Each DMA transfer completes before that staging buffer
is reused; LVGL is released only after the complete rectangle has been sent.
This avoids both temporary SPI allocations and repeatedly rendering the same
scene for every 12-line strip. A transfer failure latches a display fault while
retaining its DMA buffer, rather than rebooting or reusing memory still in flight.

Zero-length arcs stay visible as transparent objects: toggling their hidden
flag would invalidate the entire ring bounds at the first animation step. The
build defaults enable LVGL style/radius caches, performance optimization, and
the ESP32-S3's supported 240 MHz CPU setting. After updating an existing local
`sdkconfig`, verify these settings there as defaults only seed new builds.

The LVGL worker owns touch acquisition. The application copies its completed
state and point while holding the same display mutex; it never calls a second
`lv_indev_read()` concurrently with the worker. Startup brightness and lock/wake
brightness commands also take this mutex because the SPI device does not support
concurrent parameter and color transactions from different tasks.

Daily and 30-day totals come from the existing CodexBar adapter's token summary.
Missing or disconnected token data displays `--`; unavailable weekly windows
display `--` with an empty track. Weekly numbers and rings use the same weekly
window (the legacy model's five-hour fields are not used by this face).
Provisioning and enrollment retain their existing setup screens.

### Render the actual native view on a Mac or Linux host

After resolving the pinned ESP-IDF managed components once, the headless
renderer uses the same LVGL view, fonts, image textures, and animation callbacks
as the device:

```sh
cmake -S simulator/obsidian -B /tmp/pocket-code-obsidian -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/pocket-code-obsidian --parallel
/tmp/pocket-code-obsidian/obsidian_preview /tmp/obsidian.ppm
/tmp/pocket-code-obsidian/obsidian_preview /tmp/obsidian-limits.ppm limits
/tmp/pocket-code-obsidian/obsidian_preview /tmp/obsidian-unavailable.ppm unavailable
/tmp/pocket-code-obsidian/obsidian_preview /tmp/obsidian-animation.ppm animation
```

The `limits` case exercises 0%, 100%, and a longer token count. The `animation`
case advances LVGL's clock and captures the settled result. Run
`ctest --test-dir /tmp/pocket-code-obsidian --output-on-failure` to also check that
the first two animation steps update less than half the display; the old hidden
flag behavior repainted 198,304 pixels and fails this guard. Asset provenance,
licenses, and regeneration instructions live in `components/obsidian_assets`.

Hardware verification on the 1.75C (2026-09-16) measured the previous large
animation frames at 1.02–1.12 seconds. The revised cold three-ring transition
peaked near 150 ms, then settled to 42–58 ms; a single-ring live update measured
12–21 ms. These are render timings, not a guaranteed refresh rate. A temporary
input diagnostic delivered 50 Down and 50 Up events through the production
application reader without a panic, then restored the physical driver. Both
timing and input probes were removed from the shipped image. Physical touch and
perceived smoothness still require observation on the panel.
