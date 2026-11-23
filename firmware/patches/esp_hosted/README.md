# esp_hosted 3.1.0 patch

`esp_hosted-3.1.0-hdmilight.patch` is applied to `espressif/esp_hosted` 3.1.0 at configure time by
[`firmware/cmake/patched_component.cmake`](../../cmake/patched_component.cmake).

## Why

Only the ESP32-P4 builds need this. The P4 has no radio of its own: Wi-Fi comes from an ESP32-C6 companion chip
over SDIO, driven by the esp_hosted host driver. (The oneboard's ESP32-S31 has its own radio and never uses
esp_hosted.) On the P4 dev kits the C6 runs the Wi-Fi coprocessor firmware it was shipped with, which is older than
`esp-hosted-mcu` 2.11.7 and therefore never sends the `SDIO_MODE` TLV during transport init.

esp_hosted 3.1.0 added a fixed-size "staging" buffer for SDIO RX. When the coprocessor does not announce its mode,
3.1.0 assumes packet mode and sets the staging target to a single frame (1536 bytes). But those older builds stream
by default: the host reads everything the coprocessor has queued in one CMD53, so the first aggregate holding two
frames is larger than the target.

With the target set, 3.1.0 refuses an oversized read by returning `NULL` instead of growing the slot. The read task
then skips the read without consuming it, the coprocessor offers the same aggregate on the next interrupt, and it is
refused again — forever. Wi-Fi receive is dead from the first merged read until a power cycle. Transmit keeps
working, which makes it look like a network problem rather than a driver one.

## What it changes

Two hunks, both in the host SDIO transport:

1. `eh_host_mcu_transport_init_event.c` — if the coprocessor sent no `SDIO_MODE` TLV and software aggregation was
   not negotiated, leave the staging buffers dynamic (target 0) instead of guessing one frame. This is the same
   path 3.1.0 already takes when the queue size is unknown, and what 3.0.10 did for every coprocessor.
2. `eh_host_bus_sdio.c` — if an oversized read does arrive anyway, grow the slot and warn once per new high-water
   mark, rather than returning `NULL` and wedging the receive path. Safe in streaming mode: the slot being resized
   is the read task's own unpublished one, and the drain task copies each frame out rather than holding a pointer
   into it.

Hunk 1 is the fix; hunk 2 makes the failure mode recoverable rather than fatal if some other coprocessor
negotiates differently.

## Moving to a newer release

Upstream may make the staging target cope with streaming coprocessors on its own, in which case this patch can go
away. To retarget it, change the version and archive hash in `firmware/CMakeLists.txt` and
`firmware/main/idf_component.yml`, re-make the patch against the new release, and rename it to match.
