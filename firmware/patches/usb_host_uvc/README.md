# usb_host_uvc 2.6.0 patch

`usb_host_uvc-2.6.0-hdmilight.patch` is applied to `espressif/usb_host_uvc` 2.6.0 at configure time by
[`firmware/cmake/patched_component.cmake`](../../cmake/patched_component.cmake).

## Why

The MS2130 capture chip streams MJPEG over a **bulk** endpoint, and Espressif's UVC host driver cannot find the
payload boundaries in that stream.

A UVC payload starts with a 12-byte header and the driver has to know where each one begins. On a bulk endpoint
there is no per-payload framing, so `uvc_bulk.c` infers a boundary from a **short packet** — a transfer that came
back with fewer bytes than the buffer holds. That works for cameras whose payloads are not a whole number of
max-packet-size packets.

It does not work here. On a USB 2.0 link the MS2130 negotiates a payload of 15,360 bytes on endpoint 0x83 with
512-byte packets: 15360 = 30 × 512 exactly, so a mid-frame payload never ends with a short packet. The driver
therefore keeps treating the next payload's 12-byte header as picture data and leaves a header every 15,360 bytes
inside the JPEG. The hardware JPEG decoder then rejects the frame, or decodes garbage.

## What it changes

Three files, one idea: make "this transfer received a whole payload" a payload boundary as well.

1. `uvc_host.c` — remember `dwMaxPayloadTransferSize` from the stream-format negotiation, and size every bulk URB
   to exactly one payload. One transfer then holds one payload, which is what makes the test below exact. A warning
   is logged if the caller asked for a different URB size; the firmware passes `urb_size = 0` and lets the driver
   decide (see `firmware/main/capture.c`).
2. `uvc_types_priv.h` — the field that carries it.
3. `uvc_bulk.c` — a payload ends on a short packet (the original rule) **or** when the transfer received
   `dwMaxPayloadTransferSize` bytes. The next transfer then starts with a payload header.

Cameras that do send short packets are unaffected: the original test still fires first, and
`dwMaxPayloadTransferSize` is only consulted for bulk streams where the negotiation reported one.

## Moving to a newer release

To retarget it, change the version and archive hash in `firmware/CMakeLists.txt` and
`firmware/main/idf_component.yml`, re-make the patch against the new release, and rename it to match. If upstream
ever learns payload sizes on its own, this patch can go away.
