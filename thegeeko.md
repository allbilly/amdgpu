https://thegeeko.me/blog/amd-gpu-debugging/

index
AMD GPU Debugger
2025.12.06
I’ve always wondered why we don’t have a GPU debugger similar to the one used for CPUs. A tool that allows pausing execution and examining the current state. This capability feels essential, especially since the GPU’s concurrent execution model is much harder to reason about. After searching for solutions, I came across rocgdb, a debugger for AMD’s ROCm environment. Unfortunately, its scope is limited to that environment. Still, this shows it’s technically possible. I then found a helpful series of blog posts by Marcell Kiss, detailing how he achieved this, which inspired me to try to recreate the process myself.

Let’s Try To Talk To The GPU Directly
The best place to start learning about this is RADV. By tracing what it does, we can find how to do it. Our goal here is to run the most basic shader nop 0 without using Vulkan, aka RADV in our case.

First of all, we need to open the DRM file to establish a connection with the KMD, using a simple open(“/dev/dri/cardX”), then we find that it’s calling amdgpu_device_initialize, which is a function defined in libdrm, which is a library that acts as middleware between user mode drivers(UMD) like RADV and and kernel mode drivers(KMD) like amdgpu driver, and then when we try to do some actual work we have to create a context which can be achieved by calling amdgpu_cs_ctx_create from libdrm again, next up we need to allocate 2 buffers one of them for our code and the other for writing our commands into, we do this by calling a couple of functions, here’s how I do it:


void bo_alloc(amdgpu_t* dev, size_t size, u32 domain, bool uncached, amdgpubo_t* bo) {
 s32    ret         = -1;
 u32    alignment   = 0;
 u32    flags       = 0;
 size_t actual_size = 0;

 amdgpu_bo_handle bo_handle = NULL;
 amdgpu_va_handle va_handle = NULL;
 u64              va_addr   = 0;
 void*            host_addr = NULL;
Here we’re choosing the domain and assigning flags based on the params, some buffers we will need uncached, as we will see:


 if (
   domain != AMDGPU_GEM_DOMAIN_GWS && domain != AMDGPU_GEM_DOMAIN_GDS &&
   domain != AMDGPU_GEM_DOMAIN_OA) {
  actual_size = (size + 4096 - 1) & 0xFFFFFFFFFFFFF000ULL;
  alignment   = 4096;
  flags       = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED | AMDGPU_GEM_CREATE_VRAM_CLEARED |
          AMDGPU_GEM_CREATE_VM_ALWAYS_VALID;
  flags |=
    uncached ? (domain == AMDGPU_GEM_DOMAIN_GTT) * AMDGPU_GEM_CREATE_CPU_GTT_USWC : 0;
 } else {
  actual_size = size;
  alignment   = 1;
  flags       = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
 }

 struct amdgpu_bo_alloc_request req = {
  .alloc_size     = actual_size,
  .phys_alignment = alignment,
  .preferred_heap = domain,
  .flags          = flags,
 };

 // memory aquired!!
 ret = amdgpu_bo_alloc(dev->dev_handle, &req, &bo_handle);
 HDB_ASSERT(!ret, "can't allocate bo");
Now we have the memory, we need to map it. I opt to map anything that can be CPU-mapped for ease of use. We have to map the memory to both the GPU and the CPU virtual space. The KMD creates the page table when we open the DRM file, as shown here.

So map it to the GPU VM and, if possible, to the CPU VM as well. Here, at this point, there’s a libdrm function that does all of this setup for us and maps the memory, but I found that even when specifying AMDGPU_VM_MTYPE_UC, it doesn’t always tag the page as uncached, not quite sure if it’s a bug in my code or something in libdrm anyways, the function is amdgpu_bo_va_op, I opted to do it manually here and issue the IOCTL call myself:


 u32 kms_handle = 0;
 amdgpu_bo_export(bo_handle, amdgpu_bo_handle_type_kms, &kms_handle);

 ret = amdgpu_va_range_alloc(
   dev->dev_handle,
   amdgpu_gpu_va_range_general,
   actual_size,
   4096,
   0,
   &va_addr,
   &va_handle,
   0);
 HDB_ASSERT(!ret, "can't allocate VA");

 u64 map_flags =
   AMDGPU_VM_PAGE_EXECUTABLE | AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE;
 map_flags |= uncached ? AMDGPU_VM_MTYPE_UC | AMDGPU_VM_PAGE_NOALLOC : 0;

 struct drm_amdgpu_gem_va va = {
  .handle       = kms_handle,
  .operation    = AMDGPU_VA_OP_MAP,
  .flags        = map_flags,
  .va_address   = va_addr,
  .offset_in_bo = 0,
  .map_size     = actual_size,

 };

 ret = drm_ioctl_write_read(dev->drm_fd, DRM_AMDGPU_GEM_VA, &va, sizeof(va));
 HDB_ASSERT(!ret, "can't map bo in GPU space");
 // ret = amdgpu_bo_va_op(bo_handle, 0, actual_size, va_addr, map_flags,
 // AMDGPU_VA_OP_MAP);

 if (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) {
  ret = amdgpu_bo_cpu_map(bo_handle, &host_addr);
  HDB_ASSERT(!ret, "can't map bo in CPU space");

  // AMDGPU_GEM_CREATE_VRAM_CLEARED doesn't really memset the memory to 0 anyways for
  // debug I'll just do it manually for now
  memset(host_addr, 0x0, actual_size);
 }

 *bo = (amdgpubo_t){
  .bo_handle = bo_handle,
  .va_handle = va_handle,
  .va_addr   = va_addr,
  .size      = actual_size,
  .host_addr = host_addr,
 };
}
Now we have the context and 2 buffers. Next, fill those buffers and send our commands to the KMD, which will then forward them to the Command Processor (CP) in the GPU for processing.

Let’s compile our code. We can use clang assembler for that, like this:


# https://gitlab.freedesktop.org/martty/radbg-poc/-/blob/master/ll-as.sh
clang -c -x assembler -target amdgcn-amd-amdhsa -mcpu=gfx1100 -o asm.o "$1"
objdump -h asm.o | grep .text | awk '{print "dd if='asm.o' of='asmc.bin' bs=1 count=$[0x" $3 "] skip=$[0x" $6 "] status=none"}' | bash
#rm asm.o
The bash script compiles the code, and then we’re only interested in the actual machine code, so we use objdump to figure out the offset and the size of the section and copy it to a new file called asmc.bin, then we can just load the file and write its bytes to the CPU-mapped address of the code buffer.

