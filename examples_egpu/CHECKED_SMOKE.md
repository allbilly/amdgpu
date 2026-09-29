# Supervised Polaris10 compute experiment

`checked_smoke.py` performs **one four-element addition**, with temperature
sampling and checked compute-queue deactivation. It is not a thermal stress
test, a VRAM test, or a production AMD driver. PCIe kernel panics remain possible.

## Verified on 2026-09-28

An M1 MacBook Air running macOS 27.0, TinyGPU 1.0.0 build 3, and a user-confirmed
16 GB RX 580 (`1002:6fdf`, revision `ef`) connected through a Thunderbolt 3 adapter
completed `[1,2,3,4] + [10,20,30,40] = [11,22,33,44]`.
The ring reported `drained=True`; both the example's KCQ/KIQ became inactive,
and MEC halt readback passed. Temperature samples during startup/the short run
were 45–47°C. Fan tachometer telemetry was unavailable; the operator confirmed
attached cooling and spinning fans. This does not establish behavior under a
sustained load, usable GDDR capacity, or complete automatic device shutdown.

## Fixes in add.py

- Keep the fallback framebuffer and visible-memory addresses above 4 GiB;
  reject invalid sizes and recompute AGP/GART after programming the framebuffer.
  The historical 4096 MB default remains; pass the board's verified capacity
  explicitly for a larger card. A valid existing hardware aperture is preserved.
- Read VRAM table v2.2/module v8 using its actual fixed-header and module sizes.
  Unknown revisions return no diagnostic data instead of guessed offsets.
- Refuse an unrecognized device before BAR mapping unless explicitly opted in.
  An opt-in is not a compatibility assertion. The runner additionally requires
  exact live/ROM PCI identity and a single matching memory module.
- Make HQD dequeue timeout an error. Do not clear a live queue's pointers.
  Add a compute-only quiesce helper that checks queue inactivity and MEC halt.

Only `add.py` is changed. Other standalone examples contain vendored copies and
do not inherit these fixes. The ordinary `add.py` CLI still has its historical
lifecycle; use the supervised runner for this experiment.

## Offline validation

```sh
python3 -m unittest discover -s examples_egpu -p test_add_safety.py -v
python3 examples_egpu/add.py --selftest
```

The regression tests use synthetic ROM tables and mocked registers; they never
connect to hardware. They cover 4/8/16 GiB aperture placement, stale ranges,
invalid configuration, a queue that never deactivates, halt-readback failure,
ROM bounds/revisions, and device refusal before BAR mapping.

## Inputs and execution

Use the card's own previously captured ROM, not an image from another board.
The ROM must have a valid ATOM header/checksum, matching PCI ID, and a single
v2.2/module-v8 entry matching `--vram-mb`. These checks are not an authenticity
guarantee. The runner executes the ROM's startup instructions but does not flash
the ROM or set overclock/undervolt values.

Supply these official linux-firmware files in a local directory:
`polaris10_{smc,smc_sk,mc,rlc,ce,pfp,me,mec,sdma,sdma1}.bin`.
Provide `manifest.json` mapping each filename to an object with its SHA-256
under `sha256`. Obtain them from
[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/main/amdgpu),
and review its license. No firmware binaries or board ROM are redistributed here.
The local manifest detects changes after download; it is not a vendor signature.

Without `--execute`, this command only validates local files:

```sh
python3 examples_egpu/checked_smoke.py \
  --rom /path/to/own-card.rom --firmware-dir /path/to/firmware \
  --device-id 0x6fdf --vram-mb 16384 --output /path/to/result.json
```

For an attended hardware attempt, add `--execute --cooling-confirmed` only with
the heatsink attached, cooling verified, and power-off recovery available.
Startup/dispatch is limited to 45 seconds between transport operations, socket
operations have a 2-second timeout, and temperature is sampled about every
0.5 seconds at transport boundaries with a conservative 70°C stop threshold.
These checks cannot interrupt a kernel panic, guarantee that every register
operation completes, or substitute for hardware thermal protection.

## Shutdown and physical recovery

The runner attempts to deactivate the two example queues, disables write-pointer
polling and halts MEC. This does **not** prove that SMC, SDMA, RLC, or every other
DMA client has stopped. It therefore holds its TinyGPU connection and host-memory
mappings after success or failure, and prints `POWER_OFF_REQUIRED`.

At that point, switch off the eGPU and disconnect it. Do not kill the runner to
skip this step. TinyGPU normally exits when the device is removed, allowing the
runner to finish; server exit alone is not proof of physical power-off. Confirm
the card is powered off and power cycle it before reuse. This manual recovery is
intentional until complete automatic shutdown is implemented and validated.

Register references: Linux
[atombios.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/atombios.h),
[gfx_v8_0.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c),
and [smu7_thermal.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/powerplay/hwmgr/smu7_thermal.c).
