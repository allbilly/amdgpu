  .text
  .globl start
  .p2align 8
start:
  ;; Store AND, OR, XOR results at output dwords 0, 1, and 2.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 0x0000f0f0
  v_mov_b32 v2, 0x00000ff0
  v_and_b32 v3, v1, v2
  global_store_dword v0, v3, s[0:1] glc slc
  v_mov_b32 v0, 4
  v_or_b32 v3, v1, v2
  global_store_dword v0, v3, s[0:1] glc slc
  v_mov_b32 v0, 8
  v_xor_b32 v3, v1, v2
  global_store_dword v0, v3, s[0:1] glc slc
  s_waitcnt vmcnt(0)
  s_endpgm