Next up, filling in the commands. This was extremely confusing for me because it’s not well documented. It was mostly learning how RADV does things and trying to do similar things. Also, shout-out to the folks on the Graphics Programming Discord server for helping me, especially Picoduck. The commands are encoded in a special format called PM4 Packets, which has multiple types. We only care about Type 3: each packet has an opcode and the number of bytes it contains.

The first thing we need to do is program the GPU registers, then dispatch the shader. Some of those registers are rsrc[1-3]; those registers are responsible for a number of configurations, pgm_[lo/hi], which hold the pointer to the code buffer and num_thread_[x/y/z]; those are responsible for the number of threads inside a work group. All of those are set using the set shader register packets, and here is how to encode them:

It’s worth mentioning that we can set multiple registers in 1 packet if they’re consecutive.


void pkt3_set_sh_reg(pkt3_packets_t* packets, u32 reg, u32 value) {
 HDB_ASSERT(
   reg >= SI_SH_REG_OFFSET && reg < SI_SH_REG_END,
   "can't set register outside sh registers span");

 // packet header
 da_append(packets, PKT3(PKT3_SET_SH_REG, 1, 0));
 // offset of the register
 da_append(packets, (reg - SI_SH_REG_OFFSET) / 4);
 da_append(packets, value);
}
Then we append the dispatch command:


// we're going for 1 thread since we want the simplest case here.

da_append(&pkt3_packets, PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | PKT3_SHADER_TYPE_S(1));
da_append(&pkt3_packets, 1u);
da_append(&pkt3_packets, 1u);
da_append(&pkt3_packets, 1u);
da_append(&pkt3_packets, dispatch_initiator);
Now we want to write those commands into our buffer and send them to the KMD:


void dev_submit(
  amdgpu_t*         dev,
  pkt3_packets_t*   packets,
  amdgpu_bo_handle* buffers,
  u32               buffers_count,
  amdgpu_submit_t*  submit
) {
 s32        ret = -1;
 amdgpubo_t ib  = { 0 };

 bo_alloc(dev, pkt3_size(packets), AMDGPU_GEM_DOMAIN_GTT, false, &ib);
 bo_upload(&ib, packets->data, pkt3_size(packets));

 amdgpu_bo_handle* bo_handles = // +1 for the indirect buffer
   (amdgpu_bo_handle*)malloc(sizeof(amdgpu_bo_handle) * (buffers_count + 1));

 bo_handles[0] = ib.bo_handle;
 for_range(i, 0, buffers_count) {
  bo_handles[i + 1] = buffers[i];
 }

 amdgpu_bo_list_handle bo_list = NULL;
 ret =
   amdgpu_bo_list_create(dev->dev_handle, buffers_count + 1, bo_handles, NULL, &bo_list);
 HDB_ASSERT(!ret, "can't create a bo list");
 free(bo_handles);

 struct amdgpu_cs_ib_info ib_info = {
  .flags         = 0,
  .ib_mc_address = ib.va_addr,
  .size          = packets->count,
 };

 struct amdgpu_cs_request req = {
  .flags                  = 0,
  .ip_type                = AMDGPU_HW_IP_COMPUTE,
  .ip_instance            = 0,
  .ring                   = 0,
  .resources              = bo_list,
  .number_of_dependencies = 0,
  .dependencies           = NULL,
  .number_of_ibs          = 1,
  .ibs                    = &ib_info,
  .seq_no                 = 0,
  .fence_info             = { 0 },
 };

 ret = amdgpu_cs_submit(dev->ctx_handle, 0, &req, 1);
 HDB_ASSERT(!ret, "can't submit indirect buffer request");

 *submit = (amdgpu_submit_t){
    .ib = ib,
    .bo_list = bo_list,
    .fence = {
      .context = dev->ctx_handle,
      .ip_type = AMDGPU_HW_IP_COMPUTE,
      .ip_instance = 0,
      .ring = 0,
      .fence = req.seq_no,
    },
  };
}
Here is a good point to make a more complex shader that outputs something. For example, writing 1 to a buffer.

No GPU hangs ?! nothing happened ?! cool, cool, now we have a shader that runs on the GPU, what’s next? Let’s try to hang the GPU by pausing the execution, aka make the GPU trap.

TBA/TMA
The RDNA3’s ISA manual does mention 2 registers, TBA, TMA; here’s how they describe them respectively:

Holds the pointer to the current trap handler program address. Per-VMID register. Bit [63] indicates if the trap handler is present (1) or not (0) and is not considered part of the address (bit[62] is replicated into address bit[63]). Accessed via S_SENDMSG_RTN.

Temporary register for shader operations. For example, it can hold a pointer to memory used by the trap handler.

You can configure the GPU to enter the trap handler when encountering certain exceptions listed in the RDNA3 ISA manual.

We know from Marcell Kiss’s blog posts that we need to compile a trap handler, which is a normal shader the GPU switches to when encountering a s_trap. The TBA register has a special bit that indicates whether the trap handler is enabled.

Since these are privileged registers, we cannot write to them from user space. To bridge this gap for debugging, we can utilize the debugfs interface. Luckily, we have UMR, which uses that debugfs interface, and it’s open source; we copy AMD’s homework here which is great.

AMDGPU Debugfs
The amdgpu KMD has a couple of files in debugfs under /sys/kernel/debug/dri/{PCI address}; one of them is regs2, which is an interface to a amdgpu_debugfs_regs2_write in the kernel that writes to the registers. It works by simply opening the file, seeking the register’s offset, and then writing; it also performs some synchronisation and writes the value correctly. We need to provide more parameters about the register before writing to the file, tho and do that by using an ioctl call. Here are the ioctl arguments:


typedef struct amdgpu_debugfs_regs2_iocdata_v2 {
 __u32 use_srbm, use_grbm, pg_lock;
 struct {
  __u32 se, sh, instance;
 } grbm;
 struct {
  __u32 me, pipe, queue, vmid;
 } srbm;
 __u32 xcc_id;
} regs2_ioc_data_t;
The 2 structs are because there are 2 types of registers, GRBM and SRBM, each of which is banked by different constructs; you can learn more about some of them here in the Linux kernel documentation.

