#include "csf/mali_base_csf_kernel.h"
#include "csf/mali_kbase_csf_ioctl.h"
#include "initialize.h"
#include "memory.h"
#include "parse_gpu_props.h"

#include <sys/mman.h>
#include <unistd.h>

// STUBS — unverified, placeholder byte offsets. See prior discussion.
#define CS_INSERT 0x0000
#define CS_EXTRACT 0x0000
#define CS_ACTIVE 0x0008
#define CS_FAULT 0x0010
#define CS_FAULT_INFO 0x0018

int main(void) {
  // ------------------------------------------------------------------ OPEN GPU
  int fd = open_gpu();
  fprintf(stderr, "OPENED GPU\n");

  union kbase_ioctl_cs_get_glb_iface glb = {0};
  glb.in.max_group_num = 0;
  glb.in.max_total_stream_num = 0;

  if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glb) < 0) {
    perror("CS_GET_GLB_IFACE");
  } else {
    printf("glb_version=0x%x features=0x%x group_num=%u prfcnt_size=%u "
           "total_stream_num=%u instr_features=0x%x\n",
           glb.out.glb_version, glb.out.features, glb.out.group_num,
           glb.out.prfcnt_size, glb.out.total_stream_num,
           glb.out.instr_features);
  }

  struct basep_cs_group_control groups[8] = {0};
  struct basep_cs_stream_control streams[64] = {0};

  union kbase_ioctl_cs_get_glb_iface glb2 = {0};
  glb2.in.max_group_num = 8;
  glb2.in.max_total_stream_num = 64;
  glb2.in.groups_ptr = (uint64_t)(uintptr_t)groups;
  glb2.in.streams_ptr = (uint64_t)(uintptr_t)streams;

  if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glb2) < 0) {
    perror("CS_GET_GLB_IFACE (with buffers)");
  } else {
    for (int i = 0; i < 8; i++) {
      printf("group[%d]: features=0x%x stream_num=%u suspend_size=%u\n", i,
             groups[i].features, groups[i].stream_num, groups[i].suspend_size);
    }
    for (int i = 0; i < 8; i++) { // just first 8 streams for readability
      printf("stream[%d]: features=0x%x\n", i, streams[i].features);
    }
  }

  int ret = 0;

  // setup the struct to store the result of the gpu properties probe (this is
  // done to check that something is returned)
  struct kbase_ioctl_get_gpuprops probe = {
      .buffer = 0,
      .size = 0,
      .flags = 0,
  };
  ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &probe);
  if (ret < 0) {
    fprintf(stderr, "GET_GPUPROPS (size probe) failed: %s\n", strerror(errno));
    close(fd);
    return 1;
  }

  // extract the size of the properties from the return value
  size_t props_size = (size_t)ret;

  if (props_size == 0) {
    fprintf(stderr, "error obtaining the gpu properties\n");
    close(fd);
    return 1;
  }

  printf("GET_GPUPROPS reports size=%zu bytes\n", props_size);

  // create a buffer to store the data
  unsigned char *props_buf = calloc(1, props_size);
  if (!props_buf) {
    perror("calloc");
    close(fd);
    return 1;
  }

  // create a new struct to actually get the information
  struct kbase_ioctl_get_gpuprops fetch = {
      .buffer = (__u64)(uintptr_t)props_buf,
      .size = (__u32)props_size,
      .flags = 0,
  };
  // make the ioctl request
  ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &fetch);
  if (ret < 0) {
    fprintf(stderr, "GET_GPUPROPS (fetch) failed: %s\n", strerror(errno));
    free(props_buf);
    close(fd);
    return 1;
  }

  printf("GET_GPUPROPS fetch OK\n");

  // parse the GPU properties from the obtained buffer, for the human
  parse_gpuprops(props_buf, props_size);

  // and pull out the one property this test has to act on: the real
  // shader-core mask for CS_QUEUE_GROUP_CREATE below. Reuses the buffer
  // already fetched above rather than re-running the GET_GPUPROPS pair.
  uint64_t shader_present = 0;
  if (!gpuprops_lookup(props_buf, props_size, KBASE_GPUPROP_RAW_SHADER_PRESENT,
                       &shader_present)) {
    fprintf(stderr, "RAW_SHADER_PRESENT not present in gpuprops\n");
    free(props_buf);
    close(fd);
    return 1;
  }

  printf("\n== probe complete, device is talking ==\n");

  // ------------------------------------------------------------------ CREATE
  // GROUP QUEUE

  union kbase_ioctl_cs_queue_group_create create = {0};
  create.in.tiler_mask = shader_present;
  create.in.fragment_mask = shader_present;
  create.in.compute_mask = shader_present;
  create.in.cs_min = 1;
  create.in.priority = BASE_QUEUE_GROUP_PRIORITY_MEDIUM;
  create.in.tiler_max = 1;
  create.in.fragment_max = 1;
  create.in.compute_max = 1;

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE, &create) < 0) {
    perror("CS_QUEUE_GROUP_CREATE");
    return 1;
  }

  fprintf(stderr, "GROUP CREATE\n");

  fprintf(stderr, "group_handle=%u group_uid=%u ret=%d errno=%d\n",
          create.out.group_handle, create.out.group_uid, ret, errno);

  // ------------------------------------------------------------------ CREATE
  // BO
  struct kbase_bo *queue_bo = kbase_bo_create(fd, 4096);

  fprintf(stderr, "BO CREATE\n");

  // ------------------------------------------------------------------ REGISTER
  // QUEUE
  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = queue_bo->gpu_va,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
    perror("CS_QUEUE_REGISTER");
    return 1;
  }

  fprintf(stderr, "QUEUE REGISTER\n");

  // ------------------------------------------------------------------ BIND
  // QUEUE
  union kbase_ioctl_cs_queue_bind bind = {0};
  bind.in.buffer_gpu_addr = queue_bo->gpu_va;
  bind.in.group_handle = create.out.group_handle;
  bind.in.csi_index = 0;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
    perror("CS_QUEUE_BIND");
    return 1;
  }

  fprintf(stderr, "QUEUE BIND\n");

  union kbase_ioctl_cs_get_glb_iface glbnew = {0};
  glbnew.in.max_group_num = 0;
  glbnew.in.max_total_stream_num = 0;

  if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glbnew) < 0) {
    perror("CS_GET_GLB_IFACE");
  } else {
    printf("glb_version=0x%x features=0x%x group_num=%u prfcnt_size=%u "
           "total_stream_num=%u instr_features=0x%x\n",
           glbnew.out.glb_version, glbnew.out.features, glbnew.out.group_num,
           glbnew.out.prfcnt_size, glbnew.out.total_stream_num,
           glbnew.out.instr_features);
  }

  struct basep_cs_group_control groups2[8] = {0};
  struct basep_cs_stream_control streams2[64] = {0};

  union kbase_ioctl_cs_get_glb_iface glb22 = {0};
  glb22.in.max_group_num = 8;
  glb22.in.max_total_stream_num = 64;
  glb22.in.groups_ptr = (uint64_t)(uintptr_t)groups2;
  glb22.in.streams_ptr = (uint64_t)(uintptr_t)streams2;

  if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glb22) < 0) {
    perror("CS_GET_GLB_IFACE (with buffers)");
  } else {
    for (int i = 0; i < 8; i++) {
      printf("group[%d]: features=0x%x stream_num=%u suspend_size=%u\n", i,
             groups2[i].features, groups2[i].stream_num,
             groups2[i].suspend_size);
    }
    for (int i = 0; i < 8; i++) { // just first 8 streams for readability
      printf("stream[%d]: features=0x%x\n", i, streams2[i].features);
    }
  }

  // ------------------------------------------------------------------ NEW
  fprintf(stderr, "mmap_handle = 0x%llx (page_aligned=%d)\n",
          (unsigned long long)bind.out.mmap_handle,
          (bind.out.mmap_handle % sysconf(_SC_PAGESIZE)) == 0);

  size_t io_size = BASEP_QUEUE_NR_MMAP_USER_PAGES * sysconf(_SC_PAGESIZE);
  void *io = mmap(NULL, io_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (off_t)bind.out.mmap_handle);
  if (io == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  volatile uint8_t *input_page = (volatile uint8_t *)io;
  volatile uint8_t *output_page =
      (volatile uint8_t *)io + sysconf(_SC_PAGESIZE);

  volatile uint32_t *doorbell =
      (volatile uint32_t *)(io +
                            2 * sysconf(_SC_PAGESIZE)); // verify offset first!

  // ---- The actual command: raw NOP, no Mesa, no pack macros ----
  uint64_t *cs = queue_bo->cpu;
  cs[0] = 0x0000000000000000ULL; // NOP

  uint32_t insert_offset = sizeof(uint64_t);
  *(volatile uint64_t *)(input_page + CS_INSERT) = insert_offset;

  uint64_t before[3 * 512];
  memcpy(before, (void *)io, io_size);

  // ------------------------------------------------------------------ KICK
  struct kbase_ioctl_cs_queue_kick kick = {0};
  kick.buffer_gpu_addr = queue_bo->gpu_va;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0) {
    perror("CS_QUEUE_KICK");
    return 1;
  }

  usleep(1000000);
  *doorbell = 1; // ring it

  for (int i = 0; i < 50; i++) {
    usleep(100000); // 100ms per iteration, 5 seconds total
    uint64_t extract = *(volatile uint64_t *)(output_page + CS_EXTRACT);
    uint64_t active = *(volatile uint64_t *)(output_page + CS_ACTIVE);
    uint64_t fault = *(volatile uint64_t *)(output_page + CS_FAULT);
    printf("[t=%dms] extract=%lu active=%lu fault=0x%lx\n", (i + 1) * 100,
           extract, active, fault);
  }

  for (size_t i = 0; i < io_size / 8; i++) {
    uint64_t v = ((uint64_t *)io)[i];
    if (v != before[i])
      fprintf(stderr, "offset 0x%zx: 0x%lx -> 0x%lx\n", i * 8, before[i], v);
  }

  uint64_t extract = *(volatile uint64_t *)(output_page + CS_EXTRACT);
  uint64_t fault = *(volatile uint64_t *)(output_page + CS_FAULT);
  uint64_t fault_info = *(volatile uint64_t *)(output_page + CS_FAULT_INFO);

  printf("CS_EXTRACT=%lu CS_FAULT=0x%lx CS_FAULT_INFO=0x%lx\n", extract, fault,
         fault_info);

  if (extract >= insert_offset && fault == 0)
    printf("GPU consumed the instruction stream cleanly.\n");
  else if (fault != 0)
    printf("GPU faulted.\n");
  else
    printf("CS_EXTRACT never advanced.\n");

  return 0;
}