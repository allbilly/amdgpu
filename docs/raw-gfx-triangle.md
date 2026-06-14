# Raw GFX Triangle Plan

`build/radv-triangle` proves the normal Vulkan/RADV graphics path works on this
machine. The next step is to capture the PM4 that RADV emits and replay a
minimal equivalent through `libdrm_amdgpu` with `AMDGPU_HW_IP_GFX`.

## Current Status

`build/amdgpu-raw-triangle` submits a raw GFX PM4 `DRAW_INDEX_AUTO` command to
`AMDGPU_HW_IP_GFX`. It programs enough state to launch the vertex shader on
Renoir/GFX9. The vertex shader writes `0x76543210` to a GTT output BO, proving
that the graphics ring, VS program setup, primitive setup, and draw packet are
executing.

Run:

```sh
AMDGPU_MCPU=gfx90c make build/gfx-vs.bin build/gfx-ps-store.bin build/gfx-ps-store-only.bin build/amdgpu-raw-triangle
build/amdgpu-raw-triangle
```

Expected current output:

```text
submitting raw GFX triangle draw: ... PM4 dwords
output[0] = 0x76543210
```

The raw pixel shader launch path also works when isolated from color-buffer
state:

```sh
build/amdgpu-raw-triangle --ps-store-only
```

Expected output:

```text
submitting raw GFX triangle draw: ... PM4 dwords
output[0] = 0xfeed1234
```

This mode enables PS execution with `DB_SHADER_CONTROL.EXEC_ON_NOOP`, RADV-like
PS input state (`SPI_PS_INPUT_ENA/ADDR = 0x80`), and `SPI_SHADER_PGM_RSRC2_PS =
0x0c`. The PS writes to the GTT output BO and then emits a masked MRT0 export.
That proves PS waves are launching on Renoir without depending on a valid color
target.

The color-buffer path now renders the triangle into a linear VRAM color target.
The missing coverage state was `PA_CL_VTE_CNTL`; without it, clip-space
coordinates rasterized around `(0,0)` and only one pixel was written. The current
`--color-target` result writes the PS memory-store value and produces a red
triangle in the color BO:

```text
output[0] = 0xfeed1234
direct[128,128] = 0xff0000ff first_nonzero = 0xff0000ff nonzero = 18432 bbox = 32,33..223,223
```

That proves raw VS, PS, rasterization, CB export, and direct color-target
readback all work on Renoir for this linear target path.

There is an opt-in experiment:

```sh
build/amdgpu-raw-triangle --color-target
```

This path is still opt-in while the raw PM4 is being cleaned up. It programs a
linear color target using RADV-derived `CB_COLOR0_*` values, uses a 64 KiB-aligned
VRAM color BO, enables `PA_CL_VTE_CNTL`, and uses `SPI_SHADER_COL_FORMAT =
SPI_SHADER_32_ABGR` because the hand-written PS exports FP32 values. RADV uses
`SPI_SHADER_FP16_ABGR` because its compiled shader exports packed FP16 color
values.

## Capture Notes

`RADV_DEBUG=dumpibs` works without root and is the preferred first reference:

```sh
RADV_DEBUG=dumpibs build/radv-triangle > /tmp/radv-triangle-dumpibs.txt 2>&1
```

The dump showed two draws:

- a RADV internal `DI_PT_RECTLIST` draw for the render-pass clear
- the actual triangle draw with `VGT_PRIMITIVE_TYPE = DI_PT_TRILIST` and
  `DRAW_INDEX_AUTO` count `3`

Key draw packets from the triangle section:

```text
SET_UCONFIG_REG_INDEX VGT_PRIMITIVE_TYPE <- DI_PT_TRILIST
SET_UCONFIG_REG_INDEX IA_MULTI_VGT_PARAM <- 0x0070007f
SET_UCONFIG_REG       VGT_MULTI_PRIM_IB_RESET_EN <- 0
NUM_INSTANCES         <- 1
DRAW_INDEX_AUTO       VGT_NUM_INDICES <- 3, VGT_DRAW_INITIATOR <- 2
```

Key state already ported into `amdgpu-raw-triangle` includes:

- `VGT_SHADER_STAGES_EN = 0x00010000`
- `VGT_GS_MODE = 0`
- `VGT_GS_OUT_PRIM_TYPE = TRISTRIP`
- viewport/scissor state for 256x256
- `PA_CL_VTE_CNTL` viewport transform state
- VS/PS program register setup
- PS memory-store launch without a valid color target (`--ps-store-only`)
- color-target triangle render with a visible `0xff0000ff` CB write
- `VGT_PRIMITIVE_TYPE = DI_PT_TRILIST`
- `DRAW_INDEX_AUTO`

Follow-up cleanup for color output:

- remove or gate temporary VS debug marker/position stores
- optionally write the color BO to a PPM for visual inspection
- investigate why the attempted `DMA_DATA` readback copy returned zeros while
  direct mapping of the VRAM render target works

`build/radv-triangle` can force a Vulkan linear-tiled render target for PM4
comparison:

```sh
RADV_TRIANGLE_LINEAR=1 RADV_DEBUG=dumpibs build/radv-triangle > /tmp/radv-triangle-linear-dumpibs.txt 2>&1
```

For Renoir, the linear image dump changed the key register:

```text
CB_COLOR0_ATTRIB <- 0xd0000000
COLOR_SW_MODE = 0
```

The optimal image used:

```text
CB_COLOR0_ATTRIB <- 0xdc680000
COLOR_SW_MODE = 26
```

The raw experiment uses the linear value plus 64 KiB color BO alignment. The
critical non-CB state needed for full-screen coordinates was `PA_CL_VTE_CNTL`.

## Root UMR Path

UMR can also decode live rings, but it needs root/debugfs access:

- `umr` is not installed.
- `/sys/kernel/debug` requires root.
- passwordless `sudo` is not available in this session.

Install UMR and run the capture commands from an interactive shell with sudo.

## Capture Reference PM4

Run the known-good RADV triangle:

```sh
build/radv-triangle
```

In another terminal, or immediately after while the ring still contains recent
traffic, decode the GFX ring:

```sh
sudo umr --ring-stream gfx[.]
```

Search the output for packets near the draw:

```text
PKT3_SET_CONTEXT_REG
PKT3_SET_SH_REG
PKT3_SET_UCONFIG_REG
PKT3_DRAW_INDEX_AUTO
```

The most important state to extract is:

- color buffer registers for `CB_COLOR0_*`
- `CB_TARGET_MASK` and `CB_SHADER_MASK`
- viewport and scissor registers
- rasterizer and depth/stencil disabled state
- VS and PS `SPI_SHADER_PGM_*` and `SPI_SHADER_PGM_RSRC*`
- PS input/export state
- vertex buffer descriptor/user data state
- `VGT_PRIMITIVE_TYPE`
- `NUM_INSTANCES`
- `DRAW_INDEX_AUTO`

## PM4 Replay Helper

`build/amdgpu-gfx-pm4` loads a text file with one dword per line and can submit
it to the GFX ring:

```sh
build/amdgpu-gfx-pm4 --pm4 packets.txt
build/amdgpu-gfx-pm4 --pm4 packets.txt --submit
```

This is intentionally low-level. A captured RADV stream cannot be blindly
replayed after the RADV process exits because shader, vertex, and render target
GPU virtual addresses point at RADV-owned BOs. The useful workflow is to use the
decoded packet stream as a register/value reference, then rebuild the same state
with BO addresses allocated by our own process.