Turns out our registers here are SBRM registers and banked by VMIDs, meaning each VMID has its own TBA and TMA registers. Cool, now we need to figure out the VMID of our process. As far as I understand, VMIDs are a way for the GPU to identify a specific process context, including the page table base address, so the address translation unit can translate a virtual memory address. The context is created when we open the DRM file. They get assigned dynamically at dispatch time, which is a problem for us; we want to write to those registers before dispatch.

We can obtain the VMID of the dispatched process by querying the HW_ID2 register with s_getreg_b32. I do a hack here, by enabling the trap handler in every VMID, and there are 16 of them, the first being special, and used by the KMD and the last 8 allocated to the amdkfd driver. We loop over the remaining VMIDs and write to those registers. This can cause issues to other processes using other VMIDs by enabling trap handlers in them and writing the virtual address of our trap handler, which is only valid within our virtual memory address space. It’s relatively safe tho since most other processes won’t cause a trap1.

Now we can write to TMA and TBA, here’s the code:


void dev_op_reg32(
  amdgpu_t* dev, gc_11_reg_t reg, regs2_ioc_data_t ioc_data, reg_32_op_t op, u32* value) {
 s32 ret = 0;

 reg_info_t reg_info     = gc_11_regs_infos[reg];
 uint64_t   reg_offset   = gc_11_regs_offsets[reg];
 uint64_t   base_offset  = dev->gc_regs_base_addr[reg_info.soc_index];
 uint64_t   total_offset = (reg_offset + base_offset);

 // seems like we're multiplying by 4 here because the registers database in UMRs
 // source has them in indexes rather than bytes.
 total_offset *= (reg_info.type == REG_MMIO) ? 4 : 1;

 ret = hdb_ioctl(dev->regs2_fd, AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2, &ioc_data);
 HDB_ASSERT(!ret, "Failed to set registers state");

 size_t size = lseek(dev->regs2_fd, total_offset, SEEK_SET);
 HDB_ASSERT(size == total_offset, "Failed to seek register address");

 switch (op) {
 case REG_OP_READ : size = read(dev->regs2_fd, value, 4); break;
 case REG_OP_WRITE: size = write(dev->regs2_fd, value, 4); break;
 default          : HDB_ASSERT(false, "unsupported op");
 }

 HDB_ASSERT(size == 4, "Failed to write/read the values to/from the register");
}
And here’s how we write to TMA and TBA: If you noticed, I’m using bitfields. I use them because working with them is much easier than macros, and while the byte order is not guaranteed by the C spec, it’s guaranteed by System V ABI, which Linux adheres to.


void dev_setup_trap_handler(amdgpu_t* dev, u64 tba, u64 tma) {
 reg_sq_shader_tma_lo_t tma_lo = { .raw = (u32)(tma) };
 reg_sq_shader_tma_hi_t tma_hi = { .raw = (u32)(tma >> 32) };

 reg_sq_shader_tba_lo_t tba_lo = { .raw = (u32)(tba >> 8) };
 reg_sq_shader_tba_hi_t tba_hi = { .raw = (u32)(tba >> 40) };

 tba_hi.trap_en = 1;

 regs2_ioc_data_t ioc_data = {
  .use_srbm = 1,
  .xcc_id   = -1,
 };

 // NOTE(hadi):
 // vmid's get assigned when code starts executing before hand we don't know which vmid
 // will get assigned to our process so we just set all of them
 for_range(i, 1, 9) {
  ioc_data.srbm.vmid = i;
  dev_op_reg32(dev, REG_SQ_SHADER_TBA_LO, ioc_data, REG_OP_WRITE, &tba_lo.raw);
  dev_op_reg32(dev, REG_SQ_SHADER_TBA_HI, ioc_data, REG_OP_WRITE, &tba_hi.raw);

  dev_op_reg32(dev, REG_SQ_SHADER_TMA_LO, ioc_data, REG_OP_WRITE, &tma_lo.raw);
  dev_op_reg32(dev, REG_SQ_SHADER_TMA_HI, ioc_data, REG_OP_WRITE, &tma_hi.raw);
 }
}
Anyway, now that we can write to those registers, if we enable the trap handler correctly, the GPU should hang when we launch our shader if we added s_trap instruction to it, or we enabled the TRAP_ON_START bit in rsrc32 register.

Now, let’s try to write a trap handler.

The Trap Handler
If you wrote a different shader that outputs to a buffer, u can try writing to that shader from the trap handler, which is nice to make sure it’s actually being run.

We need 2 things: our trap handler and some scratch memory to use when needed, which we will store the address of in the TMA register.

The trap handler is just a normal program running in privileged state, meaning we have access to special registers like TTMP[0-15]. When we enter a trap handler, we need to first ensure that the state of the GPU registers is saved, just as the kernel does for CPU processes when context-switching, by saving a copy of the stable registers and the program counter, etc. The problem, tho, is that we don’t have a stable ABI for GPUs, or at least not one I’m aware of, and compilers use all the registers they can, so we need to save everything.

AMD GPUs’ Command Processors (CPs) have context-switching functionality, and the amdkfd driver does implement some context-switching shaders. The problem is they’re not documented, and we have to figure them out from the amdkfd driver source and from other parts of the driver stack that interact with it, which is a pain in the ass. I kinda did a workaround here since I didn’t find luck understanding how it works, and some other reasons I’ll discuss later in the post.

The workaround here is to use only TTMP registers and a combination of specific instructions to copy the values of some registers, allowing us to use more instructions to copy the remaining registers. The main idea is to make use of the global_store_addtid_b32 instruction, which adds the index of the current thread within the wave to the writing address, aka

ID 
thread
​
 ∗4+address
This allows us to write a unique value per thread using only TTMP registers, which are unique per wave, not per thread3, so we can save the context of a single wave.

The problem is that if we have more than 1 wave, they will overlap, and we will have a race condition.

Here is the code:


