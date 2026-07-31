#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gpu/mali_kbase_gpu_id.h"
#include "mali_kbase_ioctl.h"

// struct for storing the GPU information
struct gpu_info {
  // gpu id
  uint32_t gpu_id;

  // major architecture version
  unsigned arch_major;
  // minor architecture version
  unsigned arch_minor;
  // architecture revision
  unsigned arch_rev;

  // major product version
  unsigned product_major;

  // major version
  unsigned version_major;
  // minor version
  unsigned version_minor;
  // version status
  unsigned version_status;

  // model string
  const char *model;
};

// get the GPU model name from the architecture and product IDs
static const char *gpu_model(unsigned arch, unsigned product) {
  uint32_t model_key = GPU_ID2_MODEL_MAKE(arch, product);

  switch (model_key) {
  case GPU_ID2_PRODUCT_TMIX:
    return "Mali-TMIX";
  case GPU_ID2_PRODUCT_THEX:
    return "Mali-THEX";

  case GPU_ID2_PRODUCT_TSIX:
    return "Mali-TSIX";
  case GPU_ID2_PRODUCT_TNOX:
    return "Mali-TNOX";
  case GPU_ID2_PRODUCT_TGOX:
    return "Mali-TGOX";
  case GPU_ID2_PRODUCT_TDVX:
    return "Mali-TDVX";

  case GPU_ID2_PRODUCT_TTRX:
    return "Mali-TTRX";
  case GPU_ID2_PRODUCT_TNAX:
    return "Mali-TNAX";
  case GPU_ID2_PRODUCT_TBEX:
    return "Mali-TBEX";
  case GPU_ID2_PRODUCT_LBEX:
    return "Mali-LBEX";
  case GPU_ID2_PRODUCT_TBAX:
    return "Mali-TBAX";

  case GPU_ID2_PRODUCT_TODX:
    return "Mali-TODX";
  case GPU_ID2_PRODUCT_TGRX:
    return "Mali-TGRX";
  case GPU_ID2_PRODUCT_TVAX:
    return "Mali-TVAX";
  case GPU_ID2_PRODUCT_LODX:
    return "Mali-LODX";

  case GPU_ID2_PRODUCT_TTUX:
    return "Mali-TTUX";
  case GPU_ID2_PRODUCT_LTUX:
    return "Mali-LTUX";

  case GPU_ID2_PRODUCT_TTIX:
    return "Mali-TTIX";
  case GPU_ID2_PRODUCT_LTIX:
    return "Mali-LTIX";

  default:
    return "Unknown Mali GPU";
  }
}

// use the shifts in the vendor headers to extract the gpu model information
void decode_gpu_id2(uint32_t id, struct gpu_info *gpu) {
  memset(gpu, 0, sizeof(*gpu));
  gpu->gpu_id = id;

  gpu->version_status =
      (id & GPU_ID2_VERSION_STATUS) >> GPU_ID2_VERSION_STATUS_SHIFT;
  gpu->version_minor =
      (id & GPU_ID2_VERSION_MINOR) >> GPU_ID2_VERSION_MINOR_SHIFT;
  gpu->version_major =
      (id & GPU_ID2_VERSION_MAJOR) >> GPU_ID2_VERSION_MAJOR_SHIFT;
  gpu->product_major =
      (id & GPU_ID2_PRODUCT_MAJOR) >> GPU_ID2_PRODUCT_MAJOR_SHIFT;
  gpu->arch_rev = (id & GPU_ID2_ARCH_REV) >> GPU_ID2_ARCH_REV_SHIFT;
  gpu->arch_minor = (id & GPU_ID2_ARCH_MINOR) >> GPU_ID2_ARCH_MINOR_SHIFT;
  gpu->arch_major = (id & GPU_ID2_ARCH_MAJOR) >> GPU_ID2_ARCH_MAJOR_SHIFT;

  gpu->model = gpu_model(gpu->arch_major, gpu->product_major);
}

void print_gpu_info(struct gpu_info *gpu) {
  printf("\nGPU information\n");
  printf("---------------------------\n");

  printf("GPU ID        : 0x%08x\n", gpu->gpu_id);

  printf("Model         : %s\n", gpu->model);

  printf("Architecture  : %u.%u\n", gpu->arch_major, gpu->arch_minor);

  printf("Architecture revision : %u\n", gpu->arch_rev);

  printf("Product major : %u\n", gpu->product_major);

  printf("GPU version   : %u.%u.%u\n", gpu->version_major, gpu->version_minor,
         gpu->version_status);

  printf("\n");
}

