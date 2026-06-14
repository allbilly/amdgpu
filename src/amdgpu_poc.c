#define _GNU_SOURCE

#include "common.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <xf86drm.h>

#ifndef AMDGPU_TIMEOUT_INFINITE
#define AMDGPU_TIMEOUT_INFINITE 0xffffffffffffffffull
#endif

#define PKT_TYPE3 3u
#define PKT3(op, count, predicate) \
  ((PKT_TYPE3 << 30) | (((count) & 0x3fff) << 16) | (((op) & 0xff) << 8) | \
   ((predicate) & 1u))
#define PKT3_SHADER_TYPE_S(x) (((x) & 1u) << 1)

#define PKT3_SET_SH_REG 0x76u
#define PKT3_DISPATCH_DIRECT 0x15u

#define SI_SH_REG_OFFSET 0x0000b000u
#define SI_SH_REG_END 0x0000c000u

#define REG_COMPUTE_NUM_THREAD_X 0x0000b81cu
#define REG_COMPUTE_NUM_THREAD_Y 0x0000b820u
#define REG_COMPUTE_NUM_THREAD_Z 0x0000b824u
#define REG_COMPUTE_START_X 0x0000b810u
#define REG_COMPUTE_START_Y 0x0000b814u
#define REG_COMPUTE_START_Z 0x0000b818u
#define REG_COMPUTE_PGM_LO 0x0000b830u
#define REG_COMPUTE_PGM_HI 0x0000b834u
#define REG_COMPUTE_PGM_RSRC1 0x0000b848u
#define REG_COMPUTE_PGM_RSRC2 0x0000b84cu
#define REG_COMPUTE_RESOURCE_LIMITS 0x0000b854u
#define REG_COMPUTE_PGM_RSRC3 0x0000b858u
#define REG_COMPUTE_TMPRING_SIZE 0x0000b860u
#define REG_COMPUTE_USER_DATA_0 0x0000b900u

#define DISPATCH_INITIATOR_COMPUTE_SHADER_EN (1u << 0)
#define DISPATCH_INITIATOR_FORCE_START_AT_000 (1u << 2)

typedef struct {
  u32 *data;
  size_t count;
  size_t capacity;
} pm4_builder;

typedef struct {
  amdgpu_bo_handle handle;
  amdgpu_va_handle va_handle;
  u64 va;
  size_t size;
  void *cpu;
} gpu_bo;

typedef struct {
  int fd;
  u32 major;
  u32 minor;
  amdgpu_device_handle device;
  amdgpu_context_handle context;
} gpu_device;

typedef struct {
  const char *card;
  const char *shader_path;
  u32 rsrc1;
  u32 rsrc2;
  u32 rsrc3;
  u32 groups_x;
  u32 groups_y;
  u32 groups_z;
  u32 threads_x;
  u32 threads_y;
  u32 threads_z;
  u32 output_bytes;
  u64 timeout_ns;
  u32 init_output[16];
  u32 init_output_count;
  bool dry_run;
  bool pass_output_va;
  bool sdma_readback;
} options;

static const u8 embedded_nop_shader[] = {
  0x00, 0x00, 0x80, 0xbf, /* s_nop 0 */
  0x00, 0x00, 0x81, 0xbf, /* s_endpgm */
};

static void pm4_push(pm4_builder *pm4, u32 value) {
  if (pm4->count == pm4->capacity) {
    size_t next_capacity = pm4->capacity ? pm4->capacity * 2 : 64;
    u32 *next = realloc(pm4->data, next_capacity * sizeof(*next));
    if (!next) die_errno("realloc pm4");
    pm4->data = next;
    pm4->capacity = next_capacity;
  }

  pm4->data[pm4->count++] = value;
}

static void pm4_set_sh_reg(pm4_builder *pm4, u32 reg, u32 value) {
  if (reg < SI_SH_REG_OFFSET || reg >= SI_SH_REG_END) {
    dief("shader register 0x%x is outside [0x%x, 0x%x)", reg, SI_SH_REG_OFFSET, SI_SH_REG_END);
  }

  pm4_push(pm4, PKT3(PKT3_SET_SH_REG, 1, 0));
  pm4_push(pm4, (reg - SI_SH_REG_OFFSET) / 4);
  pm4_push(pm4, value);
}

