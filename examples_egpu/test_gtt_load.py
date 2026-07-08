#!/usr/bin/env python3
"""Attempt LoadUcodes with all firmware in system memory (GTT via GART).

Now that ATOM training completes, test whether the SMC can DMA-read the TOC and
firmware from GART-mapped host sysmem (avoids the dead VRAM data path entirely).
Minimal mask (RLC only) to reduce risk; short timeout.
"""
import os, time
os.environ.setdefault("AMD_BOOT_VBIOS_FILE", "/tmp/rx570.rom")
os.environ.setdefault("AMD_BOOT_FW_LAYOUT", "gtt")
os.environ.setdefault("AMD_BOOT_GART_SYSMEM", "1")
os.environ.setdefault("AMD_BOOT_UCODE_LOAD_TIMEOUT_S", "15")
os.environ.setdefault("AMD_BOOT_SMC_MSG_TIMEOUT_S", "15")
os.environ.setdefault("DEBUG", "1")

from add import PolarisDevice
from polaris_boot import PolarisBoot
from atom_replay import run_asic_init_if_needed, vram_training_ok

def main():
  dev = PolarisDevice(); b = PolarisBoot(dev)
  b.vi_common_init(); b.enable_vbios_rom()
  run_asic_init_if_needed(b)
  print(f"trained={vram_training_ok(b)}", flush=True)
  b.gmc_sw_init(); b.start_smc(); b.process_smc_firmware_header()
  b.mc_program()
  b.gart_enable()
  try:
    b.load_ip_firmware()
    print("LoadUcodes OK", flush=True)
  except Exception as e:
    print(f"LoadUcodes FAIL: {e}", flush=True)

if __name__ == "__main__":
  main()
