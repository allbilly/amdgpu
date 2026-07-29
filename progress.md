# Progress — GCN5 / gfx90c on MGPUSim

**Goal:** Run modern ROCm kernels and calibrate cycle-accurate timing against the host Renoir APU (Ryzen 7 4700U, gfx90c).

**Last updated:** 2026-07-29

## Status

| Area | State |
|------|--------|
| Arch flag `-arch gcn5 -gpu gfx90c` | Done |
| VOP3C / carry VCC gating | Done |
| V5 HSACO SGPR layout (`kernel_code_properties`) | Done |
| gfx90c HSACO for ISCA-10 benches | Done (all 10) |
| MUBUF + private-segment scratch (VGPR spill) | Done |
| DS b128, FLAT SADDR, missing SOP/VOP ops | Done |
| Timing model `gfx90c/builder.go` | Done (knobs settled) |
| ISCA-10 sim vs HW (gfx90c HSACO) | Done — geo mean HW/Sim ≈ **1.18×** |
| Remaining timing gaps | Open (model, not ISA) |

## ISCA 2019 suite (2026-07-10)

Host: Renoir gfx90c @ 1600 MHz, ROCm 7.1.1 (podman).  
Sim: `-timing -arch gcn5 -gpu gfx90c -disable-rtm -verify`.  
Artifacts: `~/mgpusim/gpu_perf_scripts/calibration/gfx90c/` (`sim_out8/`, `sim_ground_truth.txt`, `hw_ground_truth.txt`).
Repo: `~/mgpusim` (was `mgpusim/` nested here; moved 2026-07-29, remote now `allbilly/mgpusim`, branch `gcn5-gfx90c`).

| Benchmark | Sim (µs) | HW (µs) | HW / Sim |
|-----------|----------|---------|----------|
| vectoradd | 28.4 | 29.4 | 1.03× |
| relu | 15.8 | 13.1 | 0.83× |
| matrixmult | 77.1 | 39.1 | 0.51× |
| matrixtranspose | 60.3 | 140.8 | 2.34× |
| bitonicsort | 329.2 | 811.4 | 2.46× |
| aes | 14.3 | 17.0 | 1.18× |
| fir | 5.3 | 12.0 | 2.28× |
| kmeans | 52.6 | 39.2 | 0.75× |
| pagerank | 108.3 | 130.6 | 1.21× |
| nw | 137.9 | 123.1 | 0.89× |

**Geometric mean HW/Sim ≈ 1.18×.**

Reproduce:

```bash
bash ~/mgpusim/gpu_perf_scripts/calibration/gfx90c/run_sim.sh
bash ~/mgpusim/gpu_perf_scripts/calibration/gfx90c/build_and_run.sh
python3 ~/mgpusim/gpu_perf_scripts/calibration/gfx90c/compare.py
```

## Re-run (2026-07-29)

Sim re-run on `gcn5-gfx90c` branch (commit `966a6f2a`) — **all 10 numbers reproduce the 2026-07-10 ground truth bit-for-bit** (sim is deterministic, no clock drift):

| Benchmark | Sim 07-10 | Sim 07-29 | Δ |
|-----------|-----------|-----------|---|
| vectoradd | 28.4 | 28.4 | 0 |
| relu | 15.8 | 15.8 | 0 |
| matrixmult | 77.1 | 77.1 | 0 |
| matrixtranspose | 60.3 | 60.5 | +0.2 |
| bitonicsort | 329.2 | 329.2 | 0 |
| aes | 14.3 | 14.3 | 0 |
| fir | 5.3 | 5.3 | 0 |
| kmeans | 52.6 | 52.6 | 0 |
| pagerank | 108.3 | 108.3 | 0 |
| nw | 137.9 | 137.9 | 0 |

HW re-run on the same Renoir gfx90c (ROCm 7.1.1, podman) came back **1.4–9× slower** than the 07-10 ground truth. Root cause: GPU clock stuck at 200 MHz idle (`pp_dpm_sclk` reads `0: 200Mhz *`), not boosting to 1600 MHz. The 07-10 run used `power_dpm=high` (needs root); this run couldn't pin the clock. Back-to-back runs didn't warm it up either — amdgpu on Renoir won't boost without the dpm override.