static void pm4_dispatch_direct(pm4_builder *pm4, u32 x, u32 y, u32 z, u32 initiator) {
  pm4_push(pm4, PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | PKT3_SHADER_TYPE_S(1));
  pm4_push(pm4, x);
  pm4_push(pm4, y);
  pm4_push(pm4, z);
  pm4_push(pm4, initiator);
}

static u32 parse_u32(const char *value, const char *name) {
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(value, &end, 0);
  if (errno || !end || *end || parsed > UINT32_MAX) dief("invalid %s: %s", name, value);
  return (u32)parsed;
}

static u64 parse_timeout_ms(const char *value) {
  u32 ms = parse_u32(value, "timeout-ms");
  return (u64)ms * 1000ull * 1000ull;
}

static void usage(FILE *stream) {
  fprintf(stream,
          "usage: amdgpu-poc [options]\n"
          "\n"
          "Runs a tiny PM4 compute dispatch through libdrm_amdgpu. If --shader is not\n"
          "provided, an embedded 's_nop 0; s_endpgm' shader is used.\n"
          "\n"
          "options:\n"
          "  --card PATH          DRM render/card node (default: /dev/dri/renderD128)\n"
          "  --shader PATH        raw .text shader binary to upload\n"
          "  --rsrc1 HEX          COMPUTE_PGM_RSRC1 (default: 0x00ac0000)\n"
          "  --rsrc2 HEX          COMPUTE_PGM_RSRC2 (default: 0)\n"
          "  --rsrc3 HEX          COMPUTE_PGM_RSRC3 (default: 0)\n"
          "  --groups-x N         DISPATCH_DIRECT x groups (default: 1)\n"
          "  --groups-y N         DISPATCH_DIRECT y groups (default: 1)\n"
          "  --groups-z N         DISPATCH_DIRECT z groups (default: 1)\n"
          "  --threads-x N        COMPUTE_NUM_THREAD_X (default: 1)\n"
          "  --threads-y N        COMPUTE_NUM_THREAD_Y (default: 1)\n"
          "  --threads-z N        COMPUTE_NUM_THREAD_Z (default: 1)\n"
          "  --output-bytes N     allocate and dump a GTT output BO (default: 4096)\n"
          "  --init-output-u32 V  initialize next output dword before dispatch\n"
          "  --pass-output-va    pass output BO VA in s[0:1] via COMPUTE_USER_DATA_0/1\n"
          "  --sdma-readback     copy output BO to a second BO using SDMA before dump\n"
          "  --timeout-ms N       fence wait timeout in ms (default: 5000)\n"
          "  --dry-run            print PM4 dwords without submitting\n"
          "  -h, --help           show this help\n");
}