// convert the gpuprop name into its corresponding string
static const char *gpuprop_name(uint32_t id) {
  switch (id) {
  case KBASE_GPUPROP_PRODUCT_ID:
    return "PRODUCT_ID";
  case KBASE_GPUPROP_VERSION_STATUS:
    return "VERSION_STATUS";
  case KBASE_GPUPROP_MINOR_REVISION:
    return "MINOR_REVISION";
  case KBASE_GPUPROP_MAJOR_REVISION:
    return "MAJOR_REVISION";
  case KBASE_GPUPROP_GPU_FREQ_KHZ_MAX:
    return "GPU_FREQ_KHZ_MAX";
  case KBASE_GPUPROP_LOG2_PROGRAM_COUNTER_SIZE:
    return "LOG2_PROGRAM_COUNTER_SIZE";
  case KBASE_GPUPROP_TEXTURE_FEATURES_0:
    return "TEXTURE_FEATURES_0";
  case KBASE_GPUPROP_TEXTURE_FEATURES_1:
    return "TEXTURE_FEATURES_1";
  case KBASE_GPUPROP_TEXTURE_FEATURES_2:
    return "TEXTURE_FEATURES_2";
  case KBASE_GPUPROP_GPU_AVAILABLE_MEMORY_SIZE:
    return "GPU_AVAILABLE_MEMORY_SIZE";

  case KBASE_GPUPROP_L2_LOG2_LINE_SIZE:
    return "L2_LOG2_LINE_SIZE";
  case KBASE_GPUPROP_L2_LOG2_CACHE_SIZE:
    return "L2_LOG2_CACHE_SIZE";
  case KBASE_GPUPROP_L2_NUM_L2_SLICES:
    return "L2_NUM_L2_SLICES";

  case KBASE_GPUPROP_TILER_BIN_SIZE_BYTES:
    return "TILER_BIN_SIZE_BYTES";
  case KBASE_GPUPROP_TILER_MAX_ACTIVE_LEVELS:
    return "TILER_MAX_ACTIVE_LEVELS";

  case KBASE_GPUPROP_MAX_THREADS:
    return "MAX_THREADS";
  case KBASE_GPUPROP_MAX_WORKGROUP_SIZE:
    return "MAX_WORKGROUP_SIZE";
  case KBASE_GPUPROP_MAX_BARRIER_SIZE:
    return "MAX_BARRIER_SIZE";
  case KBASE_GPUPROP_MAX_REGISTERS:
    return "MAX_REGISTERS";
  case KBASE_GPUPROP_MAX_TASK_QUEUE:
    return "MAX_TASK_QUEUE";
  case KBASE_GPUPROP_MAX_THREAD_GROUP_SPLIT:
    return "MAX_THREAD_GROUP_SPLIT";
  case KBASE_GPUPROP_IMPL_TECH:
    return "IMPL_TECH";

  case KBASE_GPUPROP_RAW_SHADER_PRESENT:
    return "RAW_SHADER_PRESENT";
  case KBASE_GPUPROP_RAW_TILER_PRESENT:
    return "RAW_TILER_PRESENT";
  case KBASE_GPUPROP_RAW_L2_PRESENT:
    return "RAW_L2_PRESENT";
  case KBASE_GPUPROP_RAW_STACK_PRESENT:
    return "RAW_STACK_PRESENT";
  case KBASE_GPUPROP_RAW_L2_FEATURES:
    return "RAW_L2_FEATURES";
  case KBASE_GPUPROP_RAW_CORE_FEATURES:
    return "RAW_CORE_FEATURES";
  case KBASE_GPUPROP_RAW_MEM_FEATURES:
    return "RAW_MEM_FEATURES";
  case KBASE_GPUPROP_RAW_MMU_FEATURES:
    return "RAW_MMU_FEATURES";
  case KBASE_GPUPROP_RAW_AS_PRESENT:
    return "RAW_AS_PRESENT";
  case KBASE_GPUPROP_RAW_JS_PRESENT:
    return "RAW_JS_PRESENT";

  case KBASE_GPUPROP_RAW_JS_FEATURES_0:
    return "RAW_JS_FEATURES_0";
  case KBASE_GPUPROP_RAW_JS_FEATURES_1:
    return "RAW_JS_FEATURES_1";
  case KBASE_GPUPROP_RAW_JS_FEATURES_2:
    return "RAW_JS_FEATURES_2";
  case KBASE_GPUPROP_RAW_JS_FEATURES_3:
    return "RAW_JS_FEATURES_3";
  case KBASE_GPUPROP_RAW_JS_FEATURES_4:
    return "RAW_JS_FEATURES_4";
  case KBASE_GPUPROP_RAW_JS_FEATURES_5:
    return "RAW_JS_FEATURES_5";
  case KBASE_GPUPROP_RAW_JS_FEATURES_6:
    return "RAW_JS_FEATURES_6";
  case KBASE_GPUPROP_RAW_JS_FEATURES_7:
    return "RAW_JS_FEATURES_7";
  case KBASE_GPUPROP_RAW_JS_FEATURES_8:
    return "RAW_JS_FEATURES_8";
  case KBASE_GPUPROP_RAW_JS_FEATURES_9:
    return "RAW_JS_FEATURES_9";
  case KBASE_GPUPROP_RAW_JS_FEATURES_10:
    return "RAW_JS_FEATURES_10";
  case KBASE_GPUPROP_RAW_JS_FEATURES_11:
    return "RAW_JS_FEATURES_11";
  case KBASE_GPUPROP_RAW_JS_FEATURES_12:
    return "RAW_JS_FEATURES_12";
  case KBASE_GPUPROP_RAW_JS_FEATURES_13:
    return "RAW_JS_FEATURES_13";
  case KBASE_GPUPROP_RAW_JS_FEATURES_14:
    return "RAW_JS_FEATURES_14";
  case KBASE_GPUPROP_RAW_JS_FEATURES_15:
    return "RAW_JS_FEATURES_15";
  case KBASE_GPUPROP_RAW_TILER_FEATURES:
    return "RAW_TILER_FEATURES";
  case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0:
    return "RAW_TEXTURE_FEATURES_0";
  case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1:
    return "RAW_TEXTURE_FEATURES_1";
  case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2:
    return "RAW_TEXTURE_FEATURES_2";
  case KBASE_GPUPROP_RAW_GPU_ID:
    return "RAW_GPU_ID";
  case KBASE_GPUPROP_RAW_THREAD_MAX_THREADS:
    return "RAW_THREAD_MAX_THREADS";
  case KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE:
    return "RAW_THREAD_MAX_WORKGROUP_SIZE";
  case KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE:
    return "RAW_THREAD_MAX_BARRIER_SIZE";
  case KBASE_GPUPROP_RAW_THREAD_FEATURES:
    return "RAW_THREAD_FEATURES";
  case KBASE_GPUPROP_RAW_COHERENCY_MODE:
    return "RAW_COHERENCY_MODE";
  case KBASE_GPUPROP_COHERENCY_NUM_GROUPS:
    return "COHERENCY_NUM_GROUPS";
  case KBASE_GPUPROP_COHERENCY_NUM_CORE_GROUPS:
    return "COHERENCY_NUM_CORE_GROUPS";
  case KBASE_GPUPROP_COHERENCY_COHERENCY:
    return "COHERENCY_COHERENCY";
  case KBASE_GPUPROP_COHERENCY_GROUP_0:
    return "COHERENCY_GROUP_0";
  case KBASE_GPUPROP_COHERENCY_GROUP_1:
    return "COHERENCY_GROUP_1";
  case KBASE_GPUPROP_COHERENCY_GROUP_2:
    return "COHERENCY_GROUP_2";
  case KBASE_GPUPROP_COHERENCY_GROUP_3:
    return "COHERENCY_GROUP_3";
  case KBASE_GPUPROP_COHERENCY_GROUP_4:
    return "COHERENCY_GROUP_4";
  case KBASE_GPUPROP_COHERENCY_GROUP_5:
    return "COHERENCY_GROUP_5";
  case KBASE_GPUPROP_COHERENCY_GROUP_6:
    return "COHERENCY_GROUP_6";
  case KBASE_GPUPROP_COHERENCY_GROUP_7:
    return "COHERENCY_GROUP_7";
  case KBASE_GPUPROP_COHERENCY_GROUP_8:
    return "COHERENCY_GROUP_8";
  case KBASE_GPUPROP_COHERENCY_GROUP_9:
    return "COHERENCY_GROUP_9";
  case KBASE_GPUPROP_COHERENCY_GROUP_10:
    return "COHERENCY_GROUP_10";
  case KBASE_GPUPROP_COHERENCY_GROUP_11:
    return "COHERENCY_GROUP_11";
  case KBASE_GPUPROP_COHERENCY_GROUP_12:
    return "COHERENCY_GROUP_12";
  case KBASE_GPUPROP_COHERENCY_GROUP_13:
    return "COHERENCY_GROUP_13";
  case KBASE_GPUPROP_COHERENCY_GROUP_14:
    return "COHERENCY_GROUP_14";
  case KBASE_GPUPROP_COHERENCY_GROUP_15:
    return "COHERENCY_GROUP_15";
  case KBASE_GPUPROP_TEXTURE_FEATURES_3:
    return "TEXTURE_FEATURES_3";
  case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3:
    return "TEXTURE_FEATURES_3";
  case KBASE_GPUPROP_NUM_EXEC_ENGINES:
    return "NUM_EXEC_ENGINES";
  case KBASE_GPUPROP_RAW_THREAD_TLS_ALLOC:
    return "RAW_THREAD_TLS_ALLOC";
  case KBASE_GPUPROP_TLS_ALLOC:
    return "TLS_ALLOC";
  case KBASE_GPUPROP_RAW_GPU_FEATURES:
    return "RAW_GPU_FEATURES";

  default:
    return "UNKNOWN";
  }
}

