  .text
  .globl start
  .p2align 8
start:
  ;; s[0:1] is an output buffer address programmed through COMPUTE_USER_DATA_0/1.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 9
  v_mov_b32 v2, 4
  v_sub_u32_e32 v1, v1, v2
  global_store_dword v0, v1, s[0:1] glc slc
  s_waitcnt vmcnt(0)
  s_endpgm