static options parse_options(int argc, char **argv) {
  options opt = {
    .card = "/dev/dri/renderD128",
    .shader_path = NULL,
    .rsrc1 = 0x00ac0000u,
    .rsrc2 = 0,
    .rsrc3 = 0,
    .groups_x = 1,
    .groups_y = 1,
    .groups_z = 1,
    .threads_x = 1,
    .threads_y = 1,
    .threads_z = 1,
    .output_bytes = 4096,
    .timeout_ns = 5ull * 1000ull * 1000ull * 1000ull,
    .init_output_count = 0,
    .dry_run = false,
    .pass_output_va = false,
    .sdma_readback = false,
  };

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      usage(stdout);
      exit(0);
    } else if (!strcmp(arg, "--dry-run")) {
      opt.dry_run = true;
    } else if (!strcmp(arg, "--pass-output-va")) {
      opt.pass_output_va = true;
    } else if (!strcmp(arg, "--sdma-readback")) {
      opt.sdma_readback = true;
    } else if (!next) {
      dief("missing value for %s", arg);
    } else if (!strcmp(arg, "--card")) {
      opt.card = next;
      i++;
    } else if (!strcmp(arg, "--shader")) {
      opt.shader_path = next;
      i++;
    } else if (!strcmp(arg, "--rsrc1")) {
      opt.rsrc1 = parse_u32(next, "rsrc1");
      i++;
    } else if (!strcmp(arg, "--rsrc2")) {
      opt.rsrc2 = parse_u32(next, "rsrc2");
      i++;
    } else if (!strcmp(arg, "--rsrc3")) {
      opt.rsrc3 = parse_u32(next, "rsrc3");
      i++;
    } else if (!strcmp(arg, "--groups-x")) {
      opt.groups_x = parse_u32(next, "groups-x");
      i++;
    } else if (!strcmp(arg, "--groups-y")) {
      opt.groups_y = parse_u32(next, "groups-y");
      i++;
    } else if (!strcmp(arg, "--groups-z")) {
      opt.groups_z = parse_u32(next, "groups-z");
      i++;
    } else if (!strcmp(arg, "--threads-x")) {
      opt.threads_x = parse_u32(next, "threads-x");
      i++;
    } else if (!strcmp(arg, "--threads-y")) {
      opt.threads_y = parse_u32(next, "threads-y");
      i++;
    } else if (!strcmp(arg, "--threads-z")) {
      opt.threads_z = parse_u32(next, "threads-z");
      i++;
    } else if (!strcmp(arg, "--output-bytes")) {
      opt.output_bytes = parse_u32(next, "output-bytes");
      i++;
    } else if (!strcmp(arg, "--init-output-u32")) {
      if (opt.init_output_count == 16) dief("too many --init-output-u32 values");
      opt.init_output[opt.init_output_count++] = parse_u32(next, "init-output-u32");
      i++;
    } else if (!strcmp(arg, "--timeout-ms")) {
      opt.timeout_ns = parse_timeout_ms(next);
      i++;
    } else {
      dief("unknown option: %s", arg);
    }
  }

  if (!opt.groups_x || !opt.groups_y || !opt.groups_z) dief("dispatch groups must be non-zero");
  if (!opt.threads_x || !opt.threads_y || !opt.threads_z) dief("thread counts must be non-zero");
  if (!opt.output_bytes) opt.output_bytes = 4;

  return opt;
}

static u8 *read_whole_file(const char *path, size_t *size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) die_errno(path);

  struct stat st;
  if (fstat(fd, &st) < 0) die_errno("fstat shader");
  if (st.st_size <= 0) dief("shader is empty: %s", path);

  u8 *data = malloc((size_t)st.st_size);
  if (!data) die_errno("malloc shader");

  size_t off = 0;
  while (off < (size_t)st.st_size) {
    ssize_t got = read(fd, data + off, (size_t)st.st_size - off);
    if (got < 0) die_errno("read shader");
    if (!got) dief("short read from %s", path);
    off += (size_t)got;
  }

  close(fd);
  *size = (size_t)st.st_size;
  return data;
}

static void gpu_open(gpu_device *gpu, const char *path) {
  gpu->fd = open(path, O_RDWR | O_CLOEXEC);
  if (gpu->fd < 0) die_errno(path);

  int ret = amdgpu_device_initialize(gpu->fd, &gpu->major, &gpu->minor, &gpu->device);
  check_ret(ret, "amdgpu_device_initialize");

  ret = amdgpu_cs_ctx_create(gpu->device, &gpu->context);
  check_ret(ret, "amdgpu_cs_ctx_create");
}

static void gpu_close(gpu_device *gpu) {
  if (gpu->context) amdgpu_cs_ctx_free(gpu->context);
  if (gpu->device) amdgpu_device_deinitialize(gpu->device);
  if (gpu->fd >= 0) close(gpu->fd);
}