static uint64_t read_value(uint8_t *buf, size_t size) {
  uint64_t v = 0;

  for (size_t i = 0; i < size; i++)
    v |= ((uint64_t)buf[i]) << (i * 8);

  return v;
}

// Takes a buffer and its lenght, and prints the GPU properties
void parse_gpuprops(void *buffer, size_t length, uint64_t *out_shader_present) {
  // get a pointer to the buffer (at offset 0)
  uint8_t *buf = buffer;

  // define an offset
  size_t off = 0;

  printf("\n%3s  %-32s = %-18s  (DECIMAL)\n", "ID", "ID NAME", "HEX");
  // advance 4 by 4 bytes
  while (off + 4 <= length) {
    // get the current index
    uint32_t key = *(uint32_t *)(buf + off);

    // advance the offset
    off += 4;

    // logic defined in the vendor headers
    // extract the ID
    uint32_t id = key >> 2;
    // extract the code size
    uint32_t size_code = key & 0x3;

    size_t value_size;

    // save the value size depending on the size code
    switch (size_code) {
    case KBASE_GPUPROP_VALUE_SIZE_U8:
      value_size = 1;
      break;

    case KBASE_GPUPROP_VALUE_SIZE_U16:
      value_size = 2;
      break;

    case KBASE_GPUPROP_VALUE_SIZE_U32:
      value_size = 4;
      break;

    case KBASE_GPUPROP_VALUE_SIZE_U64:
      value_size = 8;
      break;

    default:
      printf("Invalid size code %u\n", size_code);
      return;
    }

    // if the value length doesn't match the actual length, return an error
    if (off + value_size > length) {
      printf("Truncated property %u\n", id);
      return;
    }

    uint64_t value = read_value(buf + off, value_size);

    // increase the offset by the value size
    off += value_size;

    printf("%3u  %-32s = 0x%016llx  (%llu)\n", id, gpuprop_name(id),
           (unsigned long long)value, (unsigned long long)value);

    /* Some useful decoding */
    switch (id) {
    case 55: /* RAW_GPU_ID */
      printf("\n       GPU ID: 0x%08llx\n", (unsigned long long)value);

      struct gpu_info gpu;

      decode_gpu_id2(value, &gpu);

      print_gpu_info(&gpu);
      break;

    case 25: /* RAW_SHADER_PRESENT */
      printf("\n       Shader cores: %d\n\n", __builtin_popcountll(value));
      if (out_shader_present) {
        *out_shader_present = value;
      }
      break;

    case 27: /* RAW_L2_PRESENT */
      printf("\n       L2 slices: %d\n\n", __builtin_popcountll(value));
      break;

    case 6: /* GPU_FREQ_KHZ_MAX */
      printf("\n       Max frequency: %llu kHz\n\n", (unsigned long long)value);
      break;
    }
  }
}