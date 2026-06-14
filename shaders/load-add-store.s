  .text
  .globl start
  .p2align 8
start:
  ;; Load output[0] and output[1], add, then store the result at output[2].
  s_load_dword s2, s[0:1], 0x0
  s_load_dword s3, s[0:1], 0x4
  s_waitcnt lgkmcnt(0)
  s_add_u32 s4, s2, s3
  v_mov_b32 v0, 8
  v_mov_b32 v1, s4
  global_store_dword v0, v1, s[0:1] glc slc
  s_waitcnt vmcnt(0)
  s_endpgm
