#!/usr/bin/env python3
"""Measure max backward-jump iterations any single target legitimately needs."""
import os
os.environ.setdefault("AMD_BOOT_VBIOS_FILE", "/tmp/rx570.rom")
os.environ.setdefault("AMD_ATOM_QUIET", "1")
os.environ["AMD_ATOM_JUMP_MAX"] = "500000"
os.environ["AMD_ATOM_JUMP_TIMEOUT_SEC"] = "60"

from add import PolarisDevice
from polaris_boot import PolarisBoot
from atom_replay import (read_vbios_rom, parse_atom_context, AtomCard, AtomExecutor,
  clear_asic_init_scratch, _u16, _u32, ATOM_DATA_FWI_PTR, ATOM_CMD_INIT,
  ATOM_FWI_DEFSCLK_PTR, ATOM_FWI_DEFMCLK_PTR)

def main():
  dev = PolarisDevice(); b = PolarisBoot(dev)
  b.vi_common_init(); b.enable_vbios_rom()
  bios = read_vbios_rom(b); clear_asic_init_scratch(b)
  ctx = parse_atom_context(bios); card = AtomCard(b, debug=False)
  exe = AtomExecutor(ctx, card)
  hwi = _u16(bios, ctx.data_table + ATOM_DATA_FWI_PTR)
  ps=[0]*16; ps[0]=_u32(bios,hwi+ATOM_FWI_DEFSCLK_PTR); ps[1]=_u32(bios,hwi+ATOM_FWI_DEFMCLK_PTR)
  try:
    exe.execute_table(ATOM_CMD_INIT, ps, 16)
    print(f"DONE MEMSIZE={b.rreg(0x150a)&0xffff} MISC0={b.rreg(0xa80):#x}", flush=True)
  except Exception as e:
    print(f"STOP: {e}", flush=True)
  jc = card._jump_counts
  tops = sorted(jc.items(), key=lambda kv: -kv[1])[:10]
  print("top backward-jump targets by max consecutive iters:", flush=True)
  for t, n in tops:
    print(f"  target={t:#06x} max_iters={n}", flush=True)

if __name__ == "__main__":
  main()