start:
 ;; save the STATUS word into ttmp8
 s_getreg_b32 ttmp8, hwreg(HW_REG_STATUS)

 ;; save exec into ttmp[2:3]
 s_mov_b64 ttmp[2:3], exec

 ;; getting the address of our tma buffer
 s_sendmsg_rtn_b64 ttmp[4:5], sendmsg(MSG_RTN_GET_TMA)
 s_waitcnt lgkmcnt(0)

 ;; save vcc
 s_mov_b64 ttmp[6:7], vcc

 ;; enable all threads so they can write their vgpr registers
 s_mov_b64 exec, -1

 ;; FIXME(hadi): this assumes only 1 wave is running
 global_store_addtid_b32 v0, ttmp[4:5], offset:TMA_VREG_OFFSET        glc slc dlc
 global_store_addtid_b32 v1, ttmp[4:5], offset:TMA_VREG_OFFSET + 256  glc slc dlc
 global_store_addtid_b32 v2, ttmp[4:5], offset:TMA_VREG_OFFSET + 512  glc slc dlc
 global_store_addtid_b32 v3, ttmp[4:5], offset:TMA_VREG_OFFSET + 768  glc slc dlc
 global_store_addtid_b32 v4, ttmp[4:5], offset:TMA_VREG_OFFSET + 1024 glc slc dlc
 global_store_addtid_b32 v5, ttmp[4:5], offset:TMA_VREG_OFFSET + 1280 glc slc dlc
 global_store_addtid_b32 v6, ttmp[4:5], offset:TMA_VREG_OFFSET + 1536 glc slc dlc
 s_waitcnt vmcnt(0)

 ;; only first thread is supposed to write sgprs of the wave
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

 ;; enable all threads
 s_mov_b64 exec, -1
Now that we have those values in memory, we need to tell the CPU: Hey, we got the data, and pause the GPU’s execution until the CPU issues a command. Also, notice we can just modify those from the CPU.

Before we tell the CPU, we need to write some values that might help the CPU. Here are they:


 ;; IDs to identify which parts of the hardware we are running on exactly
 s_getreg_b32 ttmp10, hwreg(HW_REG_HW_ID1)
 s_getreg_b32 ttmp11, hwreg(HW_REG_HW_ID2)
 v_mov_b32 v3, ttmp10
 v_mov_b32 v4, ttmp11
 global_store_dwordx2 v1, v[3:4], ttmp[4:5], offset:TMA_DATA_OFFSET glc slc dlc

 ;; the original vcc mask
 v_mov_b32 v3, ttmp6
 v_mov_b32 v4, ttmp7
 global_store_dwordx2 v1, v[3:4], ttmp[4:5], offset:2048 glc slc dlc
 s_waitcnt vmcnt(0)

 ;; the original exec mask
 v_mov_b32 v3, ttmp2
 v_mov_b32 v4, ttmp3
 global_store_dwordx2 v1, v[3:4], ttmp[4:5], offset:2056 glc slc dlc
 s_waitcnt vmcnt(0)

 ;; the program counter
 v_mov_b32 v3, ttmp0
 v_mov_b32 v4, ttmp1
 v_and_b32 v4, v4, 0xffff
 global_store_dwordx2 v1, v[3:4], ttmp[4:5], offset:16 glc slc dlc

 s_waitcnt vmcnt(0)
Now the GPU should just wait for the CPU, and here’s the spin code it’s implemented as described by Marcell Kiss here:


SPIN:
 global_load_dword v1, v2, ttmp[4:5] glc slc dlc

SPIN1:
 // I found the bit range of 10 to 15 using trial and error in the
 // isa manual specifies that it's a 6-bit number but the offset 10
 // is just trial and error
  s_getreg_b32 ttmp13, hwreg(HW_REG_IB_STS, 10, 15)
 s_and_b32 ttmp13, ttmp13, ttmp13
 s_cbranch_scc1 SPIN1

 v_readfirstlane_b32 ttmp13, v1
 s_and_b32 ttmp13, ttmp13, ttmp13
 s_cbranch_scc0 SPIN

CLEAR:
 v_mov_b32 v2, 0
 v_mov_b32 v1, 0
 global_store_dword v1, v2, ttmp[4:5] glc slc dlc
 s_waitcnt vmcnt(0)
The main loop in the CPU is like enable trap handler, then dispatch shader, then wait for the GPU to write some specific value in a specific address to signal all data is there, then examine and display, and tell the GPU all clear, go ahead.

Now that our uncached buffers are in play, we just keep looping and checking whether the GPU has written the register values. When it does, the first thing we do is halt the wave by writing into the SQ_CMD register to allow us to do whatever with the wave without causing any issues, tho if we halt for too long, the GPU CP will reset the command queue and kill the process, but we can change that behaviour by adjusting lockup_timeout parameter of the amdgpu kernel module:


reg_sq_wave_hw_id1_t hw1 = { .raw = tma[2] };
reg_sq_wave_hw_id2_t hw2 = { .raw = tma[3] };

reg_sq_cmd_t halt_cmd = {
 .cmd  = 1,
 .mode = 1,
 .data = 1,
};

regs2_ioc_data_t ioc_data = {
 .use_srbm = false,
 .use_grbm = true,
};

dev_op_reg32(&amdgpu, REG_SQ_CMD, ioc_data, REG_OP_WRITE, &halt_cmd.raw);
gpu_is_halted = true;
From here on, we can do whatever with the data we have. All the data we need to build a proper debugger. We will come back to what to do with the data in a bit; let’s assume we did what was needed for now.

Now that we’re done with the CPU, we need to write to the first byte in our TMA buffer, since the trap handler checks for that, then resume the wave, and the trap handler should pick it up. We can resume by writing to the SQ_CMD register again:


halt_cmd.mode = 0;
dev_op_reg32(&amdgpu, REG_SQ_CMD, ioc_data, REG_OP_WRITE, &halt_cmd.raw);
gpu_is_halted = false;
Then the GPU should continue. We need to restore everything and return the program counter to the original address. Based on whether it’s a hardware trap or not, the program counter may point to the instruction before or the instruction itself. The ISA manual and Marcell Kiss’s posts explain that well, so refer to them.


RETURN:
 ;; extract the trap ID from ttmp1
 s_and_b32 ttmp9, ttmp1, PC_HI_TRAP_ID_MASK
 s_lshr_b32 ttmp9, ttmp9, PC_HI_TRAP_ID_SHIFT

 ;; if the trapID == 0, then this is a hardware trap,
 ;; we don't need to fix up the return address
 s_cmpk_eq_u32 ttmp9, 0
 s_cbranch_scc1 RETURN_FROM_NON_S_TRAP

 ;; restore PC
 ;; add 4 to the faulting address, with carry
 s_add_u32 ttmp0, ttmp0, 4
 s_addc_u32 ttmp1, ttmp1, 0

