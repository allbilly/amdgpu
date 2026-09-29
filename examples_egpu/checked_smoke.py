#!/usr/bin/env python3
"""Supervised single-add experiment; keep DMA mappings until physical removal.

Default is offline validation. --execute requires attended cooling and an
operator able to switch off the eGPU when POWER_OFF_REQUIRED is printed.
This is not a thermal stress test or a general device-shutdown implementation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

import add


class SafetyAbort(BaseException):
  """Bypass upstream retry/fallback handlers that catch Exception."""


def validate_inputs(rom_path, firmware_dir, device_id, vram_mb):
  rom = rom_path.read_bytes()
  if not add.check_atom_bios(rom) or sum(rom) % 256:
    raise ValueError("Invalid ATOM ROM or checksum")
  pcir = struct.unpack_from("<H", rom, 0x18)[0]
  if rom[pcir:pcir + 4] != b"PCIR" or struct.unpack_from("<HH", rom, pcir + 4) != (0x1002, device_id):
    raise ValueError("ROM PCI identity does not match requested device")
  ctx = add.parse_atom_context(rom)
  memory = add.parse_vram_info(rom, ctx.data_table)
  if memory is None or memory["module_num"] != 1 or memory["memory_size_mb"] != vram_mb:
    raise ValueError("ROM memory size/module selection is not verified")
  manifest = json.loads((firmware_dir / "manifest.json").read_text())
  blobs = {}
  for suffix in ("smc", "smc_sk", "mc", "rlc", "ce", "pfp", "me", "mec", "sdma", "sdma1"):
    name = f"polaris10_{suffix}.bin"
    blob = (firmware_dir / name).read_bytes()
    if hashlib.sha256(blob).hexdigest() != manifest[name]["sha256"]:
      raise ValueError(f"Firmware manifest mismatch: {name}")
    offset, size = add.parse_common_fw(blob)
    if size == 0 or offset + size > len(blob):
      raise ValueError(f"Firmware bounds invalid: {name}")
    blobs[name] = blob
  return blobs


def main():
  p = argparse.ArgumentParser(description=__doc__)
  p.add_argument("--rom", type=Path, required=True)
  p.add_argument("--firmware-dir", type=Path, required=True)
  p.add_argument("--device-id", type=lambda s: int(s, 0), required=True)
  p.add_argument("--vram-mb", type=int, required=True)
  p.add_argument("--output", type=Path, required=True)
  p.add_argument("--execute", action="store_true")
  p.add_argument("--cooling-confirmed", action="store_true")
  args = p.parse_args()
  if args.device_id not in (0x67df, 0x6fdf):
    p.error("Only Polaris10 IDs 0x67df/0x6fdf are considered by this experiment")
  blobs = validate_inputs(args.rom, args.firmware_dir, args.device_id, args.vram_mb)
  report = {"device_id": hex(args.device_id), "vram_mb": args.vram_mb,
            "input_validation": "passed", "compute_passed": False, "temperatures_c": []}
  def save():
    args.output.write_text(json.dumps(report, indent=2) + "\n")
  save()
  if not args.execute:
    print("Offline inputs validated; no device connection.")
    return
  if not args.cooling_confirmed:
    p.error("--execute requires --cooling-confirmed and an attended power-off recovery")

  # Ignore inherited experimental force/retry knobs; this is one controlled run.
  for key in list(os.environ):
    if key.startswith(("AMD_BOOT_", "AMD_EGPU_", "AMD_ATOM_", "AMD_SMC_")):
      del os.environ[key]
  add.apply_add_defaults()
  os.environ.update({"AMD_VRAM_MB": str(args.vram_mb), "AMD_EGPU_ALLOW_ANY": "1",
                     "AMD_EGPU_NO_AUTO_RESET": "1", "AMD_BOOT_ATTEMPTS": "1",
                     "AMD_BOOT_RESET": "0", "AMD_BOOT_VBIOS_FILE": str(args.rom.resolve()),
                     "AMD_BOOT_PROT_FALLBACK": "0", "AMD_BOOT_NO_PROT_FALLBACK": "1"})
  # Never fetch firmware during a partially initialized hardware session.
  add.PolarisBoot.fw = lambda self, name: blobs[name]
  dev = None
  armed = False
  enabled = False
  checking = False
  next_sample = 0.0
  deadline = float("inf")
  original_rpc = add.RemotePCIDevice._rpc
  original_write = add.RemotePCIDevice._bulk_write

  def temperature():
    saved = dev.mmio[add.mmSMC_IND_INDEX_11]
    try:
      dev.mmio[add.mmSMC_IND_INDEX_11] = 0xc0300014
      raw = dev.mmio[add.mmSMC_IND_DATA_11]
    finally:
      dev.mmio[add.mmSMC_IND_INDEX_11] = saved
    field = (raw & 0x3fe00) >> 9
    if raw in (0, 0xffffffff) or field & 0x200 or not 5 <= field <= 125:
      raise SafetyAbort(f"Invalid temperature register: {raw:#x}")
    report["temperatures_c"].append(field)
    print(f"temperature_c={field}", flush=True)
    if field >= 70:
      raise SafetyAbort("Conservative 70 C stop threshold reached")

  def check():
    nonlocal checking, next_sample
    if not enabled or checking:
      return
    if time.monotonic() >= deadline:
      raise SafetyAbort("45-second startup/dispatch deadline reached")
    if time.monotonic() >= next_sample:
      checking = True
      try:
        temperature()
      finally:
        checking = False
      next_sample = time.monotonic() + 0.5

  def guarded_rpc(*a, **kw):
    check()
    try:
      return original_rpc(*a, **kw)
    except (OSError, RuntimeError) as exc:
      if enabled:
        raise SafetyAbort(f"Transport failed: {exc}") from exc
      raise

  def guarded_write(self, *a, **kw):
    check()
    return original_write(self, *a, **kw)

  with tempfile.TemporaryDirectory(prefix="polaris-smoke-") as directory:
    sockpath = str(Path(directory) / "gpu.sock")
    os.environ["APL_REMOTE_SOCK"] = sockpath
    with open(args.output.with_suffix(".server.log"), "w") as log:
      server = subprocess.Popen([add.APLRemotePCIDevice.APP_PATH, "server", sockpath], stdout=log, stderr=log)
      try:
        for _ in range(100):
          if server.poll() is not None:
            raise RuntimeError("TinyGPU server exited before startup")
          if Path(sockpath).exists():
            break
          time.sleep(0.05)
        else:
          raise RuntimeError("TinyGPU server did not start")
        # Verify exact live identity before the constructor can map BARs/write.
        class CheckedDevice(add.PolarisDevice):
          @classmethod
          def _open_config(cls, pci, reset=False, **kw):
            pci.sock.settimeout(2)
            ids = cls._read_pci_ids(pci)
            if ids != (0x1002, args.device_id) or reset:
              raise RuntimeError("Unexpected live device or reset request")
            return ids
          def run_add(self, *a, **kw):
            result = super().run_add(*a, **kw)
            report["actual_result"] = result
            return result
        dev = CheckedDevice()
        dev.pci.sock.settimeout(2)
        if dev.reg(add.REG_CP_MEC_CNTL) != add.CP_MEC_CNTL_HALT:
          raise RuntimeError("Expected cold halted MEC; power cycle before testing")
        add.RemotePCIDevice._rpc = staticmethod(guarded_rpc)
        add.RemotePCIDevice._bulk_write = guarded_write
        deadline = time.monotonic() + 45
        enabled = True
        check()
        armed = True
        dev._op_cases = [((1., 2., 3., 4.), (10., 20., 30., 40.))]
        dev.boot(stage="add")
        if report.get("actual_result") != [11., 22., 33., 44.]:
          raise RuntimeError("No verified dispatch result (startup may have skipped submission)")
        report["compute_passed"] = True
        report["expected_result"] = [11., 22., 33., 44.]
      except (Exception, SafetyAbort, KeyboardInterrupt) as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        print(report["error"], flush=True)
      finally:
        enabled = False
        if armed:
          try:
            dev._boot.quiesce_compute()
            report["compute_quiesce"] = "verified queue inactivity and MEC halt"
          except Exception as exc:
            report["compute_quiesce"] = f"failed: {exc}"
          report["recovery"] = "waiting for physical eGPU power-off/removal; DMA mappings retained"
          save()
          print("POWER_OFF_REQUIRED: turn off/unplug eGPU now. Keeping transport and DMA mappings alive.", flush=True)
          # TinyGPU exits on kIOMessageServiceIsTerminated. Do not drop its
          # connection while SMC/other DMA clients may still own host mappings.
          while server.poll() is None:
            try:
              time.sleep(0.2)
            except KeyboardInterrupt:
              print("Power off/unplug the eGPU to finish recovery; mappings still retained.", flush=True)
          report["server_exit_code"] = server.returncode
          report["recovery"] = "TinyGPU server exited; operator must confirm physical power-off before reuse"
        if dev is not None:
          dev.pci.sock.close()
        if server.poll() is None:
          server.terminate()
          server.wait(timeout=3)
        add.RemotePCIDevice._rpc = staticmethod(original_rpc)
        add.RemotePCIDevice._bulk_write = original_write
        save()
  print(json.dumps(report, indent=2))


if __name__ == "__main__":
  main()
