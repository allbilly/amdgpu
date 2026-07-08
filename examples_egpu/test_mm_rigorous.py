#!/usr/bin/env python3
"""Rigorous VRAM persistence test: write A@x, B@y, read both back."""
import os, struct
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
  print(f"trained={vram_training_ok(b)} FB={b.rreg(0x809):#x}", flush=True)
  b.wreg(mmBIF_FB_EN, 0x3); b.wreg(mmBIF_MM_INDACCESS_CNTL, 0)
  A, B = 0xAAAA1111, 0xBBBB2222
  x, y = 0x1000, 0x8000
  mm_write(b, x, A); mm_write(b, y, B)
  b.hdp_flush(); b.hdp_invalidate()
  rx, ry = mm_read(b, x), mm_read(b, y)
  print(f"MM persistence: x={rx:#010x}(want {A:#x}) y={ry:#010x}(want {B:#x})", flush=True)
  print(f"  persistent={'YES' if rx==A and ry==B else 'NO'}", flush=True)
  # Also test doorbell/BAR2 and BAR0 raw at same offset with two patterns
  for pat in (0x11112222, 0x33334444):
    dev.vram[0x5000:0x5004] = struct.pack('<I', pat)
    b.hdp_flush(); b.hdp_invalidate()
    got = struct.unpack('<I', bytes(dev.vram[0x5000:0x5004]))[0]
    print(f"BAR0 0x5000 w={pat:#010x} r={got:#010x} {'OK' if got==pat else 'FAIL'}", flush=True)

if __name__ == "__main__":
  main()