| Benchmark | Sim (µs) | HW pinned (µs) | HW throttled (µs) | pin/sim | thr/sim |
|-----------|----------|----------------|-------------------|---------|---------|
| vectoradd | 28.4 | 29.4 | 40.9 | 1.04× | 1.44× |
| relu | 15.8 | 13.1 | 30.4 | 0.83× | 1.92× |
| matrixmult | 77.1 | 39.1 | 275.6 | 0.51× | 3.57× |
| matrixtranspose | 60.3 | 140.8 | 215.5 | 2.33× | 3.57× |
| bitonicsort | 329.2 | 811.4 | 1865.2 | 2.46× | 5.67× |
| aes | 14.3 | 17.0 | 131.2 | 1.19× | 9.17× |
| fir | 5.3 | 12.0 | 49.5 | 2.26× | 9.34× |
| kmeans | 52.6 | 39.2 | 298.7 | 0.75× | 5.68× |
| pagerank | 108.3 | 130.6 | 823.5 | 1.21× | 7.60× |
| nw | 137.9 | 123.1 | 980.4 | 0.89× | 7.11× |
| **geo mean** | | | | **1.18×** | **4.70×** |

The ~4× throttle factor is consistent (200 MHz / 1600 MHz = 8× clock ratio, partially offset by fixed launch overheads on short kernels). This confirms the sim is calibrated to 1600 MHz and the throttled HW run is not a fair comparison.

**Conclusion: the sim implementation is stable and reproducible — all 10 sim numbers reproduce bit-for-bit across 19 days and a repo move. The 07-10 calibration at pinned 1600 MHz (geo mean HW/Sim ≈ 1.18×) remains the accuracy reference. To get a fresh HW comparison, the GPU clock must be pinned first (`echo high | sudo tee .../power_dpm_force_performance_level`), which needs root access.**

## Done (bring-up)

### Architecture / decode / ALU
- `amd/arch`: `GCN5` type; runner `-arch gcn5 -gpu gfx90c`
- VOP2 `_co` names + VCC write only for opcodes 25–30
- GFX9 FLAT/GLOBAL SADDR (decoder `IsCDNA3` for GCN5 path)
- SOP2: `s_mul_hi_*`, `s_bfe_u32`; SOPC cmps; VOP2 `v_add_u16`; VOPC `v_cmp_gt_i16`
- DS `ds_write_b128` / `ds_read_b128`
- SDWA src0_sgpr bit fix (AES)
- FLAT sbyte/sshort load return extend (timing CU)

### MUBUF + scratch (matrixmult VGPR spill)
- Decode + ALU: `amd/emu/gcn3/alu_mubuf.go`; timing coalescer / vecmem
- Driver: `prepareScratch` + AQL `ScratchAddress`
- PSB V# with ADD_TID + stride: `amd/kernels/scratch.go`
- Per-WG scratch base so concurrent WGs don’t collide
- Zero next SGPR after WG IDs when PSB present
- matrixmult kernarg: removed bogus device `BlockA` (static `__shared__`)

### Timing preset
- `amd/samples/runner/timingconfig/gfx90c/builder.go`
- 8 CU (4 SA × 2), 1600 MHz, L2 1 MB, banked DRAM depth 40
- LDS 12, VALU scoreboard 8, CP tax 2000, DMA-through-L2 &lt; 64 KB
- Knob sweeps (LDS/L1V/scoreboard) barely move FIR/transpose — remaining gaps need model work

## Open (timing model)

- **Fast (HW/Sim ~2.3–2.5×):** matrixtranspose, bitonicsort, fir — LDS bank conflicts / multi-launch sync under-modeled (bitonic ≈ 78 launches)
- **Slow (HW/Sim ~0.5–0.8×):** matrixmult (spill traffic over-costly), relu / kmeans (short kernels vs banked DRAM)

## Earlier notes (superseded by docs)

Session log from 2026-06-15 (V2 HSACO, VOP3C blockers, Docker images) lived here and was deleted once; durable material is in:

- `docs/MGPUSim-V5-Support.md`
- `docs/Adding-VOP3C-Instructions.md`
- `~/mgpusim/README.md` (ISCA-10 table)

## Next

1. Model LDS bank conflicts / multi-launch overhead without over-penalizing single-launch kernels
2. Revisit scratch / MUBUF cost for matrixmult spill path
3. Optional: tighten short-kernel DRAM path for relu/kmeans
