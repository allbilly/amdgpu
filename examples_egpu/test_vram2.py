#!/usr/bin/env python3
"""Test VRAM access using the real 40-bit FB base VBIOS programmed."""
import os, struct, time
os.environ.setdefault("AMD_BOOT_VBIOS_FILE", "/tmp/rx570.rom")
from add import PolarisDevice
from polaris_boot import (PolarisBoot, mmBIF_FB_EN, mmBIF_MM_INDACCESS_CNTL,
  mmMM_INDEX, mmMM_INDEX_HI, mmMM_DATA)
from atom_replay import run_asic_init_if_needed, vram_training_ok

def mm_write(b, pos, val):
  b.wreg(mmMM_INDEX, (pos & 0x7fffffff) | 0x80000000)
  b.wreg(mmMM_INDEX_HI, pos >> 31)
  b.wreg(mmMM_DATA, val)
def mm_read(b, pos):
  b.wreg(mmMM_INDEX, (pos & 0x7fffffff) | 0x80000000)
  b.wreg(mmMM_INDEX_HI, pos >> 31)
  return b.rreg(mmMM_DATA)

def main():
  dev = PolarisDevice(); b = PolarisBoot(dev)
  b.vi_common_init(); b.enable_vbios_rom()
  run_asic_init_if_needed(b)
  fb = b.rreg(0x809)
  base = (fb & 0xffff) << 24  # 40-bit MC base (bits set by VBIOS)
  print(f"trained={vram_training_ok(b)} FB_LOC={fb:#x} mc_base={base:#x}", flush=True)
  b.wreg(mmBIF_FB_EN, 0x3)
  b.wreg(mmBIF_MM_INDACCESS_CNTL, 0)
  print("\n=== MM_INDEX at true MC base (pos = base + off) ===", flush=True)
  for off in (0x1000, 0x2000, 0x4000, 0x100000):
    pos = base + off
    pat = (0x5A000000 | off) & 0xffffffff
    mm_write(b, pos, pat); b.hdp_flush()
    b.hdp_invalidate()
    got = mm_read(b, pos)
    print(f"  pos={pos:#x} wrote={pat:#010x} read={got:#010x} {'OK' if got==pat else 'FAIL'}", flush=True)
  print("\n=== MM_INDEX at low pos (0-based) with hdp inval ===", flush=True)
  for off in (0x1000, 0x2000, 0x4000):
    pat = (0x7C000000 | off) & 0xffffffff
    mm_write(b, off, pat); b.hdp_flush(); b.hdp_invalidate()
    got = mm_read(b, off)
    print(f"  pos={off:#x} wrote={pat:#010x} read={got:#010x} {'OK' if got==pat else 'FAIL'}", flush=True)

if __name__ == "__main__":
  main()
