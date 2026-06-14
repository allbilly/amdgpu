  .text
  .globl start
  .p2align 8
start:
  ;; Store left and logical-right shift results at output dwords 0 and 1.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 1
  v_lshlrev_b32 v1, 8, v1
  global_store_dword v0, v1, s[0:1] glc slc
  v_mov_b32 v0, 4
  v_mov_b32 v1, 0x800
  v_lshrrev_b32 v1, 4, v1
  global_store_dword v0, v1, s[0:1] glc slc
  s_waitcnt vmcnt(0)
  s_endpgm