static void bo_alloc(gpu_device *gpu, size_t requested_size, u32 domain, bool uncached, gpu_bo *bo) {
  memset(bo, 0, sizeof(*bo));

  size_t size = (size_t)align_up_u64(requested_size, 4096);
  u64 flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

#ifdef AMDGPU_GEM_CREATE_VRAM_CLEARED
  flags |= AMDGPU_GEM_CREATE_VRAM_CLEARED;
#endif
#ifdef AMDGPU_GEM_CREATE_VM_ALWAYS_VALID
  flags |= AMDGPU_GEM_CREATE_VM_ALWAYS_VALID;
#endif
#ifdef AMDGPU_GEM_CREATE_CPU_GTT_USWC
  if (uncached && domain == AMDGPU_GEM_DOMAIN_GTT) flags |= AMDGPU_GEM_CREATE_CPU_GTT_USWC;
#else
  (void)uncached;
#endif

  struct amdgpu_bo_alloc_request req = {
    .alloc_size = size,
    .phys_alignment = 4096,
    .preferred_heap = domain,
    .flags = flags,
  };

  int ret = amdgpu_bo_alloc(gpu->device, &req, &bo->handle);
  check_ret(ret, "amdgpu_bo_alloc");

  ret = amdgpu_va_range_alloc(gpu->device, amdgpu_gpu_va_range_general, size, 4096, 0, &bo->va,
                              &bo->va_handle, 0);
  check_ret(ret, "amdgpu_va_range_alloc");

  u64 map_flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
#ifdef AMDGPU_VM_MTYPE_UC
  if (uncached) map_flags |= AMDGPU_VM_MTYPE_UC;
#endif
#ifdef AMDGPU_VM_PAGE_NOALLOC
  if (uncached) map_flags |= AMDGPU_VM_PAGE_NOALLOC;
#endif

#ifdef DRM_AMDGPU_GEM_VA
  u32 kms_handle = 0;
  ret = amdgpu_bo_export(bo->handle, amdgpu_bo_handle_type_kms, &kms_handle);
  check_ret(ret, "amdgpu_bo_export(kms)");

  struct drm_amdgpu_gem_va va = {
    .handle = kms_handle,
    .operation = AMDGPU_VA_OP_MAP,
    .flags = map_flags,
    .va_address = bo->va,
    .offset_in_bo = 0,
    .map_size = size,
  };

  ret = drmCommandWriteRead(gpu->fd, DRM_AMDGPU_GEM_VA, &va, sizeof(va));
  check_ret(ret, "DRM_AMDGPU_GEM_VA map");
#else
  ret = amdgpu_bo_va_op(bo->handle, 0, size, bo->va, map_flags, AMDGPU_VA_OP_MAP);
  check_ret(ret, "amdgpu_bo_va_op map");
#endif

  ret = amdgpu_bo_cpu_map(bo->handle, &bo->cpu);
  check_ret(ret, "amdgpu_bo_cpu_map");
  memset(bo->cpu, 0, size);
  bo->size = size;
}

static void bo_free(gpu_bo *bo) {
  if (!bo->handle) return;
  if (bo->cpu) amdgpu_bo_cpu_unmap(bo->handle);
  if (bo->va) amdgpu_bo_va_op(bo->handle, 0, bo->size, bo->va, 0, AMDGPU_VA_OP_UNMAP);
  if (bo->va_handle) amdgpu_va_range_free(bo->va_handle);
  amdgpu_bo_free(bo->handle);
  memset(bo, 0, sizeof(*bo));
}

static void bo_upload(gpu_bo *bo, const void *data, size_t size) {
  if (size > bo->size) dief("upload size %zu exceeds BO size %zu", size, bo->size);
  memcpy(bo->cpu, data, size);
}

static void build_dispatch_pm4(pm4_builder *pm4, const gpu_bo *code, const gpu_bo *output,
                               const options *opt) {
  u64 pgm = code->va >> 8;
  u32 rsrc2 = opt->rsrc2;

  if (opt->pass_output_va) {
    rsrc2 |= 2u << 1;
    pm4_set_sh_reg(pm4, REG_COMPUTE_USER_DATA_0, (u32)output->va);
    pm4_set_sh_reg(pm4, REG_COMPUTE_USER_DATA_0 + 4, (u32)(output->va >> 32));
  }

  pm4_set_sh_reg(pm4, REG_COMPUTE_PGM_LO, (u32)pgm);
  pm4_set_sh_reg(pm4, REG_COMPUTE_PGM_HI, (u32)(pgm >> 32));
  pm4_set_sh_reg(pm4, REG_COMPUTE_PGM_RSRC1, opt->rsrc1);
  pm4_set_sh_reg(pm4, REG_COMPUTE_PGM_RSRC2, rsrc2);
  pm4_set_sh_reg(pm4, REG_COMPUTE_PGM_RSRC3, opt->rsrc3);
  pm4_set_sh_reg(pm4, REG_COMPUTE_RESOURCE_LIMITS, 0);
  pm4_set_sh_reg(pm4, REG_COMPUTE_TMPRING_SIZE, 0);
  pm4_set_sh_reg(pm4, REG_COMPUTE_START_X, 0);
  pm4_set_sh_reg(pm4, REG_COMPUTE_START_Y, 0);
  pm4_set_sh_reg(pm4, REG_COMPUTE_START_Z, 0);
  pm4_set_sh_reg(pm4, REG_COMPUTE_NUM_THREAD_X, opt->threads_x);
  pm4_set_sh_reg(pm4, REG_COMPUTE_NUM_THREAD_Y, opt->threads_y);
  pm4_set_sh_reg(pm4, REG_COMPUTE_NUM_THREAD_Z, opt->threads_z);
  pm4_dispatch_direct(pm4, opt->groups_x, opt->groups_y, opt->groups_z,
                      DISPATCH_INITIATOR_COMPUTE_SHADER_EN | DISPATCH_INITIATOR_FORCE_START_AT_000);
}

