#!/usr/bin/env python3
"""Reprogram FB_LOCATION to a 0-based MC layout, then test BAR0 + MM_INDEX."""
import os, struct, time
os.environ.setdefault("AMD_BOOT_VBIOS_FILE", "/tmp/rx570.rom")
from add import PolarisDevice
from polaris_boot import (PolarisBoot, mmBIF_FB_EN, mmBIF_MM_INDACCESS_CNTL,
  mmMM_INDEX, mmMM_INDEX_HI, mmMM_DATA, mmMC_VM_FB_LOCATION,
  mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
  mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR)
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
  print(f"trained={vram_training_ok(b)} FB_before={b.rreg(0x809):#x}", flush=True)
  # Program a 0-based 4GB FB: base=0, top=0x00ff (top 24-bit >>24)
  size_mb = 4096
  vram_end = size_mb*1024*1024 - 1
  fb = ((vram_end >> 24) & 0xffff) << 16 | 0
  b.wreg(mmMC_VM_FB_LOCATION, fb)
  b.wreg(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, 0)
  b.wreg(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, vram_end >> 12)
  b.wreg(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, 0)
  b.wreg(mmBIF_FB_EN, 0x3)
  b.wreg(mmBIF_MM_INDACCESS_CNTL, 0)
  b.mmio_sync_safe()
  print(f"FB_after={b.rreg(0x809):#x} SYS_HI={b.rreg(0x80e):#x}", flush=True)

  print("\n=== BAR0 aperture ===", flush=True)
  for off in (0x1000, 0x2000, 0x10000):
    pat=(0xA5000000|off)&0xffffffff
    try:
      dev.vram[off:off+4]=struct.pack('<I',pat); b.hdp_flush(); b.hdp_invalidate()
      got=struct.unpack('<I',bytes(dev.vram[off:off+4]))[0]
      print(f"  off={off:#08x} w={pat:#010x} r={got:#010x} {'OK' if got==pat else 'FAIL'}", flush=True)
    except Exception as e:
      print(f"  off={off:#08x} EXC {e}", flush=True)

  print("\n=== MM_INDEX 0-based ===", flush=True)
  for off in (0x1000, 0x2000, 0x4000, 0x100000):
    pat=(0x5A000000|off)&0xffffffff
    mm_write(b, off, pat); b.hdp_flush(); b.hdp_invalidate()
    got=mm_read(b, off)
    print(f"  pos={off:#08x} w={pat:#010x} r={got:#010x} {'OK' if got==pat else 'FAIL'}", flush=True)

if __name__ == "__main__":
  main()