RETURN_FROM_NON_S_TRAP:
 s_load_dwordx4 s[0:3], ttmp[4:5], TMA_SREG_OFFSET glc dlc
 s_load_dword s4, ttmp[4:5], TMA_SREG_OFFSET + 16 glc dlc
 s_waitcnt lgkmcnt(0)

 s_mov_b64 exec, -1
 global_load_addtid_b32 v0, ttmp[4:5], offset:TMA_VREG_OFFSET        glc slc dlc
 global_load_addtid_b32 v1, ttmp[4:5], offset:TMA_VREG_OFFSET + 256  glc slc dlc
 global_load_addtid_b32 v2, ttmp[4:5], offset:TMA_VREG_OFFSET + 512  glc slc dlc
 global_load_addtid_b32 v3, ttmp[4:5], offset:TMA_VREG_OFFSET + 768  glc slc dlc
 global_load_addtid_b32 v4, ttmp[4:5], offset:TMA_VREG_OFFSET + 1024 glc slc dlc
 global_load_addtid_b32 v5, ttmp[4:5], offset:TMA_VREG_OFFSET + 1280 glc slc dlc
 global_load_addtid_b32 v6, ttmp[4:5], offset:TMA_VREG_OFFSET + 1536 glc slc dlc
 s_waitcnt vmcnt(0)

 ;; mask off non-address high bits from ttmp1
 s_and_b32 ttmp1, ttmp1, 0xffff

 ;; restore exec
 s_load_b64 vcc, ttmp[4:5], 2048 glc dlc
 s_load_b64 ttmp[2:3], ttmp[4:5], 2056 glc dlc
 s_waitcnt lgkmcnt(0)
 s_mov_b64 exec, ttmp[2:3]

 ;; restore STATUS.EXECZ, not writable by s_setreg_b32
 s_and_b64 exec, exec, exec

 ;; restore STATUS.VCCZ, not writable by s_setreg_b32
 s_and_b64 vcc, vcc, vcc

 ;; restore STATUS.SCC
 s_setreg_b32 hwreg(HW_REG_STATUS, 0, 1), ttmp8

 s_waitcnt vmcnt(0) lgkmcnt(0) expcnt(0)  ; Full pipeline flush
 ;; return from trap handler and restore STATUS.PRIV
 s_rfe_b64 [ttmp0, ttmp1]
SPIR-V
Now we can run compiled code directly, but we don’t want people to compile their code manually, then extract the text section, and give it to us. The plan is to take SPIR-V code, compile it correctly, then run it, or, even better, integrate with RADV and let RADV give us more information to work with.

My main plan was making like fork RADV and then add then make report for us the vulkan calls and then we can have a better view on the GPU work know the buffers/textures it’s using etc, This seems like a lot more work tho so I’ll keep it in mind but not doing that for now unless someone is willing to pay me for that ;).

For now, let’s just use RADV’s compiler ACO. Luckily, RADV has a null_winsys mode, aka it will not do actual work or open DRM files, just a fake Vulkan device, which is perfect for our case here, since we care about nothing other than just compiling code. We can enable it by setting the env var RADV_FORCE_FAMILY, then we just call what we need like this:


int32_t hdb_compile_spirv_to_bin(
  const void* spirv_binary,
  size_t size,
  hdb_shader_stage_t stage,
  hdb_shader_t* shader
) {
 setenv("RADV_FORCE_FAMILY", "navi31", 1);
 //  setenv("RADV_DEBUG", "nocache,noopt", 1);
 setenv("ACO_DEBUG", "nocache,noopt", 1);

 VkInstanceCreateInfo i_cinfo = {
  .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
  .pApplicationInfo =
    &(VkApplicationInfo){
      .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName   = "HDB Shader Compiler",
      .applicationVersion = 1,
      .pEngineName        = "HDB",
      .engineVersion      = 1,
      .apiVersion         = VK_API_VERSION_1_4,
    },
 };

 VkInstance vk_instance = {};
 radv_CreateInstance(&i_cinfo, NULL, &vk_instance);

 struct radv_instance* instance = radv_instance_from_handle(vk_instance);
 instance->debug_flags |=
   RADV_DEBUG_NIR_DEBUG_INFO | RADV_DEBUG_NO_CACHE | RADV_DEBUG_INFO;

 uint32_t         n       = 1;
 VkPhysicalDevice vk_pdev = {};
 instance->vk.dispatch_table.EnumeratePhysicalDevices(vk_instance, &n, &vk_pdev);

 struct radv_physical_device* pdev = radv_physical_device_from_handle(vk_pdev);
 pdev->use_llvm                    = false;

 VkDeviceCreateInfo d_cinfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
 VkDevice vk_dev = {};
 pdev->vk.dispatch_table.CreateDevice(vk_pdev, &d_cinfo, NULL, &vk_dev);

 struct radv_device* dev = radv_device_from_handle(vk_dev);

 struct radv_shader_stage radv_stage = {
  .spirv.data = spirv_binary,
  .spirv.size = size,
  .entrypoint = "main",
  .stage      = MESA_SHADER_COMPUTE,
  .layout = {
   .push_constant_size = 16,
  },
  .key = {
   .optimisations_disabled = true,
  },
 };

 struct radv_shader_binary* cs_bin = NULL;
 struct radv_shader*        cs_shader =
   radv_compile_cs(dev, NULL, &radv_stage, true, true, false, true, &cs_bin);

 *shader = (hdb_shader_t){
  .bin              = cs_shader->code,
  .bin_size         = cs_shader->code_size,
  .rsrc1            = cs_shader->config.rsrc1,
  .rsrc2            = cs_shader->config.rsrc2,
  .rsrc3            = cs_shader->config.rsrc3,
  .debug_info       = cs_shader->debug_info,
  .debug_info_count = cs_shader->debug_info_count,
 };

 return 0;
}
Now that we have a well-structured loop and communication between the GPU and the CPU, we can run SPIR-V binaries to some extent. Let’s see how we can make it an actual debugger.

An Actual Debugger
We talked earlier about CPs natively supporting context-switching, this appears to be compute spcific feature, which prevents from implementing it for other types of shaders, tho, it appears that mesh shaders and raytracing shaders are just compute shaders under the hood, which will allow us to use that functionality. For now debugging one wave feels enough, also we can moify the wave parameters to debug some specific indices.

Here’s some of the features