static void dump_pm4(const pm4_builder *pm4) {
  for (size_t i = 0; i < pm4->count; i++) printf("%04zu: 0x%08x\n", i, pm4->data[i]);
}

static void dump_bytes(const char *label, const void *data, size_t size) {
  const u8 *bytes = data;
  printf("%s (%zu bytes):\n", label, size);
  for (size_t i = 0; i < size; i++) {
    if ((i % 16) == 0) printf("  %04zx:", i);
    printf(" %02x", bytes[i]);
    if ((i % 16) == 15 || i + 1 == size) printf("\n");
  }
}

static void submit_pm4(gpu_device *gpu, pm4_builder *pm4, gpu_bo *code, gpu_bo *output,
                       const options *opt) {
  gpu_bo ib;
  bo_alloc(gpu, pm4->count * sizeof(u32), AMDGPU_GEM_DOMAIN_GTT, false, &ib);
  bo_upload(&ib, pm4->data, pm4->count * sizeof(u32));

  amdgpu_bo_handle handles[3] = {ib.handle, code->handle, output->handle};
  amdgpu_bo_list_handle bo_list = NULL;
  int ret = amdgpu_bo_list_create(gpu->device, 3, handles, NULL, &bo_list);
  check_ret(ret, "amdgpu_bo_list_create");

  struct amdgpu_cs_ib_info ib_info = {
    .flags = 0,
    .ib_mc_address = ib.va,
    .size = (u32)pm4->count,
  };

  struct amdgpu_cs_request req = {
    .ip_type = AMDGPU_HW_IP_COMPUTE,
    .ip_instance = 0,
    .ring = 0,
    .resources = bo_list,
    .number_of_ibs = 1,
    .ibs = &ib_info,
  };

  ret = amdgpu_cs_submit(gpu->context, 0, &req, 1);
  check_ret(ret, "amdgpu_cs_submit");

  struct amdgpu_cs_fence fence = {
    .context = gpu->context,
    .ip_type = AMDGPU_HW_IP_COMPUTE,
    .ip_instance = 0,
    .ring = 0,
    .fence = req.seq_no,
  };

  u32 expired = 0;
  ret = amdgpu_cs_query_fence_status(&fence, opt->timeout_ns, 0, &expired);
  check_ret(ret, "amdgpu_cs_query_fence_status");
  if (!expired) dief("GPU fence timed out after %llu ns", (unsigned long long)opt->timeout_ns);

  amdgpu_bo_list_destroy(bo_list);
  bo_free(&ib);
}

