#!/usr/bin/env python3
"""Diagnose the BAR0/VRAM aperture: is it dead, or just mis-based, after training?"""
import os, struct
os.environ.setdefault("AMD_BOOT_VBIOS_FILE", "/tmp/rx570.rom")
from add import PolarisDevice
from polaris_boot import (PolarisBoot, mmBIF_FB_EN, mmBUS_CNTL, mmBIF_MM_INDACCESS_CNTL,
  mmMC_VM_FB_LOCATION, mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR)
from atom_replay import run_asic_init_if_needed, vram_training_ok

def rd(dev, off): return struct.unpack('<I', bytes(dev.vram[off:off+4]))[0]

def main():
  dev = PolarisDevice(); b = PolarisBoot(dev)
  # raw BAR0 read BEFORE any programming
  print("BAR0 raw pre-init:", [hex(rd(dev, o)) for o in (0, 0x1000, 0x40000, 0x1000000)], flush=True)
  b.vi_common_init(); b.enable_vbios_rom()
  run_asic_init_if_needed(b)
  print(f"trained={vram_training_ok(b)}", flush=True)
  print("BAR0 raw post-train:", [hex(rd(dev, o)) for o in (0, 0x1000, 0x40000, 0x1000000)], flush=True)
  # dump MC routing regs
  for name, reg in [("FB_LOCATION",0x809),("FB_OFFSET",0x81a),("SYS_LOW",0x80d),
                    ("SYS_HIGH",0x80e),("SYS_DEFAULT",0x80f),("BIF_FB_EN",0x1524),
                    ("BUS_CNTL",0x1508),("MX_L1_TLB",0x518),("MC_ARB_RAMCFG",0x9d0),
                    ("MC_VM_MB_L1_TLB0",0x51a),("HDP_NONSURF_BASE",0xb01)]:
    print(f"  {name}({reg:#x}) = {b.rreg(reg):#x}", flush=True)
  # Try enabling FB and reading via BAR0 at the VBIOS FB base offset 0
  b.wreg(mmBIF_FB_EN, 0x3); b.mmio_sync_safe()
  print("BAR0 raw after BIF_FB_EN=3:", [hex(rd(dev, o)) for o in (0, 0x1000)], flush=True)
  # write via BAR0 and read via BAR0 with a settle
  import time
  dev.vram[0x1000:0x1004] = struct.pack('<I', 0x12345678)
  b.hdp_flush(); time.sleep(0.05); b.hdp_invalidate()
  print("BAR0 wr/rd 0x1000:", hex(rd(dev, 0x1000)), flush=True)

if __name__ == "__main__":
  main()