Breakpoints and Stepping
For stepping, we can use 2 bits: one in RSRC1 and the other in RSRC3. They’re DEBUG_MODE and TRAP_ON_START, respectively. The former enters the trap handler after each instruction, and the latter enters before the first instruction. This means we can automatically enable instruction-level stepping.

Regarding breakpoints, I haven’t implemented them, but they’re rather simple to implement here by us having the base address of the code buffer and knowing the size of each instruction; we can calculate the program counter location ahead and have a list of them available to the GPU, and we can do a binary search on the trap handler.

Source Code Line Mapping
The ACO shader compiler does generate instruction-level source code mapping, which is good enough for our purposes here. By taking the offset4 of the current program counter and indexing into the code buffer, we can retrieve the current instruction and disassemble it, as well as find the source code mapping from the debug info.

Address Watching aka Watchpoints
We can implement this by marking the GPU page as protected. On a GPU fault, we enter the trap handler, check whether it’s within the range of our buffers and textures, and then act accordingly. Also, looking at the registers, we can find these:


typedef union {
 struct {
  uint32_t addr: 16;
 };
 uint32_t raw;
} reg_sq_watch0_addr_h_t;

typedef union {
 struct {
  uint32_t __reserved_0 : 6;
  uint32_t addr: 26;
 };
 uint32_t raw;
} reg_sq_watch0_addr_l_t;
which suggests that the hardware already supports this natively, so we don’t even need to do that dance. It needs more investigation on my part, tho, since I didn’t implement this.

Variables Types and Names
This needs some serious plumbing, since we need to make NIR(Mesa’s intermediate representation) optimisation passes propagate debug info correctly. I already started on this here. Then we need to make ACO track variables and store the information.

Vulkan Integration
This requires ditching our simple UMD we made earlier and using RADV, which is what should happen eventually, then we have our custom driver maybe pause on before a specific frame, or get triggered by a key, and then ask before each dispatch if to attach to it or not, or something similar, since we have a full proper Vulkan implementation we already have all the information we would need like buffers, textures, push constants, types, variable names, .. etc, that would be a much better and more pleasant debugger to use.

Finally, here’s some live footage:


Bonus Round
Here is an incomplete user-mode page walking code for gfx11, aka rx7900xtx


typedef struct {
 u64 valid         : 1;  // 0
 u64 system        : 1;  // 1
 u64 coherent      : 1;  // 2
 u64 __reserved_0  : 3;  // 5
 u64 pte_base_addr : 42; // 47
 u64 pa_rsvd       : 4;  // 51
 u64 __reserved_1  : 2;  // 53
 u64 mall_reuse    : 2;  // 55
 u64 tfs_addr      : 1;  // 56
 u64 __reserved_2  : 1;  // 57
 u64 frag_size     : 5;  // 62
 u64 pte           : 1;  // 63
} pde_t;

typedef struct {
 u64 valid          : 1; // = pte_entry & 1;
 u64 system         : 1; // = (pte_entry >> 1) & 1;
 u64 coherent       : 1; // = (pte_entry >> 2) & 1;
 u64 tmz            : 1; // = (pte_entry >> 3) & 1;
 u64 execute        : 1; // = (pte_entry >> 4) & 1;
 u64 read           : 1; // = (pte_entry >> 5) & 1;
 u64 write          : 1; // = (pte_entry >> 6) & 1;
 u64 fragment       : 5; // = (pte_entry >> 7) & 0x1F;
 u64 page_base_addr : 36;
 u64 mtype          : 2; // = (pte_entry >> 48) & 3;
 u64 prt            : 1; // = (pte_entry >> 51) & 1;
 u64 software       : 2; // = (pte_entry >> 52) & 3;
 u64 pde            : 1; // = (pte_entry >> 54) & 1;
 u64 __reserved_0   : 1;
 u64 further        : 1; // = (pte_entry >> 56) & 1;
 u64 gcr            : 1; // = (pte_entry >> 57) & 1;
 u64 llc_noalloc    : 1; // = (pte_entry >> 58) & 1;
} pte_t;

static inline pde_t decode_pde(u64 pde_raw) {
 pde_t pde         = *((pde_t*)(&pde_raw));
 pde.pte_base_addr = (u64)pde.pte_base_addr << 6;
 return pde;
}

static inline pte_t decode_pte(u64 pde_raw) {
 pte_t pte          = *((pte_t*)(&pde_raw));
 pte.page_base_addr = (u64)pte.page_base_addr << 12;
 return pte;
}

static inline u64 log2_range_round_up(u64 s, u64 e) {
 u64 x = e - s - 1;
 return (x == 0 || x == 1) ? 1 : 64 - __builtin_clzll(x);
}

void dev_linear_vram(amdgpu_t* dev, u64 phy_addr, size_t size, void* buf) {
 HDB_ASSERT(!((phy_addr & 3) || (size & 3)), "Must be page aligned address and size");

 size_t offset = lseek(dev->vram_fd, phy_addr, SEEK_SET);
 HDB_ASSERT(offset == phy_addr, "Couldn't seek to the requested addr");

 offset = read(dev->vram_fd, buf, size);
 HDB_ASSERT(offset == size, "Couldn't read the full requested size");
}

