Allocating memory on the GPU is done through the KBASE_IOCTL_MEM_ALLOC operation.

It takes in an object kbase_ioctl_mem_alloc with the following union:
--- C
/**
 * union kbase_ioctl_mem_alloc - Allocate memory on the GPU
 * @in: Input parameters
 * @in.va_pages: The number of pages of virtual address space to reserve
 * @in.commit_pages: The number of physical pages to allocate
 * @in.extension: The number of extra pages to allocate on each GPU fault which grows the region
 * @in.flags: Flags
 * @out: Output parameters
 * @out.flags: Flags
 * @out.gpu_va: The GPU virtual address which is allocated
 */
---

From my testing, the output gpu_va seems to be an offset rather than a real GPU address.

Afterwards you can map it to a real address using mmap.

From my understanding, the GPU address matches a CPU address when the SAME_VA flag is provided / returned