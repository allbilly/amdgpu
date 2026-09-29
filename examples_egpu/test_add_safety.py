"""Offline regression tests. No TinyGPU connection or hardware writes."""
import os
import struct
import types
import unittest
from unittest.mock import patch

import add


class RegisterModel:
  def __init__(self, registers=None, active_reads=None):
    self.registers = dict(registers or {})
    self.writes = []
    self.banks = []
    self.active_reads = iter(active_reads) if active_reads is not None else None
    dev = types.SimpleNamespace(mmio=None, bar0_size=256 << 20, _vram_start=0)
    self.boot = add.PolarisBoot(dev)
    self.boot.rreg = self.read
    self.boot.wreg = self.write
    self.boot.mmio_sync_safe = lambda: None
    self.boot.srbm_select = lambda *args: self.banks.append(args)

  def read(self, reg):
    if reg == add.mmCP_HQD_ACTIVE and self.active_reads is not None:
      return next(self.active_reads)
    return self.registers.get(reg, 0)

  def write(self, reg, value):
    self.writes.append((reg, value))
    self.registers[reg] = value


class MemoryTests(unittest.TestCase):
  def test_cold_apertures_above_four_gib(self):
    for mb in (4096, 8192, 16384):
      with self.subTest(mb=mb), patch.dict(os.environ, {"AMD_VRAM_MB": str(mb)}):
        model = RegisterModel()
        b = model.boot
        b.mc_program_light()
        fb = model.registers[add.mmMC_VM_FB_LOCATION]
        base = (fb & 0xffff) << 24
        end = (((fb >> 16) & 0xffff) << 24) | 0xffffff
        self.assertEqual(end - base + 1, mb << 20)
        self.assertEqual(b.vram_visible_mc, (mb << 20) - (256 << 20))
        self.assertGreater(b.agp_start, end)
        self.assertLess(b.agp_end, b.gart_start)
        self.assertEqual(model.registers[add.mmMC_VM_AGP_BOT] << 22, b.agp_start)
        self.assertEqual(model.registers[add.mmCONFIG_MEMSIZE], mb)

  def test_recompute_after_replacing_stale_fb(self):
    with patch.dict(os.environ, {"AMD_VRAM_MB": "16384"}):
      model = RegisterModel({add.mmMC_VM_FB_LOCATION: 0x1fff1000})
      model.boot.mc_program_light()
      self.assertEqual(model.boot.agp_start, 16 << 30)
      self.assertEqual(model.boot.vram_start, 0)

  def test_existing_valid_memory_not_overwritten(self):
    with patch.dict(os.environ, {"AMD_VRAM_MB": "16384"}):
      model = RegisterModel({add.mmCONFIG_MEMSIZE: 8192, add.mmMC_VM_FB_LOCATION: 0x1ff0000})
      model.boot.mc_program_light()
      self.assertFalse(any(r in (add.mmCONFIG_MEMSIZE, add.mmMC_VM_FB_LOCATION) for r, _ in model.writes))

  def test_invalid_fallback_fails_before_writes(self):
    for value in ("0", "127", "129", "65535", "65536", "-1", "garbage"):
      with self.subTest(value=value), patch.dict(os.environ, {"AMD_VRAM_MB": value}):
        model = RegisterModel()
        with self.assertRaises(ValueError):
          model.boot.mc_program_light()
        self.assertEqual(model.writes, [])


class QueueTests(unittest.TestCase):
  def test_stuck_queue_does_not_clear_pointers_or_request(self):
    model = RegisterModel({add.mmCP_HQD_ACTIVE: 1})
    with self.assertRaisesRegex(RuntimeError, "still active"):
      model.boot.deactivate_hqd(1, 0, 0, timeout_s=0)
    self.assertEqual(model.writes, [(add.mmCP_HQD_DEQUEUE_REQUEST, 1)])
    self.assertEqual(model.banks[-1], (0, 0, 0, 0))

  def test_dequeue_completion_precedes_pointer_clear(self):
    model = RegisterModel(active_reads=[1, 1, 0])
    model.boot.deactivate_hqd(1, 0, 0)
    self.assertEqual(model.writes, [(add.mmCP_HQD_DEQUEUE_REQUEST, 1),
                                  (add.mmCP_HQD_DEQUEUE_REQUEST, 0),
                                  (add.mmCP_HQD_PQ_RPTR, 0), (add.mmCP_HQD_PQ_WPTR, 0)])

  def test_quiesce_halts_mec_and_disables_polling(self):
    model = RegisterModel({add.mmCP_PQ_WPTR_POLL_CNTL: 0x80000123})
    with patch.object(add.time, "sleep"):
      model.boot.quiesce_compute()
    self.assertEqual(model.registers[add.mmCP_PQ_WPTR_POLL_CNTL], 0x123)
    self.assertEqual(model.registers[add.mmCP_MEC_CNTL], add.CP_MEC_CNTL_HALT)

  def test_quiesce_does_not_halt_mec_while_dequeue_pending(self):
    model = RegisterModel({add.mmCP_HQD_ACTIVE: 1})
    with self.assertRaises(RuntimeError):
      model.boot.quiesce_compute(timeout_s=0)
    self.assertNotIn(add.mmCP_MEC_CNTL, model.registers)

  def test_failed_halt_is_not_reported_as_success(self):
    model = RegisterModel()
    model.boot.cp_compute_enable = lambda enable: None
    with self.assertRaisesRegex(RuntimeError, "halt readback"):
      model.boot.quiesce_compute()


class RomTests(unittest.TestCase):
  def fixture(self):
    # Synthetic table only: no copyrighted/user VBIOS image in the tests.
    rom = bytearray(512)
    master, table, mod = 16, 128, 148
    struct.pack_into("<H", rom, master + 4 + add.MDT_IDX_VRAM_INFO * 2, table)
    struct.pack_into("<HBB", rom, table, 75, 2, 2)
    rom[table + 16], rom[table + 18] = 1, 8
    struct.pack_into("<H", rom, mod + 4, 55)
    struct.pack_into("<H", rom, mod + 20, 16384)
    rom[mod + 11] = 0x50
    rom[mod + 44:mod + 55] = b"EXAMPLE123\0\0"
    return rom, master, table, mod

  def test_v22_module_v8(self):
    rom, master, _, _ = self.fixture()
    result = add.parse_vram_info(rom, master)
    self.assertEqual(result["memory_size_mb"], 16384)
    self.assertEqual(result["part_number"], "EXAMPLE123")

  def test_unknown_revision_and_truncation(self):
    rom, master, table, mod = self.fixture()
    rom[table + 3] = 3
    self.assertIsNone(add.parse_vram_info(rom, master))
    rom[table + 3] = 2
    self.assertIsNone(add.parse_vram_info(rom[:mod + 43], master))
    struct.pack_into("<H", rom, mod + 4, 65535)
    self.assertIsNone(add.parse_vram_info(rom, master))


class IdentityTests(unittest.TestCase):
  def test_unknown_device_refused_before_bar_mapping(self):
    from unittest.mock import Mock
    pci = Mock()
    with patch.object(add, "APLRemotePCIDevice", return_value=pci), \
         patch.object(add.PolarisDevice, "_open_config", return_value=(0x1002, 0x6fdf)), \
         patch.dict(os.environ, {"AMD_EGPU_ALLOW_ANY": "0"}):
      with self.assertRaisesRegex(RuntimeError, "review compatibility"):
        add.PolarisDevice()
    pci.map_bar.assert_not_called()
    pci.mask_msi.assert_not_called()


if __name__ == "__main__":
  unittest.main()
