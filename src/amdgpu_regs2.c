#define _GNU_SOURCE

#include "common.h"

#include <fcntl.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct amdgpu_debugfs_regs2_iocdata_v2 {
  u32 use_srbm;
  u32 use_grbm;
  u32 pg_lock;
  struct {
    u32 se;
    u32 sh;
    u32 instance;
  } grbm;
  struct {
    u32 me;
    u32 pipe;
    u32 queue;
    u32 vmid;
  } srbm;
  u32 xcc_id;
} regs2_ioc_data;

#ifndef AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2
#define AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2 _IOW(0x20, 2, regs2_ioc_data)
#endif

typedef enum {
  OP_READ,
  OP_WRITE,
} op_kind;

typedef struct {
  const char *regs2_path;
  u64 offset;
  u32 value;
  op_kind op;
  regs2_ioc_data state;
} options;

static u32 parse_u32_arg(const char *value, const char *name) {
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(value, &end, 0);
  if (errno || !end || *end || parsed > UINT32_MAX) dief("invalid %s: %s", name, value);
  return (u32)parsed;
}

static u64 parse_u64_arg(const char *value, const char *name) {
  char *end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull(value, &end, 0);
  if (errno || !end || *end) dief("invalid %s: %s", name, value);
  return (u64)parsed;
}

static void parse_csv3(const char *value, u32 out[3], const char *name) {
  char *copy = strdup(value);
  if (!copy) die_errno("strdup");

  char *cursor = copy;
  for (int i = 0; i < 3; i++) {
    char *part = strsep(&cursor, ",");
    if (!part || !*part) dief("invalid %s: %s", name, value);
    out[i] = parse_u32_arg(part, name);
  }
  if (cursor && *cursor) dief("invalid %s: %s", name, value);
  free(copy);
}

static void parse_csv4(const char *value, u32 out[4], const char *name) {
  char *copy = strdup(value);
  if (!copy) die_errno("strdup");

  char *cursor = copy;
  for (int i = 0; i < 4; i++) {
    char *part = strsep(&cursor, ",");
    if (!part || !*part) dief("invalid %s: %s", name, value);
    out[i] = parse_u32_arg(part, name);
  }
  if (cursor && *cursor) dief("invalid %s: %s", name, value);
  free(copy);
}

static void usage(FILE *stream) {
  fprintf(stream,
          "usage: amdgpu-regs2 --regs2 PATH --offset BYTES read\n"
          "       amdgpu-regs2 --regs2 PATH --offset BYTES write --value U32\n"
          "\n"
          "Low-level helper for /sys/kernel/debug/dri/*/regs2. Offsets are byte\n"
          "offsets in the debugfs register aperture. Use UMR or the kernel register\n"
          "headers to resolve named registers for your ASIC.\n"
          "\n"
          "banking options:\n"
          "  --srbm ME,PIPE,QUEUE,VMID    set SRBM banking, for VMID-banked regs\n"
          "  --srbm-vmid VMID             shortcut for --srbm 0,0,0,VMID\n"
          "  --grbm SE,SH,INSTANCE        set GRBM banking\n"
          "  --pg-lock N                  set pg_lock in ioctl state\n"
          "  --xcc-id N                   set xcc_id (default: 0xffffffff)\n"
          "\n"
          "examples:\n"
          "  amdgpu-regs2 --regs2 /sys/kernel/debug/dri/0/regs2 --offset 0x1234 read\n"
          "  amdgpu-regs2 --regs2 /sys/kernel/debug/dri/0/regs2 --srbm-vmid 1 "
          "--offset 0x1234 write --value 0xdeadbeef\n");
}

static options parse_options(int argc, char **argv) {
  options opt = {
    .regs2_path = NULL,
    .offset = UINT64_MAX,
    .value = 0,
    .op = OP_READ,
    .state = {.xcc_id = UINT32_MAX},
  };
  bool saw_op = false;
  bool saw_value = false;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      usage(stdout);
      exit(0);
    } else if (!strcmp(arg, "read")) {
      opt.op = OP_READ;
      saw_op = true;
    } else if (!strcmp(arg, "write")) {
      opt.op = OP_WRITE;
      saw_op = true;
    } else if (!next) {
      dief("missing value for %s", arg);
    } else if (!strcmp(arg, "--regs2")) {
      opt.regs2_path = next;
      i++;
    } else if (!strcmp(arg, "--offset")) {
      opt.offset = parse_u64_arg(next, "offset");
      i++;
    } else if (!strcmp(arg, "--value")) {
      opt.value = parse_u32_arg(next, "value");
      saw_value = true;
      i++;
    } else if (!strcmp(arg, "--srbm-vmid")) {
      opt.state.use_srbm = 1;
      opt.state.srbm.vmid = parse_u32_arg(next, "srbm-vmid");
      i++;
    } else if (!strcmp(arg, "--srbm")) {
      u32 fields[4];
      parse_csv4(next, fields, "srbm");
      opt.state.use_srbm = 1;
      opt.state.srbm.me = fields[0];
      opt.state.srbm.pipe = fields[1];
      opt.state.srbm.queue = fields[2];
      opt.state.srbm.vmid = fields[3];
      i++;
    } else if (!strcmp(arg, "--grbm")) {
      u32 fields[3];
      parse_csv3(next, fields, "grbm");
      opt.state.use_grbm = 1;
      opt.state.grbm.se = fields[0];
      opt.state.grbm.sh = fields[1];
      opt.state.grbm.instance = fields[2];
      i++;
    } else if (!strcmp(arg, "--pg-lock")) {
      opt.state.pg_lock = parse_u32_arg(next, "pg-lock");
      i++;
    } else if (!strcmp(arg, "--xcc-id")) {
      opt.state.xcc_id = parse_u32_arg(next, "xcc-id");
      i++;
    } else {
      dief("unknown option: %s", arg);
    }
  }

  if (!saw_op) dief("missing operation: read or write");
  if (!opt.regs2_path) dief("missing --regs2 PATH");
  if (opt.offset == UINT64_MAX) dief("missing --offset BYTES");
  if (opt.op == OP_WRITE && !saw_value) dief("write requires --value U32");
  return opt;
}

int main(int argc, char **argv) {
  options opt = parse_options(argc, argv);

  int fd = open(opt.regs2_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) die_errno(opt.regs2_path);

  if (ioctl(fd, AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2, &opt.state) < 0) {
    die_errno("AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2");
  }

  off_t got = lseek(fd, (off_t)opt.offset, SEEK_SET);
  if (got < 0 || (u64)got != opt.offset) die_errno("lseek regs2");

  if (opt.op == OP_READ) {
    u32 value = 0;
    ssize_t bytes = read(fd, &value, sizeof(value));
    if (bytes != (ssize_t)sizeof(value)) die_errno("read regs2");
    printf("0x%08x\n", value);
  } else {
    ssize_t bytes = write(fd, &opt.value, sizeof(opt.value));
    if (bytes != (ssize_t)sizeof(opt.value)) die_errno("write regs2");
  }

  close(fd);
  return 0;
}
