  .text
  .globl start
  .p2align 8
start:
  ;; Compare 7 == 7 and select 0xaaaaaaaa when true.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 0xaaaaaaaa
  v_mov_b32 v2, 0xbbbbbbbb
  v_mov_b32 v4, 7
  v_cmp_eq_u32_e32 vcc, v4, v4
  v_cndmask_b32 v3, v2, v1, vcc
  global_store_dword v0, v3, s[0:1] glc slc
  s_waitcnt vmcnt(0)
  s_endpgm