static void submit_sdma_copy(gpu_device *gpu, const gpu_bo *dst, const gpu_bo *src, u32 bytes,
                             u64 timeout_ns) {
  if (!bytes) return;
  if (bytes > 0x3fffffu) dief("SDMA copy too large for one packet: %u", bytes);

  u32 packet[7] = {
    0x00000001u,      /* SDMA_OP_COPY, SUBOP_COPY_LINEAR */
    bytes - 1u,       /* byte count minus one */
    0x00000000u,
    (u32)src->va,
    (u32)(src->va >> 32),
    (u32)dst->va,
    (u32)(dst->va >> 32),
  };

  gpu_bo ib;
  bo_alloc(gpu, sizeof(packet), AMDGPU_GEM_DOMAIN_GTT, false, &ib);
  bo_upload(&ib, packet, sizeof(packet));

  amdgpu_bo_handle handles[3] = {ib.handle, src->handle, dst->handle};
  amdgpu_bo_list_handle bo_list = NULL;
  int ret = amdgpu_bo_list_create(gpu->device, 3, handles, NULL, &bo_list);
  check_ret(ret, "amdgpu_bo_list_create(SDMA)");

  struct amdgpu_cs_ib_info ib_info = {
    .flags = 0,
    .ib_mc_address = ib.va,
    .size = (u32)(sizeof(packet) / sizeof(packet[0])),
  };

  struct amdgpu_cs_request req = {
    .ip_type = AMDGPU_HW_IP_DMA,
    .ip_instance = 0,
    .ring = 0,
    .resources = bo_list,
    .number_of_ibs = 1,
    .ibs = &ib_info,
  };

  ret = amdgpu_cs_submit(gpu->context, 0, &req, 1);
  check_ret(ret, "amdgpu_cs_submit(SDMA)");

  struct amdgpu_cs_fence fence = {
    .context = gpu->context,
    .ip_type = AMDGPU_HW_IP_DMA,
    .ip_instance = 0,
    .ring = 0,
    .fence = req.seq_no,
  };

  u32 expired = 0;
  ret = amdgpu_cs_query_fence_status(&fence, timeout_ns, 0, &expired);
  check_ret(ret, "amdgpu_cs_query_fence_status(SDMA)");
  if (!expired) dief("SDMA fence timed out after %llu ns", (unsigned long long)timeout_ns);

  amdgpu_bo_list_destroy(bo_list);
  bo_free(&ib);
}

int main(int argc, char **argv) {
  options opt = parse_options(argc, argv);

  size_t shader_size = sizeof(embedded_nop_shader);
  u8 *shader_file = NULL;
  const u8 *shader = embedded_nop_shader;
  if (opt.shader_path) {
    shader_file = read_whole_file(opt.shader_path, &shader_size);
    shader = shader_file;
  }

  gpu_device gpu = {.fd = -1};
  gpu_bo code = {0};
  gpu_bo output = {0};
  gpu_bo readback = {0};
  pm4_builder pm4 = {0};

  gpu_open(&gpu, opt.card);
  bo_alloc(&gpu, shader_size, AMDGPU_GEM_DOMAIN_VRAM, false, &code);
  bo_alloc(&gpu, opt.output_bytes, AMDGPU_GEM_DOMAIN_GTT, false, &output);
  if (opt.sdma_readback) bo_alloc(&gpu, opt.output_bytes, AMDGPU_GEM_DOMAIN_GTT, false, &readback);
  if ((size_t)opt.init_output_count * sizeof(u32) > output.size) dief("initial output values exceed output BO size");
  memcpy(output.cpu, opt.init_output, (size_t)opt.init_output_count * sizeof(u32));
  bo_upload(&code, shader, shader_size);

  build_dispatch_pm4(&pm4, &code, &output, &opt);

  printf("device initialized: amdgpu libdrm %u.%u\n", gpu.major, gpu.minor);
  printf("code BO:   va=0x%llx size=%zu shader=%zu bytes\n", (unsigned long long)code.va, code.size,
         shader_size);
  printf("output BO: va=0x%llx size=%zu\n", (unsigned long long)output.va, output.size);
  printf("rsrc: rsrc1=0x%08x rsrc2=0x%08x rsrc3=0x%08x\n", opt.rsrc1,
         opt.pass_output_va ? (opt.rsrc2 | (2u << 1)) : opt.rsrc2, opt.rsrc3);
  if (opt.pass_output_va) printf("user data: s[0:1] = output BO VA\n");

  if (opt.dry_run) {
    dump_pm4(&pm4);
  } else {
    submit_pm4(&gpu, &pm4, &code, &output, &opt);
    if (opt.sdma_readback) {
      submit_sdma_copy(&gpu, &readback, &output, opt.output_bytes, opt.timeout_ns);
      dump_bytes("sdma-readback", readback.cpu, opt.output_bytes < 256 ? opt.output_bytes : 256);
    } else {
      dump_bytes("output", output.cpu, opt.output_bytes < 256 ? opt.output_bytes : 256);
    }
  }

  free(pm4.data);
  bo_free(&readback);
  bo_free(&output);
  bo_free(&code);
  gpu_close(&gpu);
  free(shader_file);
  return 0;
}
