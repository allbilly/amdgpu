# AMDGPU Debugger Notes

This repository now contains a small standalone implementation of the low-level
pieces from `thegeeko.md`:

- `amdgpu-poc` opens a DRM node, initializes `libdrm_amdgpu`, creates a command
  submission context, allocates GPU/CPU mapped BOs, uploads shader code, emits
  PM4 `SET_SH_REG` and `DISPATCH_DIRECT` packets, submits an indirect buffer,
  and waits for the compute fence.
- `amdgpu-regs2` is a cautious wrapper around `/sys/kernel/debug/dri/*/regs2`.
  It can set SRBM/GRBM banking with the `REGS2_IOC_SET_STATE_V2` ioctl and then
  read or write one 32-bit register at a byte offset.
- `tools/extract-text.sh` compiles GCN assembly with clang and extracts the raw
  `.text` bytes expected by `amdgpu-poc`.
- `shaders/trap-handler-gfx11.s` is the trap-handler assembly sketch from the
  article. It is not enabled automatically.

## Important Safety Notes

Direct PM4 submission and debugfs register writes can hang or reset the GPU.
The trap-handler path is especially risky because TBA/TMA are VMID-banked and
the VMID is assigned dynamically. The article's workaround writes all user VMIDs;
that can affect unrelated GPU processes.

Prefer testing on a spare machine, unload desktop GPU users, and understand your
kernel's `amdgpu.lockup_timeout` behavior before running trap experiments.

## Build

Install the development packages for libdrm and clang/LLVM. On Fedora this is
typically:

```sh
sudo dnf install libdrm-devel clang llvm
```

Then build:

```sh
make
```

## Basic Dispatch

Run the embedded no-op shader:

```sh
build/amdgpu-poc --card /dev/dri/renderD128
```

Compile and run the assembly no-op shader:

```sh
AMDGPU_MCPU=gfx900 make shaders
build/amdgpu-poc --shader build/nop.bin
```

Run a shader that writes `0x12345678` to the output BO:

```sh
AMDGPU_MCPU=gfx900 make shaders
build/amdgpu-poc --shader build/store-output.bin --pass-output-va
```

The first four output bytes should be `78 56 34 12`. `--pass-output-va` writes
the output BO GPU virtual address into `COMPUTE_USER_DATA_0/1`, which becomes
`s[0:1]` for shaders that request at least two user SGPRs. The tool also ORs
`COMPUTE_PGM_RSRC2.USER_SGPR=2` into the submitted `rsrc2` value.

Run a simple add shader through the same PM4 packet path:

```sh
AMDGPU_MCPU=gfx900 make shaders
build/amdgpu-poc --shader build/add-output.bin --pass-output-va --output-bytes 16
```

The first four output bytes should be `05 00 00 00`, proving the submitted
compute PM4 dispatched a shader that calculated `2 + 3` and stored the result.

The compute PM4 path explicitly sets `COMPUTE_START_X/Y/Z = 0` and dispatches
with `COMPUTE_SHADER_EN | FORCE_START_AT_000`, matching tinygrad's baseline
compute dispatch setup more closely.

To validate GPU-side copies, add `--sdma-readback`. This submits a second IB to
`AMDGPU_HW_IP_DMA`, copies the output BO to a readback BO with an SDMA linear-copy
packet, and dumps the copied bytes:

```sh
build/amdgpu-poc --shader build/add-output.bin --pass-output-va --output-bytes 16 --sdma-readback
```

Run a simple multiply shader through the same packet path:

```sh
AMDGPU_MCPU=gfx900 make shaders
build/amdgpu-poc --shader build/mul-output.bin --pass-output-va --output-bytes 16
```

The first four output bytes should be `2a 00 00 00`, proving the shader
calculated `6 * 7` and stored the result.

Other small compute PM4 tests:

| Shader | Useful command suffix | Expected result |
| --- | --- | --- |
| `sub-output.bin` | `--shader build/sub-output.bin --pass-output-va --output-bytes 16` | `9 - 4 = 5` |
| `and-or-xor-output.bin` | `--shader build/and-or-xor-output.bin --pass-output-va --output-bytes 16` | AND/OR/XOR dwords: `0x00f0`, `0xfff0`, `0xff00` |
| `shl-shr-output.bin` | `--shader build/shl-shr-output.bin --pass-output-va --output-bytes 16` | shift dwords: `0x100`, `0x80` |
| `load-add-store.bin` | `--shader build/load-add-store.bin --pass-output-va --init-output-u32 11 --init-output-u32 31 --output-bytes 16` | loads `11` and `31`, stores `42` |
| `multi-thread-addtid.bin` | `--shader build/multi-thread-addtid.bin --pass-output-va --threads-x 8 --output-bytes 64` | eight lanes write `0x11110000..0x11110007` |
| `branch-select.bin` | `--shader build/branch-select.bin --pass-output-va --output-bytes 16` | compare/select stores `0xaaaaaaaa` |
| `atomic-add.bin` | `--shader build/atomic-add.bin --pass-output-va --threads-x 8 --output-bytes 16` | eight lanes atomically increment to `8` |

Use `AMDGPU_MCPU=gfx1100` or another clang-supported target for other ASICs.

The default `COMPUTE_PGM_RSRC1` is a minimal common value (`0x00ac0000`). For
real shaders, pass the compiler-provided `rsrc1`, `rsrc2`, and `rsrc3` values.

## Register Access

Mount debugfs and find the correct DRM debugfs directory:

```sh
sudo mount -t debugfs none /sys/kernel/debug
ls /sys/kernel/debug/dri
```

Read a register by byte offset:

```sh
sudo build/amdgpu-regs2 --regs2 /sys/kernel/debug/dri/0/regs2 --offset 0x1234 read
```

Write a VMID-banked SRBM register:

```sh
sudo build/amdgpu-regs2 --regs2 /sys/kernel/debug/dri/0/regs2 --srbm-vmid 1 \
  --offset 0x1234 write --value 0xdeadbeef
```

Use UMR or kernel register headers to resolve ASIC-specific offsets for
`SQ_SHADER_TBA_LO/HI`, `SQ_SHADER_TMA_LO/HI`, and `SQ_CMD`.
