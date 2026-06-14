/*
 * RDNA3/GFX11 trap handler sketch based on thegeeko.md.
 *
 * This is intentionally kept as assembly source, not wired into amdgpu-poc by
 * default. Enabling it requires root, debugfs, ASIC-specific TBA/TMA register
 * offsets, and VMID banking through amdgpu-regs2.
 */

#define TMA_READY_OFFSET 0
#define TMA_PC_OFFSET 16
#define TMA_DATA_OFFSET 32
#define TMA_SREG_OFFSET 256
#define TMA_VREG_OFFSET 1024
#define TMA_VCC_OFFSET 2048
#define TMA_EXEC_OFFSET 2056

  .text
  .globl start
  .p2align 8
start:
  s_getreg_b32 ttmp8, hwreg(HW_REG_STATUS)
  s_mov_b64 ttmp[2:3], exec
  s_sendmsg_rtn_b64 ttmp[4:5], sendmsg(MSG_RTN_GET_TMA)
  s_waitcnt lgkmcnt(0)
  s_mov_b64 ttmp[6:7], vcc

  s_mov_b64 exec, -1
  global_store_addtid_b32 v0, ttmp[4:5], offset:TMA_VREG_OFFSET glc slc dlc
  global_store_addtid_b32 v1, ttmp[4:5], offset:TMA_VREG_OFFSET + 256 glc slc dlc
  global_store_addtid_b32 v2, ttmp[4:5], offset:TMA_VREG_OFFSET + 512 glc slc dlc
  global_store_addtid_b32 v3, ttmp[4:5], offset:TMA_VREG_OFFSET + 768 glc slc dlc
  global_store_addtid_b32 v4, ttmp[4:5], offset:TMA_VREG_OFFSET + 1024 glc slc dlc
  global_store_addtid_b32 v5, ttmp[4:5], offset:TMA_VREG_OFFSET + 1280 glc slc dlc
  global_store_addtid_b32 v6, ttmp[4:5], offset:TMA_VREG_OFFSET + 1536 glc slc dlc
  s_waitcnt vmcnt(0)

  s_mov_b64 exec, 1
  v_mov_b32 v1, s0
  v_mov_b32 v2, s1
  v_mov_b32 v3, s2
  v_mov_b32 v4, s3
  v_mov_b32 v5, s4
  v_mov_b32 v0, 0
  global_store_b32 v0, v1, ttmp[4:5], offset:TMA_SREG_OFFSET glc slc dlc
  global_store_b32 v0, v2, ttmp[4:5], offset:TMA_SREG_OFFSET + 4 glc slc dlc
  global_store_b32 v0, v3, ttmp[4:5], offset:TMA_SREG_OFFSET + 8 glc slc dlc
  global_store_b32 v0, v4, ttmp[4:5], offset:TMA_SREG_OFFSET + 12 glc slc dlc
  global_store_b32 v0, v5, ttmp[4:5], offset:TMA_SREG_OFFSET + 16 glc slc dlc
  s_waitcnt vmcnt(0)

  s_getreg_b32 ttmp10, hwreg(HW_REG_HW_ID1)
  s_getreg_b32 ttmp11, hwreg(HW_REG_HW_ID2)
  v_mov_b32 v3, ttmp10
  v_mov_b32 v4, ttmp11
  global_store_dwordx2 v0, v[3:4], ttmp[4:5], offset:TMA_DATA_OFFSET glc slc dlc

  v_mov_b32 v3, ttmp6
  v_mov_b32 v4, ttmp7
  global_store_dwordx2 v0, v[3:4], ttmp[4:5], offset:TMA_VCC_OFFSET glc slc dlc

  v_mov_b32 v3, ttmp2
  v_mov_b32 v4, ttmp3
  global_store_dwordx2 v0, v[3:4], ttmp[4:5], offset:TMA_EXEC_OFFSET glc slc dlc

  v_mov_b32 v3, ttmp0
  v_mov_b32 v4, ttmp1
  v_and_b32 v4, v4, 0xffff
  global_store_dwordx2 v0, v[3:4], ttmp[4:5], offset:TMA_PC_OFFSET glc slc dlc
  s_waitcnt vmcnt(0)

spin:
  global_load_dword v1, v0, ttmp[4:5] glc slc dlc
spin_wait_ib:
  s_getreg_b32 ttmp13, hwreg(HW_REG_IB_STS, 10, 15)
  s_and_b32 ttmp13, ttmp13, ttmp13
  s_cbranch_scc1 spin_wait_ib
  v_readfirstlane_b32 ttmp13, v1
  s_and_b32 ttmp13, ttmp13, ttmp13
  s_cbranch_scc0 spin

  v_mov_b32 v0, 0
  v_mov_b32 v1, 0
  global_store_dword v0, v1, ttmp[4:5] glc slc dlc
  s_waitcnt vmcnt(0)

  s_and_b32 ttmp9, ttmp1, 0xffff0000
  s_lshr_b32 ttmp9, ttmp9, 16
  s_cmpk_eq_u32 ttmp9, 0
  s_cbranch_scc1 restore
  s_add_u32 ttmp0, ttmp0, 4
  s_addc_u32 ttmp1, ttmp1, 0

restore:
  s_load_dwordx4 s[0:3], ttmp[4:5], TMA_SREG_OFFSET glc dlc
  s_load_dword s4, ttmp[4:5], TMA_SREG_OFFSET + 16 glc dlc
  s_waitcnt lgkmcnt(0)

  s_mov_b64 exec, -1
  global_load_addtid_b32 v0, ttmp[4:5], offset:TMA_VREG_OFFSET glc slc dlc
  global_load_addtid_b32 v1, ttmp[4:5], offset:TMA_VREG_OFFSET + 256 glc slc dlc
  global_load_addtid_b32 v2, ttmp[4:5], offset:TMA_VREG_OFFSET + 512 glc slc dlc
  global_load_addtid_b32 v3, ttmp[4:5], offset:TMA_VREG_OFFSET + 768 glc slc dlc
  global_load_addtid_b32 v4, ttmp[4:5], offset:TMA_VREG_OFFSET + 1024 glc slc dlc
  global_load_addtid_b32 v5, ttmp[4:5], offset:TMA_VREG_OFFSET + 1280 glc slc dlc
  global_load_addtid_b32 v6, ttmp[4:5], offset:TMA_VREG_OFFSET + 1536 glc slc dlc
  s_waitcnt vmcnt(0)

  s_and_b32 ttmp1, ttmp1, 0xffff
  s_load_b64 vcc, ttmp[4:5], TMA_VCC_OFFSET glc dlc
  s_load_b64 ttmp[2:3], ttmp[4:5], TMA_EXEC_OFFSET glc dlc
  s_waitcnt lgkmcnt(0)
  s_mov_b64 exec, ttmp[2:3]
  s_and_b64 exec, exec, exec
  s_and_b64 vcc, vcc, vcc
  s_setreg_b32 hwreg(HW_REG_STATUS, 0, 1), ttmp8
  s_waitcnt vmcnt(0) lgkmcnt(0) expcnt(0)
  s_rfe_b64 [ttmp0, ttmp1]