void dev_decode(amdgpu_t* dev, u32 vmid, u64 va_addr) {
 reg_gcmc_vm_fb_location_base_t fb_base_reg   = { 0 };
 reg_gcmc_vm_fb_location_top_t  fb_top_reg    = { 0 };
 reg_gcmc_vm_fb_offset_t        fb_offset_reg = { 0 };

 regs2_ioc_data_t ioc_data = { 0 };
 dev_op_reg32(
   dev, REG_GCMC_VM_FB_LOCATION_BASE, ioc_data, REG_OP_READ, &fb_base_reg.raw);
 dev_op_reg32(dev, REG_GCMC_VM_FB_LOCATION_TOP, ioc_data, REG_OP_READ, &fb_top_reg.raw);
 dev_op_reg32(dev, REG_GCMC_VM_FB_OFFSET, ioc_data, REG_OP_READ, &fb_offset_reg.raw);

 u64 fb_offset = (u64)fb_offset_reg.fb_offset;

 // TODO(hadi): add zfb mode support
 bool zfb = fb_top_reg.fb_top + 1 < fb_base_reg.fb_base;
 HDB_ASSERT(!zfb, "ZFB mode is not implemented yet!");

 // printf(
 //   "fb base: 0x%x\nfb_top: 0x%x\nfb_offset: 0x%x\n",
 //   fb_base_reg.raw,
 //   fb_top_reg.raw,
 //   fb_offset_reg.raw);

 gc_11_reg_t pt_start_lo_id = { 0 };
 gc_11_reg_t pt_start_hi_id = { 0 };
 gc_11_reg_t pt_end_lo_id   = { 0 };
 gc_11_reg_t pt_end_hi_id   = { 0 };
 gc_11_reg_t pt_base_hi_id  = { 0 };
 gc_11_reg_t pt_base_lo_id  = { 0 };
 gc_11_reg_t ctx_cntl_id    = { 0 };

 switch (vmid) {
 case 0:
  pt_start_lo_id = REG_GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT0_CNTL;
  break;
 case 1:
  pt_start_lo_id = REG_GCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT1_CNTL;
  break;
 case 2:
  pt_start_lo_id = REG_GCVM_CONTEXT2_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT2_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT2_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT2_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT2_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT2_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT2_CNTL;
  break;
 case 3:
  pt_start_lo_id = REG_GCVM_CONTEXT3_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT3_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT3_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT3_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT3_CNTL;
  break;
 case 4:
  pt_start_lo_id = REG_GCVM_CONTEXT4_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT4_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT4_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT4_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT4_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT4_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT4_CNTL;
  break;
 case 5:
  pt_start_lo_id = REG_GCVM_CONTEXT5_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT5_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT5_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT5_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT5_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT5_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT5_CNTL;
  break;
 case 6:
  pt_start_lo_id = REG_GCVM_CONTEXT6_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT6_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT6_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT6_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT6_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT6_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT6_CNTL;
  break;
 case 7:
  pt_start_lo_id = REG_GCVM_CONTEXT7_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT7_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT7_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT7_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT7_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT7_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT7_CNTL;
  break;
 case 8:
  pt_start_lo_id = REG_GCVM_CONTEXT8_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT8_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT8_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT8_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT7_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT7_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT7_CNTL;
  break;
 case 9:
  pt_start_lo_id = REG_GCVM_CONTEXT9_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT9_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT9_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT9_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT7_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT7_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT7_CNTL;
  break;
 case 10:
  pt_start_lo_id = REG_GCVM_CONTEXT10_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT10_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT10_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT10_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT10_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT10_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT10_CNTL;
  break;
 case 11:
  pt_start_lo_id = REG_GCVM_CONTEXT11_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT11_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT11_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT11_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT11_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT11_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT11_CNTL;
  break;
 case 12:
  pt_start_lo_id = REG_GCVM_CONTEXT12_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT12_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT12_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT12_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT12_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT12_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT12_CNTL;
  break;
 case 13:
  pt_start_lo_id = REG_GCVM_CONTEXT13_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT13_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT13_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT13_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT13_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT13_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT13_CNTL;
  break;
 case 14:
  pt_start_lo_id = REG_GCVM_CONTEXT14_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT14_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT14_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT14_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT14_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT14_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT14_CNTL;
  break;
 case 15:
  pt_start_lo_id = REG_GCVM_CONTEXT15_PAGE_TABLE_START_ADDR_LO32;
  pt_start_hi_id = REG_GCVM_CONTEXT15_PAGE_TABLE_START_ADDR_HI32;
  pt_end_lo_id   = REG_GCVM_CONTEXT15_PAGE_TABLE_END_ADDR_LO32;
  pt_end_hi_id   = REG_GCVM_CONTEXT15_PAGE_TABLE_END_ADDR_HI32;
  pt_base_lo_id  = REG_GCVM_CONTEXT15_PAGE_TABLE_BASE_ADDR_LO32;
  pt_base_hi_id  = REG_GCVM_CONTEXT15_PAGE_TABLE_BASE_ADDR_HI32;
  ctx_cntl_id    = REG_GCVM_CONTEXT15_CNTL;
  break;
 default: HDB_ASSERT(false, "Out of range VMID 0-15 trying to access %u", vmid);
 }

 // all the types of the contexts are the same so will just use 0 but pass the correct
 // register enum to the read function
 reg_gcvm_context0_page_table_start_addr_lo32_t pt_start_lo = { 0 };
 reg_gcvm_context0_page_table_start_addr_hi32_t pt_start_hi = { 0 };
 reg_gcvm_context0_page_table_end_addr_lo32_t   pt_end_lo   = { 0 };
 reg_gcvm_context0_page_table_end_addr_hi32_t   pt_end_hi   = { 0 };
 reg_gcvm_context0_page_table_base_addr_lo32_t  pt_base_lo  = { 0 };
 reg_gcvm_context0_page_table_base_addr_hi32_t  pt_base_hi  = { 0 };
 reg_gcvm_context0_cntl_t                       ctx_cntl    = { 0 };

 dev_op_reg32(dev, pt_start_lo_id, ioc_data, REG_OP_READ, &pt_start_lo.raw);
 dev_op_reg32(dev, pt_start_hi_id, ioc_data, REG_OP_READ, &pt_start_hi.raw);
 dev_op_reg32(dev, pt_end_lo_id, ioc_data, REG_OP_READ, &pt_end_lo.raw);
 dev_op_reg32(dev, pt_end_hi_id, ioc_data, REG_OP_READ, &pt_end_hi.raw);
 dev_op_reg32(dev, pt_base_lo_id, ioc_data, REG_OP_READ, &pt_base_lo.raw);
 dev_op_reg32(dev, pt_base_hi_id, ioc_data, REG_OP_READ, &pt_base_hi.raw);
 dev_op_reg32(dev, ctx_cntl_id, ioc_data, REG_OP_READ, &ctx_cntl.raw);

 u64 pt_start_addr = ((u64)pt_start_lo.raw << 12) | ((u64)pt_start_hi.raw << 44);
 u64 pt_end_addr   = ((u64)pt_end_lo.raw << 12) | ((u64)pt_end_hi.raw << 44);
 u64 pt_base_addr  = ((u64)pt_base_lo.raw << 0) | ((u64)pt_base_hi.raw << 32);
 u32 pt_depth      = ctx_cntl.page_table_depth;
 u32 ptb_size      = ctx_cntl.page_table_block_size;

 HDB_ASSERT(pt_base_addr != 0xffffffffffffffffull, "Invalid page table base addr");

 printf(
   "\tPage Table Start: 0x%lx\n\tPage Table End: 0x%lx\n\tPage Table Base: "
   "0x%lx\n\tPage Table Depth: %u\n\tBlock Size: %u\n",
   pt_start_addr,
   pt_end_addr,
   pt_base_addr,
   pt_depth,
   ptb_size);

 // decode base PDB
 pde_t pde = decode_pde(pt_base_addr);
 pt_base_addr -= fb_offset * !pde.system; // substract only on vram

 u64 pt_last_byte_addr = pt_end_addr + 0xfff; // 0xfff is 1 page
 HDB_ASSERT(
   pt_start_addr <= va_addr || va_addr < pt_last_byte_addr,
   "Invalid virtual address outside the range of the root page table of this vm");

 va_addr -= pt_start_addr;
 //
 // Size of the first PDB depends on the total coverage of the
 // page table and the PAGE_TABLE_BLOCK_SIZE.
 // Entire table takes ceil(log2(total_vm_size)) bits
 // All PDBs except the first one take 9 bits each
 // The PTB covers at least 2 MiB (21 bits)
 // And PAGE_TABLE_BLOCK_SIZE is log2(num 2MiB ranges PTB covers)
 // As such, the formula for the size of the first PDB is:
 //                       PDB1, PDB0, etc.      PTB covers at least 2 MiB
 //                                        Block size can make it cover more
 //   total_vm_bits - (9 * num_middle_pdbs) - (page_table_block_size + 21)
 //
 // we need the total range range here not the last byte addr like above
 u32 total_vaddr_bits = log2_range_round_up(pt_start_addr, pt_end_addr + 0x1000);

 u32 total_pdb_bits = total_vaddr_bits;
 // substract everything from the va_addr to leave just the pdb bits
 total_pdb_bits -= 9 * (pt_depth - 1); // middle PDBs each is 9 bits
 total_pdb_bits -= (ptb_size + 21);    // at least 2mb(21) bits + ptb_size

 // u64 va_mask = (1ull << total_pdb_bits) - 1;
 // va_mask <<= (total_vaddr_bits - total_pdb_bits);

 // pde_t pdes[8]  = { 0 };
 // u32   curr_pde = 0;
 // u64   pde_addr = 0;
 // u64  loop_pde = pt_base_addr;

 if (pt_depth == 0) { HDB_ASSERT(false, "DEPTH = 0 is not implemented yet"); }

 pde_t curr_pde    = pde;
 u64   entry_bits  = 0;
 s32   curr_depth  = pt_depth;
 bool  pde0_is_pte = false;
 // walk all middle PDEs
 while (curr_depth > 0) {
  // printf("pde(%u):0x%lx \n", curr_depth, curr_pde.pte_base_addr);
  u64 next_entry_addr = 0;

  u32 shift_amount = total_vaddr_bits;
  shift_amount -= total_pdb_bits;
  // for each pdb shift 9 more
  shift_amount -= ((pt_depth - curr_depth) * 9);

  // shift address and mask out unused bits
  u64 next_pde_idx = va_addr >> shift_amount;
  next_pde_idx &= 0x1ff;

  // if on vram we need to apply this offset
  if (!curr_pde.system) curr_pde.pte_base_addr -= fb_offset;

  next_entry_addr = curr_pde.pte_base_addr + next_pde_idx * 8;
  curr_depth--;

  if (!curr_pde.system) {
   dev_linear_vram(dev, next_entry_addr, 8, &entry_bits);
   curr_pde = decode_pde(entry_bits);
   printf(
     "\tPage Dir Entry(%u):\n\t  Addr:0x%lx\n\t  Base: 0x%lx\n\n\t        ↓\n\n",
     curr_depth,
     next_entry_addr,
     curr_pde.pte_base_addr);
  } else {
   HDB_ASSERT(false, "GTT physical memory access is not implemented yet");
  }

  if (!curr_pde.valid) { break; }

  if (curr_pde.pte) {
   // PDB0 can act as a pte
   // also I'm making an assumption here that UMRs code doesn't make
   // that the the PDB0 as PTE path can't have the further bit set
   pde0_is_pte = true;
   break;
  }
 }

 if (pde0_is_pte) { HDB_ASSERT(false, "PDE0 as PTE is not implemented yet"); }

 // page_table_block_size is the number of 2MiB regions covered by a PTB
 // If we set it to 0, then PTB cover 2 MiB
 // If it's 9 PTB cover 1024 MiB
 // pde0_block_fragment_size tells us how many 4 KiB regions each PTE covers
 // If it's 0 PTEs cover 4 KiB
 // If it's 9 PTEs cover 2 MiB
 // So the number of PTEs in a PTB is 2^(9+ptbs-pbfs)
 //
 // size here is actually the log_2 of the size
 u32 pte_page_size  = curr_pde.frag_size;
 u32 ptes_per_ptb   = 9 + ptb_size - pte_page_size;
 u64 pte_index_mask = (1ul << ptes_per_ptb) - 1;

 u32 pte_bits_count   = pte_page_size + 12;
 u64 page_offset_mask = (1ul << pte_bits_count) - 1; // minimum of 12

 u64 pte_index = (va_addr >> pte_bits_count) & pte_index_mask;
 u64 pte_addr  = curr_pde.pte_base_addr + pte_index * 8;

 pte_t pte = { 0 };
 if (!curr_pde.system) {
  dev_linear_vram(dev, pte_addr, 8, &entry_bits);
  pte = decode_pte(entry_bits);

  printf("\tPage Table Entry: 0x%lx\n", pte.page_base_addr);
 } else {
  HDB_ASSERT(false, "GTT physical memory access is not implemented yet");
 }

 if (pte.further) { HDB_ASSERT(false, "PTE as PDE walking is not implemented yet"); }
 if (!pte.system) pte.page_base_addr -= fb_offset;

 u64 offset_in_page = va_addr & page_offset_mask;
 u64 physical_addr  = pte.page_base_addr + offset_in_page;
 printf("\tFinal Physical Address: 0x%lx\n", physical_addr);
}
Footnotes
Other processes need to have a s_trap instruction or have trap on exception flags set, which is not true for most normal GPU processes. ↩

Available since RDNA3, if I’m not mistaken. ↩

VGPRs are unique per thread, and SGPRs are unique per wave ↩

We can get that by subtracting the current program counter from the address of the code buffer. ↩

© 2026 Abdelhadi
I'm Actively looking for new opportunities, Contact Me