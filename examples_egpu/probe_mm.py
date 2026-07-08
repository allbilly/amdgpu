#!/usr/bin/env python3
"""Careful MM_INDEX VRAM probe with per-op drain + retries (no SMC, no LoadUcodes).

Goal: decide whether trained VRAM is CPU-reachable via MM_INDEX at all, or the
0xffffffff is a real dead aperture (not just USB4 MMIO queue flakiness).
"""
import os, struct, time
os.environ.setdefault("AMD_BOOT_VBIOS_FILE", "/tmp/rx570.rom")
from add import PolarisDevice
from polaris_boot import (PolarisBoot, mmBIF_FB_EN, mmBIF_MM_INDACCESS_CNTL,
  mmMM_INDEX, mmMM_INDEX_HI, mmMM_DATA, mmBIF_DOORBELL_APER_EN, mmBUS_CNTL)
from atom_replay import run_asic_init_if_needed, vram_training_ok

def drain(b): b.dev.pci.drain_mmio(bar=5, reg=0x2004)

def mm_write(b, pos, val):
  b.wreg(mmMM_INDEX, (pos & 0x7fffffff) | 0x80000000); drain(b)
  b.wreg(mmMM_INDEX_HI, pos >> 31); drain(b)
  b.wreg(mmMM_DATA, val); drain(b)

def mm_read(b, pos):
  b.wreg(mmMM_INDEX, (pos & 0x7fffffff) | 0x80000000); drain(b)
  b.wreg(mmMM_INDEX_HI, pos >> 31); drain(b)
  v = b.rreg(mmMM_DATA); drain(b)
  return v

def main():
  dev = PolarisDevice(); b = PolarisBoot(dev)
  b.vi_common_init(); b.enable_vbios_rom()
  run_asic_init_if_needed(b)
  print(f"trained={vram_training_ok(b)} FB={b.rreg(0x809):#x} "
        f"BUS_CNTL={b.rreg(mmBUS_CNTL):#x} MM_INDACC={b.rreg(mmBIF_MM_INDACCESS_CNTL):#x}", flush=True)
  b.wreg(mmBIF_FB_EN, 0x3); drain(b)
  b.wreg(mmBIF_MM_INDACCESS_CNTL, 0); drain(b)
  # Read the same location 5x to see if MM_DATA is stable (aperture) or noise (queue)
  pos = 0x2000
  print("stability read (no write):", [hex(mm_read(b, pos)) for _ in range(5)], flush=True)
  # Write then read 3x
  pat = 0xCAFE0002
  mm_write(b, pos, pat)
  reads = [hex(mm_read(b, pos)) for _ in range(3)]
  print(f"after write {pat:#x}: {reads}", flush=True)
  # Try several offsets, count OK
  ok = 0; tot = 0
  for off in (0x1000, 0x2000, 0x3000, 0x8000, 0x10000, 0x40000, 0x100000):
    tot += 1
    p = (0x1000_0000 | off) & 0xffffffff
    mm_write(b, off, p)
    got = mm_read(b, off)
    good = got == p
    ok += good
    print(f"  off={off:#08x} w={p:#010x} r={got:#010x} {'OK' if good else 'FAIL'}", flush=True)
  print(f"MM_INDEX summary: {ok}/{tot} OK", flush=True)

if __name__ == "__main__":
  main()
