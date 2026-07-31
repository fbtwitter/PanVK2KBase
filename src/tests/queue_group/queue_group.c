#include "csf/mali_kbase_csf_ioctl.h"
#include "initialize.h"
#include "memory.h"

int main(void) {
  // open the GPU
  int fd = open_gpu();

  union kbase_ioctl_cs_queue_group_create create = {0};

  create.in.tiler_mask = ~0ULL;
  create.in.fragment_mask = ~0ULL;
  create.in.compute_mask = ~0ULL;

  create.in.cs_min = 1;
  create.in.priority = 0;

  create.in.tiler_max = 1;
  create.in.fragment_max = 1;
  create.in.compute_max = 1;

  int ret = ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE, &create);

  printf("ret=%d errno=%d\n", ret, errno);

  if (ret < 0) {
    perror("CS_QUEUE_GROUP_CREATE");
    return 1;
  }

  printf("group_handle = %u\n", create.out.group_handle);
  printf("group_uid    = %u\n", create.out.group_uid);

  struct kbase_bo *queue_bo = kbase_bo_create(fd, 4096);

  printf("queue gpu_va = 0x%016lx\n", queue_bo->gpu_va);
  printf("queue size   = %zu\n", queue_bo->size);

  // -------------------------------------------REGISTER-------------------------------------------

  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = (uint64_t)(uintptr_t)queue_bo->cpu,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };

  ret = ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg);

  printf("KBASE_IOCTL_CS_QUEUE_REGISTER: ret=%d errno=%d\n", ret, errno);

  if (ret < 0) {
    perror("CS_QUEUE_REGISTER");
    return 1;
  }

  // -------------------------------------------BIND-------------------------------------------

  union kbase_ioctl_cs_queue_bind bind = {0};

  bind.in.buffer_gpu_addr = (uint64_t)(uintptr_t)queue_bo->cpu;
  bind.in.group_handle = create.out.group_handle;
  bind.in.csi_index = 1;

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
    perror("CS_QUEUE_BIND");
    return 1;
  }

  printf("mmap_handle = 0x%llx\n", (unsigned long long)bind.out.mmap_handle);

  // HW doorbell page, input page, output page - in that order, measured
  // by tests/user_io_probe (see utils/csf_user_regs.h). This probe only
  // maps them; it does not index into them.
  // (BASEP_QUEUE_NR_MMAP_USER_PAGES, csf/mali_base_csf_kernel.h).
  size_t queue_state_size = BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096;

  void *queue_state = mmap(
      NULL,
      queue_state_size,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      fd,
      (off_t)bind.out.mmap_handle
  );

  if (queue_state == MAP_FAILED) {
      perror("CS_QUEUE_BIND mmap");
      return 1;
  }

  printf("queue_state=%p (doorbell/input/output, %zu bytes)\n",
         queue_state, queue_state_size);

  uint32_t *q = queue_bo->cpu;

  for (int i = 0; i < 16; i++)
      q[i] = 0xdeadbeef;

  struct kbase_ioctl_cs_queue_kick kick = {0};

  kick.buffer_gpu_addr = (uint64_t)(uintptr_t)queue_bo->cpu;

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0) {
    perror("CS_QUEUE_KICK");
    return 1;
  }

  printf("CS_QUEUE_KICK OK\n");

  if (munmap(queue_state, queue_state_size) < 0)
    perror("munmap queue_state");

  // Tear down in dependency order: the queue holds a reference to
  // queue_bo (via REGISTER's buffer_gpu_addr) and the group holds a
  // reference to the queue (via BIND) - MEM_FREE on queue_bo fails with
  // EINVAL if either is still alive when it's called.
  struct kbase_ioctl_cs_queue_terminate q_term = {
      .buffer_gpu_addr = (uint64_t)(uintptr_t)queue_bo->cpu,
  };

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &q_term) < 0)
    perror("CS_QUEUE_TERMINATE");

  struct kbase_ioctl_cs_queue_group_term term = {
      .group_handle = create.out.group_handle,
  };

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term) < 0)
    perror("CS_QUEUE_GROUP_TERMINATE");

  kbase_bo_free(fd, queue_bo);

  return 0;
}