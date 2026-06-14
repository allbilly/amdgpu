  .text
  .globl start
  .p2align 8
start:
  ;; Each active lane stores one dword at output + lane_id * 4.
  v_mbcnt_lo_u32_b32 v0, -1, 0
  v_mbcnt_hi_u32_b32 v0, -1, v0
  v_lshlrev_b32 v1, 2, v0
  v_add_u32_e32 v2, 0x11110000, v0
  global_store_dword v1, v2, s[0:1] glc slc
  s_waitcnt vmcnt(0)
  s_endpgm
