  .text
  .globl start
  .p2align 8
start:
  ;; Each active lane atomically increments output[0] by 1.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 1
  global_atomic_add v0, v1, s[0:1] slc
  s_waitcnt vmcnt(0)
  s_endpgm
