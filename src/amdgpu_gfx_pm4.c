#define _GNU_SOURCE

#include "common.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  u32 *data;
  size_t count;
  size_t capacity;
} dwords;

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
  const char *pm4_path;
  u64 timeout_ns;
  bool submit;
} options;

static void usage(FILE *stream) {
  fprintf(stream,
          "usage: amdgpu-gfx-pm4 --pm4 FILE [--submit] [options]\n"
          "\n"
          "Loads text PM4 dwords and optionally submits them to AMDGPU_HW_IP_GFX.\n"
          "FILE format is one 32-bit hex/decimal dword per line; blank lines and\n"
          "lines beginning with # are ignored. Without --submit, the stream is only\n"
          "validated and dumped.\n"
          "\n"
          "options:\n"
          "  --card PATH       DRM render/card node (default: /dev/dri/renderD128)\n"
          "  --pm4 FILE        text file containing PM4 dwords\n"
          "  --timeout-ms N    fence wait timeout in ms (default: 5000)\n"
          "  --submit          actually submit to the GFX ring\n"
          "  -h, --help        show this help\n");
}

static u32 parse_u32_arg(const char *value, const char *name) {
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(value, &end, 0);
  if (errno || !end || *end || parsed > UINT32_MAX) dief("invalid %s: %s", name, value);
  return (u32)parsed;
}

static options parse_options(int argc, char **argv) {
  options opt = {
    .card = "/dev/dri/renderD128",
    .pm4_path = NULL,
    .timeout_ns = 5ull * 1000ull * 1000ull * 1000ull,
    .submit = false,
  };

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      usage(stdout);
      exit(0);
    } else if (!strcmp(arg, "--submit")) {
      opt.submit = true;
    } else if (!next) {
      dief("missing value for %s", arg);
    } else if (!strcmp(arg, "--card")) {
      opt.card = next;
      i++;
    } else if (!strcmp(arg, "--pm4")) {
      opt.pm4_path = next;
      i++;
    } else if (!strcmp(arg, "--timeout-ms")) {
      opt.timeout_ns = (u64)parse_u32_arg(next, "timeout-ms") * 1000ull * 1000ull;
      i++;
    } else {
      dief("unknown option: %s", arg);
    }
  }

  if (!opt.pm4_path) dief("missing --pm4 FILE");
  return opt;
}

static void push_dword(dwords *dw, u32 value) {
  if (dw->count == dw->capacity) {
    size_t next_capacity = dw->capacity ? dw->capacity * 2 : 256;
    u32 *next = realloc(dw->data, next_capacity * sizeof(*next));
    if (!next) die_errno("realloc pm4");
    dw->data = next;
    dw->capacity = next_capacity;
  }
  dw->data[dw->count++] = value;
}

static dwords read_pm4_text(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file) die_errno(path);

  dwords dw = {0};
  char *line = NULL;
  size_t cap = 0;
  unsigned line_no = 0;
  while (getline(&line, &cap, file) >= 0) {
    line_no++;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '#') continue;

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(p, &end, 0);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (errno || !end || *end || value > UINT32_MAX) {
      dief("%s:%u: invalid dword", path, line_no);
    }
    push_dword(&dw, (u32)value);
  }
  free(line);
  fclose(file);

  if (!dw.count) dief("%s: no PM4 dwords", path);
  return dw;
}

static void dump_pm4(const dwords *dw) {
  for (size_t i = 0; i < dw->count; i++) printf("%04zu: 0x%08x\n", i, dw->data[i]);
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

static void bo_alloc(gpu_device *gpu, size_t requested_size, gpu_bo *bo) {
  memset(bo, 0, sizeof(*bo));
  size_t size = (size_t)align_up_u64(requested_size, 4096);
  struct amdgpu_bo_alloc_request req = {
    .alloc_size = size,
    .phys_alignment = 4096,
    .preferred_heap = AMDGPU_GEM_DOMAIN_GTT,
    .flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
  };

  int ret = amdgpu_bo_alloc(gpu->device, &req, &bo->handle);
  check_ret(ret, "amdgpu_bo_alloc");
  ret = amdgpu_va_range_alloc(gpu->device, amdgpu_gpu_va_range_general, size, 4096, 0, &bo->va,
                              &bo->va_handle, 0);
  check_ret(ret, "amdgpu_va_range_alloc");
  ret = amdgpu_bo_va_op(bo->handle, 0, size, bo->va,
                        AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE,
                        AMDGPU_VA_OP_MAP);
  check_ret(ret, "amdgpu_bo_va_op map");
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
}

static void submit_gfx(gpu_device *gpu, const dwords *dw, const options *opt) {
  gpu_bo ib = {0};
  bo_alloc(gpu, dw->count * sizeof(u32), &ib);
  memcpy(ib.cpu, dw->data, dw->count * sizeof(u32));

  amdgpu_bo_handle handles[1] = {ib.handle};
  amdgpu_bo_list_handle bo_list = NULL;
  int ret = amdgpu_bo_list_create(gpu->device, 1, handles, NULL, &bo_list);
  check_ret(ret, "amdgpu_bo_list_create");

  struct amdgpu_cs_ib_info ib_info = {
    .ib_mc_address = ib.va,
    .size = (u32)dw->count,
  };
  struct amdgpu_cs_request req = {
    .ip_type = AMDGPU_HW_IP_GFX,
    .ip_instance = 0,
    .ring = 0,
    .resources = bo_list,
    .number_of_ibs = 1,
    .ibs = &ib_info,
  };

  ret = amdgpu_cs_submit(gpu->context, 0, &req, 1);
  check_ret(ret, "amdgpu_cs_submit(GFX)");

  struct amdgpu_cs_fence fence = {
    .context = gpu->context,
    .ip_type = AMDGPU_HW_IP_GFX,
    .ip_instance = 0,
    .ring = 0,
    .fence = req.seq_no,
  };
  u32 expired = 0;
  ret = amdgpu_cs_query_fence_status(&fence, opt->timeout_ns, 0, &expired);
  check_ret(ret, "amdgpu_cs_query_fence_status(GFX)");
  if (!expired) dief("GFX fence timed out after %llu ns", (unsigned long long)opt->timeout_ns);

  amdgpu_bo_list_destroy(bo_list);
  bo_free(&ib);
}

int main(int argc, char **argv) {
  options opt = parse_options(argc, argv);
  dwords pm4 = read_pm4_text(opt.pm4_path);
  printf("loaded %zu PM4 dwords from %s\n", pm4.count, opt.pm4_path);
  dump_pm4(&pm4);

  if (opt.submit) {
    gpu_device gpu = {.fd = -1};
    gpu_open(&gpu, opt.card);
    printf("submitting to GFX ring on amdgpu libdrm %u.%u\n", gpu.major, gpu.minor);
    submit_gfx(&gpu, &pm4, &opt);
    gpu_close(&gpu);
    printf("GFX fence signaled\n");
  } else {
    printf("dry run only; pass --submit to execute on the GFX ring\n");
  }

  free(pm4.data);
  return 0;
}
