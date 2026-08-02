CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
MALIFLAGS ?= -DMALI_USE_CSF=1
MESAFLAGS ?= -DPAN_ARCH=10
 
# Which vendored kbase uapi header set to build against. Not limited to
# a fixed list - any third_party/kbase-uapi-<name>/ directory following
# the same layout works (see third_party/kbase-uapi-r49p1/README.md for
# what that layout is: an 18-19 file mirror of a real vendor kbase UAPI
# tree). Two are vendored right now: r44p0 (UK 1.20, Mali-G615-MC2
# target) and r49p1 (UK 1.30, confirmed match for the Poco X8 Pro /
# Mali-G720 device tested in docs/kbase-notes.md). Override on the
# command line to target any of them, e.g.:
#   make queue_group KBASE_VERSION=r49p1
#   make list-kbase-versions   # see what's actually vendored
KBASE_VERSION ?= r44p0
KBASE_UAPI_DIR := third_party/kbase-uapi-$(KBASE_VERSION)

ifeq ($(wildcard $(KBASE_UAPI_DIR)),)
$(error KBASE_VERSION=$(KBASE_VERSION) has no $(KBASE_UAPI_DIR)/ - run \
  'make list-kbase-versions' to see what's vendored, or vendor a new \
  one there first)
endif

# -include the kconfig shim so headers using IS_ENABLED() (r49p1, and
# potentially future MTK-derived versions) resolve it without a real
# kernel build tree (see src/utils/kconfig_shim.h). Harmless for header
# sets that don't use IS_ENABLED at all, like r44p0.
#
# Source files that use an ioctl/struct/flag not guaranteed present
# across every vendored version (like KBASE_IOCTL_INTERNAL_FENCE_WAIT,
# r49p1-only) should guard that usage with "#ifdef SYMBOL_NAME" so they
# build against any KBASE_VERSION and just skip what that header
# doesn't declare - see tests/fence_probe/fence_probe.c for the pattern
# to follow when adding new version-sensitive code.
# (The -include itself is on the INCLUDES line below.)

# An in-tree Mesa checkout (third_party/mesa, gitignored) plus the C11
# threads.h stubs under stubs/. Only the targets that pull in Mesa headers
# directly - the ones built with $(MESAFLAGS) - need these; the standalone
# kbase probes below build without a Mesa checkout present.
MESA_DIR := third_party/mesa/src
MESA_GENXML_DIR := third_party/mesa/src/panfrost
MESA_INCLUDE_DIR := third_party/mesa/include
STUB_INCLUDE_DIR := stubs

MESA_INCLUDES := -I$(STUB_INCLUDE_DIR) -I$(MESA_DIR) -I$(MESA_GENXML_DIR) \
                 -I$(MESA_INCLUDE_DIR)

INCLUDES := -I$(KBASE_UAPI_DIR) -Isrc/utils -include src/utils/kconfig_shim.h \
            $(MESA_INCLUDES)

.PHONY: all clean list-kbase-versions regress regress-render regress-list

all: first_test

list-kbase-versions:
	@ls -1 third_party | sed -n 's/^kbase-uapi-//p'

# Build, push (md5-verified) and run the known-good probe set against the
# attached device, with per-probe timeouts and interleaved health checks.
# The logic lives in tools/run-probes.sh rather than here - it needs state
# that does not read well as Make. See that file's header, especially the
# part about why tests/alias_cs_probe is never run by it.
#
#   make regress          raw + driver tiers (default, unattended-safe)
#   make regress-render   also the render tier (passes --i-know-it-hangs)
#   make regress-list     print the manifest without running anything
#
# Pass extra flags with ARGS=, e.g.  make regress ARGS=--only=first_test
regress:
	@bash tools/run-probes.sh $(ARGS)

regress-render:
	@bash tools/run-probes.sh --with-render $(ARGS)

regress-list:
	@bash tools/run-probes.sh --list
 
first_test: ./src/tests/first_test/first_test.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/first_test $<

memory: ./src/tests/memory/memory.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/memory $<

memory2: ./src/tests/memory/memory2.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/memory2 $<

queue_group: ./src/tests/queue_group/queue_group.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/queue_group $<

command: ./src/tests/command/command.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/command $<

# Builds against any KBASE_VERSION - adapts at compile time to whether
# that header set declares KBASE_IOCTL_INTERNAL_FENCE_WAIT (r49p1: yes,
# r44p0: no) instead of requiring one specific version.
fence_probe: ./src/tests/fence_probe/fence_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/fence_probe $<

event_probe: ./src/tests/event_probe/event_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/event_probe $<

# Read-only query of the CSF firmware's global interface (slot counts,
# stream counts, interface version). No submission, no state change.
glb_iface_probe: ./src/tests/glb_iface_probe/glb_iface_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/glb_iface_probe $<

# dlopen()s a built PanVK Android driver to check it loads on-device.
# Doesn't touch /vendor - see the source for why.
driver_load_probe: ./src/tests/driver_load_probe/driver_load_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_load_probe $<

# The driver_* probes below load a real Android Vulkan driver, so unlike the
# rest of this makefile they must be cross-compiled against the NDK sysroot -
# the default gcc has no <vulkan/vulkan.h> and could not run on the device
# anyway. From WSL:
#
#   CC=/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/\
#   aarch64-linux-android34-clang make driver_compute_probe
#
# Push with PowerShell, not Git Bash: Git Bash rewrites the remote path in
# `adb push`, which has previously left stale binaries on the device while
# the log claimed a fresh one. Verify md5 on both ends afterwards.
#
# Drives a built PanVK driver through its Android HAL entrypoint and calls
# vkEnumeratePhysicalDevices - the end-to-end test of the kbase backend.
driver_enum_probe: ./src/tests/driver_enum_probe/driver_enum_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_enum_probe $<

# Asks the built driver whether it still claims dma-buf import/export, which
# it cannot do on kbase. Exits non-zero if it does. This is what verifies
# patch-panvk-kbase-external-memory.py actually took effect - the patch
# compiling proves nothing about what the driver reports.
driver_extmem_probe: ./src/tests/driver_extmem_probe/driver_extmem_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_extmem_probe $<

# Drives the kbase event-memory vk_sync through the real Vulkan API:
# timeline semaphore signal/get/wait and binary fence status/reset. Proves
# the sync type works, not merely that it registered.
# The other half of tests/dmabuf_import_probe: the same import, but through
# the real Vulkan entry points with the GPU doing the writing. Reads back
# through an independent mmap of the dma-buf rather than vkMapMemory, which
# is what makes it proof rather than a tautology. Shares the fd-source code
# with dmabuf_import_probe so there is one place that knows how to get an fd.
driver_dmabuf_probe: ./src/tests/driver_dmabuf_probe/driver_dmabuf_probe.c
	$(CC) $(CFLAGS) -I./src/tests/dmabuf_import_probe -o ./build/driver_dmabuf_probe $<

driver_sync_probe: ./src/tests/driver_sync_probe/driver_sync_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_sync_probe $<

# The first command buffer to reach the GPU on kbase. Staged: recording only
# by default (cannot hang the device), --submit kicks a CALLed stream for the
# first time, --fill runs a real shader and checks the result. Run them as
# separate invocations, in that order.
driver_compute_probe: ./src/tests/driver_compute_probe/driver_compute_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_compute_probe $<

# A real compute pipeline from application SPIR-V: descriptor sets, push
# constants, vkCmdDispatch. driver_compute_probe --fill only reaches PanVK's
# own precompiled shaders, so this is the first thing to drive SPIR-V through
# the driver's own compiler.
#
# --burst=N additionally submits N times back to back with a single fence at
# the end. That is the only workload here that can show whether submits
# overlap - every other probe waits per submit, which serialises the queue
# from the application side and hides what the driver does.
driver_pipeline_probe: ./src/tests/driver_pipeline_probe/driver_pipeline_probe.c
	$(CC) $(CFLAGS) -I./src/tests/driver_pipeline_probe -o ./build/driver_pipeline_probe $<

# The first real VERTEX_TILER/FRAGMENT execution ever attempted on kbase in
# this repo - a render pass with LOAD_OP_CLEAR and no draw calls, gated the
# way tests/alias_cs_probe is. Requires --i-know-it-hangs to run. See the
# file header before running this on a device.
render_clear_probe: ./src/tests/render_clear_probe/render_clear_probe.c
	$(CC) $(CFLAGS) -o ./build/render_clear_probe $<

# A real triangle - vertex shader, IDVS, tiling, fragment shader - the step
# after render_clear_probe proved render-pass entry works. Same
# --i-know-it-hangs gate, for the same reason: nothing here has run on this
# device before either.
render_triangle_probe: ./src/tests/render_triangle_probe/render_triangle_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_triangle_probe -o ./build/render_triangle_probe $<

# Regenerate the embedded triangle shaders. Only needed after editing
# triangle.vert/.frag - the generated headers are committed so a normal
# build needs no glslang.
render_triangle_probe_shaders:
	cd ./src/tests/render_triangle_probe && \
	  glslangValidator -V triangle.vert -o triangle_vert.spv && \
	  glslangValidator -V triangle.frag -o triangle_frag.spv && \
	  python3 ../../../tools/spv_to_header.py triangle_vert.spv \
	    triangle_vert_spv.h render_triangle_probe_vert && \
	  python3 ../../../tools/spv_to_header.py triangle_frag.spv \
	    triangle_frag_spv.h render_triangle_probe_frag

# One variable changed from render_triangle_probe: positions come from a
# real bound VkBuffer instead of a hardcoded shader array. Same
# --i-know-it-hangs gate - vertex fetch has not run on this device before
# either.
render_vbo_probe: ./src/tests/render_vbo_probe/render_vbo_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_vbo_probe -o ./build/render_vbo_probe $<

render_vbo_probe_shaders:
	cd ./src/tests/render_vbo_probe && \
	  glslangValidator -V vbo.vert -o vbo_vert.spv && \
	  glslangValidator -V vbo.frag -o vbo_frag.spv && \
	  python3 ../../../tools/spv_to_header.py vbo_vert.spv \
	    vbo_vert_spv.h render_vbo_probe_vert && \
	  python3 ../../../tools/spv_to_header.py vbo_frag.spv \
	    vbo_frag_spv.h render_vbo_probe_frag

# One variable changed from render_vbo_probe: the fragment colour comes
# from a push constant instead of being hardcoded. Same --i-know-it-hangs
# gate - push constants have not reached a graphics-stage fragment shader
# on this device before (compute already works, per driver_pipeline_probe,
# but that is different code).
render_push_probe: ./src/tests/render_push_probe/render_push_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_push_probe -o ./build/render_push_probe $<

render_push_probe_shaders:
	cd ./src/tests/render_push_probe && \
	  glslangValidator -V push.vert -o push_vert.spv && \
	  glslangValidator -V push.frag -o push_frag.spv && \
	  python3 ../../../tools/spv_to_header.py push_vert.spv \
	    push_vert_spv.h render_push_probe_vert && \
	  python3 ../../../tools/spv_to_header.py push_frag.spv \
	    push_frag_spv.h render_push_probe_frag

# One variable changed from render_push_probe: the fragment colour comes
# from a uniform buffer through a real descriptor set instead of a push
# constant - the last basic plumbing mechanism this port had not
# exercised. Same --i-know-it-hangs gate.
render_ubo_probe: ./src/tests/render_ubo_probe/render_ubo_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_ubo_probe -o ./build/render_ubo_probe $<

render_ubo_probe_shaders:
	cd ./src/tests/render_ubo_probe && \
	  glslangValidator -V ubo.vert -o ubo_vert.spv && \
	  glslangValidator -V ubo.frag -o ubo_frag.spv && \
	  python3 ../../../tools/spv_to_header.py ubo_vert.spv \
	    ubo_vert_spv.h render_ubo_probe_vert && \
	  python3 ../../../tools/spv_to_header.py ubo_frag.spv \
	    ubo_frag_spv.h render_ubo_probe_frag

# One variable changed from render_ubo_probe: the descriptor is a combined
# image sampler bound to a real 1x1 texture, instead of a uniform buffer -
# a genuinely different hardware path (image sampling). Same
# --i-know-it-hangs gate.
render_texture_probe: ./src/tests/render_texture_probe/render_texture_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_texture_probe -o ./build/render_texture_probe $<

render_texture_probe_shaders:
	cd ./src/tests/render_texture_probe && \
	  glslangValidator -V texture.vert -o texture_vert.spv && \
	  glslangValidator -V texture.frag -o texture_frag.spv && \
	  python3 ../../../tools/spv_to_header.py texture_vert.spv \
	    texture_vert_spv.h render_texture_probe_vert && \
	  python3 ../../../tools/spv_to_header.py texture_frag.spv \
	    texture_frag_spv.h render_texture_probe_frag

# One variable changed from render_vbo_probe: a real depth attachment,
# depth test and write enabled - the Z-test unit, untested until now. Same
# --i-know-it-hangs gate.
render_depth_probe: ./src/tests/render_depth_probe/render_depth_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_depth_probe -o ./build/render_depth_probe $<

render_depth_probe_shaders:
	cd ./src/tests/render_depth_probe && \
	  glslangValidator -V depth.vert -o depth_vert.spv && \
	  glslangValidator -V depth.frag -o depth_frag.spv && \
	  python3 ../../../tools/spv_to_header.py depth_vert.spv \
	    depth_vert_spv.h render_depth_probe_vert && \
	  python3 ../../../tools/spv_to_header.py depth_frag.spv \
	    depth_frag_spv.h render_depth_probe_frag

# Two draws in one render pass, state (a push constant) changed between
# them - the last basic plumbing mechanism this session's probes had not
# exercised. Same --i-know-it-hangs gate.
render_multidraw_probe: ./src/tests/render_multidraw_probe/render_multidraw_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_multidraw_probe -o ./build/render_multidraw_probe $<

render_multidraw_probe_shaders:
	cd ./src/tests/render_multidraw_probe && \
	  glslangValidator -V multidraw.vert -o multidraw_vert.spv && \
	  glslangValidator -V multidraw.frag -o multidraw_frag.spv && \
	  python3 ../../../tools/spv_to_header.py multidraw_vert.spv \
	    multidraw_vert_spv.h render_multidraw_probe_vert && \
	  python3 ../../../tools/spv_to_header.py multidraw_frag.spv \
	    multidraw_frag_spv.h render_multidraw_probe_frag

# ROADMAP.md Phase 5: the first probe to touch multisampling/resolve at
# all. Same triangle as render_vbo_probe, but the colour attachment is
# multisampled with storeOp=NONE and resolves (VK_RESOLVE_MODE_AVERAGE_BIT)
# to a second, single-sample image that gets read back - exactly the
# in-tile-memory resolve path PanVK's rewritten framebuffer abstraction
# added (see the probe's own header comment). Same --i-know-it-hangs gate.
render_msaa_probe: ./src/tests/render_msaa_probe/render_msaa_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_msaa_probe -o ./build/render_msaa_probe $<

render_msaa_probe_shaders:
	cd ./src/tests/render_msaa_probe && \
	  glslangValidator -V msaa.vert -o msaa_vert.spv && \
	  glslangValidator -V msaa.frag -o msaa_frag.spv && \
	  python3 ../../../tools/spv_to_header.py msaa_vert.spv \
	    msaa_vert_spv.h render_msaa_probe_vert && \
	  python3 ../../../tools/spv_to_header.py msaa_frag.spv \
	    msaa_frag_spv.h render_msaa_probe_frag

# Tests whether the cold-start bug found in CTS's
# record_many_draws_secondary_2 (fails 100% of the time as the FIRST
# secondary-command-buffer draw in a process, passes after any other
# secondary draw ran first - see docs/kbase-notes.md) also covers
# many_indirect_draws_on_secondary. deqp-vk's own fixed alphabetical case
# order made this untestable through CTS case selection alone. Same
# --i-know-it-hangs gate as every render probe, though the actual risk is
# lower here - deqp-vk has already run this exact combination (secondary
# buffer + indirect draw) many times this session without hanging.
render_secondary_warmup_probe: ./src/tests/render_secondary_warmup_probe/render_secondary_warmup_probe.c
	$(CC) $(CFLAGS) -I./src/tests/render_secondary_warmup_probe -o ./build/render_secondary_warmup_probe $<

render_secondary_warmup_probe_shaders:
	cd ./src/tests/render_secondary_warmup_probe && \
	  glslangValidator -V warmup.vert -o warmup_vert.spv && \
	  glslangValidator -V warmup.frag -o warmup_frag.spv && \
	  python3 ../../../tools/spv_to_header.py warmup_vert.spv \
	    warmup_vert_spv.h warmup_probe_vert && \
	  python3 ../../../tools/spv_to_header.py warmup_frag.spv \
	    warmup_frag_spv.h warmup_probe_frag

# Semaphores: creation, a binary chain between two submits, and a timeline
# value the GPU has to write exactly. vkCreateSemaphore used to fail outright,
# so nothing before this could order any work. Reuses driver_pipeline_probe's
# shader and its generated header rather than carrying a second copy.
driver_semaphore_probe: ./src/tests/driver_semaphore_probe/driver_semaphore_probe.c
	$(CC) $(CFLAGS) -I./src/tests/driver_pipeline_probe -o ./build/driver_semaphore_probe $<

# Regenerate the embedded SPIR-V. Only needed after editing shader.comp - the
# generated header is committed so a normal build needs no glslang.
pipeline_probe_shader:
	cd ./src/tests/driver_pipeline_probe && \
	  glslangValidator -V shader.comp -o shader.spv && \
	  python3 ../../../tools/spv_to_header.py shader.spv shader_spv.h \
	    pipeline_probe_shader

# Settles what a SAME_VA kbase BO can be re-mmap-ed with (cookie vs resolved
# address). Decides kbase_kmod_bo_get_mmap_offset()'s implementation.
# Can kbase map one allocation at two adjacent GPU VAs (KBASE_IOCTL_MEM_ALIAS)?
# Decides whether PanVK's render descriptor ringbuf - which relies on that
# double mapping to wrap - is expressible on kbase at all, and so whether the
# VERTEX_TILER/FRAGMENT subqueues can be initialised.
alias_probe: ./src/tests/alias_probe/alias_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/alias_probe $<

# Which number is an allocation's real GPU address - out.gpu_va, or the CPU
# pointer? Answers it per allocation kind (plain / GPU_EX / FIXABLE) using
# MEM_ALIAS as an oracle, and exits non-zero if the rule kbase_bo_create()
# encodes does not hold. Re-run this when moving to a new device or a new
# KBASE_VERSION. Upstreamed as Joshua-Micheletti/PanVK2KBase#2.
same_va_probe: ./src/tests/same_va_probe/same_va_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/same_va_probe $<

# The other half of the aliasing question: alias_probe shows MEM_ALIAS
# composes the region, this shows the two windows are the same pages. Needs
# a command stream because the CPU cannot reach window 1 at all - see the
# file header.
alias_cs_probe: ./src/tests/alias_cs_probe/alias_cs_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/alias_cs_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

remap_probe: ./src/tests/remap_probe/remap_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/remap_probe $<

# Answers whether kbase can allocate at a caller-chosen GPU VA
# (KBASE_IOCTL_MEM_ALLOC_EX + BASE_MEM_FIXED). Decides whether pan_kmod's
# vm_bind contract is implementable at all - see docs/kbase-notes.md.
fixed_va_probe: ./src/tests/fixed_va_probe/fixed_va_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/fixed_va_probe $<

# Establishes that KBASE_IOCTL_VERSION_CHECK is once-per-fd.
# Can kbase import a dma-buf on this device, and is the gpu_va it returns a
# real address or an mmap cookie? Answers the "does your kernel's kbase
# expose the ioctls you'll need for dma-buf import?" box that has been open
# in docs/kbase-notes.md since Phase 1, and gates the Mesa-side import work.
#
# Deliberately does NO GPU work at all - no queue group, no kick, no command
# stream - which is why it needs no --i-know-it-hangs gate and is safe for
# tools/run-probes.sh to run unattended. See the file header.
dmabuf_import_probe: ./src/tests/dmabuf_import_probe/dmabuf_import_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -I./src/tests/dmabuf_import_probe -o ./build/dmabuf_import_probe $<

double_handshake_probe: ./src/tests/double_handshake_probe/double_handshake_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/double_handshake_probe $<

# --- Mesa CS instruction encoder (see docs/mesa-cs-builder.md) ---
# Builds real Mali CSF instructions via Mesa's own encoder
# (cs_builder.h) instead of sentinel bytes - see docs/kbase-notes.md's
# "poll()/read() on the kbase fd" section for why. Needs a real Mesa
# checkout, not vendored in this repo (see docs/mesa-cs-builder.md for
# the clone command - it's gitignored, ~600MB, local to whichever
# machine clones it) and a Python interpreter for the genxml codegen
# step below.
MESA_DIR ?= third_party/MESA-KMOD
# llvm-nm from the same toolchain as CC, for the backend symbol check.
NM ?= llvm-nm
# Must match the target GPU's architecture major version, e.g. 12 for
# this repo's Poco X8 Pro/Mali-G720 target (see utils/parse_gpu_props.h's
# decode). A different device needs a different PAN_ARCH.
PAN_ARCH ?= 12
PYTHON ?= python3
MESA_GENXML := $(MESA_DIR)/src/panfrost/genxml
MESA_PACK_H := $(MESA_GENXML)/v$(PAN_ARCH)_pack.h
MESA_CS_INCLUDES := -I$(MESA_DIR)/src/panfrost -I$(MESA_DIR)/src -I$(MESA_DIR)/src/util -I$(MESA_DIR)/include
# Mesa's C11 threads/time compat shims (src/c11/) need to be told
# explicitly what the platform supports - normally supplied by meson.
# Android and Linux both have real pthreads and a real struct timespec.
MESA_CS_DEFS := -DPAN_ARCH=$(PAN_ARCH) -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC
# Dead-code-strip so only what cs_builder.h actually calls
# (reralloc_size, util_dynarray_is_data_stack_allocated) gets linked,
# not the GPU-shader-printf machinery ralloc.c would otherwise
# transitively pull in via whole-object linking - see
# docs/mesa-cs-builder.md.
MESA_CS_GC := -ffunction-sections -fdata-sections -Wl,--gc-sections

.PHONY: mesa-cs-pack

mesa-cs-pack: $(MESA_PACK_H)

$(MESA_PACK_H):
	@test -d "$(MESA_DIR)" || { echo "error: $(MESA_DIR) not found - see docs/mesa-cs-builder.md for the clone command"; exit 1; }
	cd $(MESA_GENXML) && $(PYTHON) gen_pack.py v$(PAN_ARCH).xml > v$(PAN_ARCH)_pack.h

cs_encode_probe: ./src/tests/cs_encode_probe/cs_encode_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/cs_encode_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Combines the kbase ioctl surface with Mesa's CS encoder - builds a
# real instruction, writes it through the real CS_INSERT/KICK protocol
# (see utils/csf_user_regs.h), and checks for a real CSF notification.
live_kick_probe: ./src/tests/live_kick_probe/live_kick_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/live_kick_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Can a CS pick up ring appends made while it is still executing? The rule
# that a kick only lands on an idle CS was measured in a state where the
# stream had already finished; this separates that from a CS genuinely
# mid-execution, which is what decides whether submissions can overlap.
kick_pipeline_probe: ./src/tests/kick_pipeline_probe/kick_pipeline_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/kick_pipeline_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Determines empirically which of CS_QUEUE_BIND's 3 mmap'd pages is
# input/output/doorbell, rather than assuming. Exists because Panfork (a
# Panfrost driver that ran on real kbase hardware) disagrees with
# utils/csf_user_regs.h by one page - see "Prior art found" in
# docs/kbase-notes.md. Same setup as live_kick_probe so results are
# comparable.
user_io_probe: ./src/tests/user_io_probe/user_io_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/user_io_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Gets a real completion signal out of the GPU: BASE_MEM_CSF_EVENT memory
# plus a SYNC_SET64 emitted into the command stream. This is the mechanism
# a non-DRM vk_sync has to be built on - see "Finding 2" in
# docs/kbase-notes.md.
event_slot_probe: ./src/tests/event_slot_probe/event_slot_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/event_slot_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# --- Phase 2: pan_kmod_kbase Mesa backend (see src/mesa/README.md) ---
# The Mesa checkout is gitignored, so the backend source lives in this repo
# and is synced into it. MESA_DIR/PAN_ARCH/KBASE_VERSION are shared with the
# cs_encode_probe targets above.
MESA_KMOD_DIR := $(MESA_DIR)/src/panfrost/lib/kmod

.PHONY: mesa-backend-sync mesa-backend-check

mesa-backend-sync:
	@test -d "$(MESA_KMOD_DIR)" || { echo "error: $(MESA_KMOD_DIR) not found - see docs/mesa-cs-builder.md for the Mesa clone command"; exit 1; }
	cp src/mesa/pan_kmod_kbase.c src/mesa/pan_kmod_kbase.h $(MESA_KMOD_DIR)/
	cp src/utils/csf_user_regs.h $(MESA_KMOD_DIR)/
	cp src/mesa/panvk_kbase_sync.c src/mesa/panvk_kbase_sync.h $(MESA_DIR)/src/panfrost/vulkan/
	cp src/mesa/panvk_vX_kbase_queue.c $(MESA_DIR)/src/panfrost/vulkan/csf/
	@echo ""
	@echo "Copied pan_kmod_kbase.{c,h} and csf_user_regs.h into $(MESA_KMOD_DIR)/"
	@echo "Copied panvk_kbase_sync.{c,h} into $(MESA_DIR)/src/panfrost/vulkan/"
	@echo "Still to apply (kept as readable patches since upstream moves):"
	@echo "  - src/mesa/pan_kmod.c.kbase.patch      -> $(MESA_KMOD_DIR)/pan_kmod.c"
	@echo "  - src/mesa/meson.build.kbase.patch     -> $(MESA_KMOD_DIR)/meson.build"
	@echo "  - src/mesa/patch-panvk-kbase-enumeration.py <mesa-dir>  (PanVK enumeration)"
	@echo "  - src/mesa/patch-panvk-kbase-sync.py        <mesa-dir>  (PanVK vk_sync)"
	@echo "  - src/mesa/patch-panthor-csif-dispatch.py   <mesa-dir>  (csif props)"
	@echo "  - src/mesa/patch-panvk-kbase-queue.py       <mesa-dir>  (GPU queue)"
	@echo "  - src/mesa/patch-panvk-kbase-subqueue-init.py <mesa-dir> (subqueue init)"
	@echo "  - src/mesa/patch-pan-kmod-import-fd.py      <mesa-dir>  (dma-buf import hook)"
	@echo "  - src/mesa/patch-panvk-kbase-external-memory.py <mesa-dir> (import/export capability)"
	@echo "  - src/mesa/patch-panvk-null-device-destroy.py <mesa-dir> (null-handle vkDestroyDevice, not kbase-specific)"

# Real libdrm, fetched via Mesa's own meson wrap (pan_kmod.h includes
# <xf86drm.h>, and a shallow clone doesn't fetch subprojects). Falls back to
# the minimal stub in src/mesa/syntax-check-stubs/ when absent.
MESA_LIBDRM := $(firstword $(wildcard $(MESA_DIR)/subprojects/libdrm-*))
ifeq ($(MESA_LIBDRM),)
DRM_INC := -Isrc/mesa/syntax-check-stubs
DRM_KIND := stubbed libdrm
else
DRM_INC := -I$(MESA_LIBDRM) -I$(MESA_LIBDRM)/include/drm
DRM_KIND := real libdrm ($(notdir $(MESA_LIBDRM)))
endif

MESA_BACKEND_INC := $(DRM_INC) \
  -I$(MESA_DIR)/src/panfrost/lib -I$(MESA_KMOD_DIR) \
  -I$(MESA_DIR)/src/panfrost/model -I$(MESA_DIR)/src/panfrost/perf \
  -I$(MESA_DIR)/src/panfrost -I$(MESA_DIR)/src -I$(MESA_DIR)/src/util \
  -I$(MESA_DIR)/include -I$(KBASE_UAPI_DIR)

.PHONY: mesa-libdrm

# Fetch real libdrm into the Mesa checkout via its own pinned wrap.
mesa-libdrm:
	cd $(MESA_DIR) && $(PYTHON) -m mesonbuild.mesonmain subprojects download libdrm

# Compiles the backend to a real object file (not just -fsyntax-only)
# against the real Mesa and kbase headers, then checks that the kbase
# symbols pan_kmod.c needs are exactly the ones the backend defines. This
# is a compile + symbol-resolution check, NOT a full Mesa build - see
# src/mesa/README.md for why a full build needs LLVM.
#
# The -DHAVE_* defines stand in for the config header meson generates and
# this hand-rolled compile does not have. HAVE_ENDIAN_H is what lets
# util/u_endian.h consult glibc's <endian.h>; without it that header hits
# its own #error before the compiler ever reaches backend code.
mesa-backend-check: mesa-backend-sync
	@mkdir -p build/mesa-backend
	@echo "using: $(DRM_KIND)"
	$(CC) -c -O1 -Wall $(MALIFLAGS) -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC \
	  -DHAVE_ENDIAN_H \
	  -include src/utils/kconfig_shim.h $(MESA_BACKEND_INC) \
	  -o build/mesa-backend/pan_kmod_kbase.o \
	  $(MESA_KMOD_DIR)/pan_kmod_kbase.c
	@echo "pan_kmod_kbase.c: compiles clean against real Mesa + kbase headers"
	@echo "defines: $$($(NM) --defined-only -g build/mesa-backend/pan_kmod_kbase.o | awk '{print $$NF}' | tr '\n' ' ')"

# Bridges standard-Vulkan-loader-ABI tools (deqp-vk, in particular) to this
# repo's driver, which speaks Android's hwvulkan HAL ABI instead - see the
# file header for why that gap exists and why this is the safe way to
# close it (not installing the driver as the system Vulkan HAL). Needs the
# Vulkan headers, which is why this links against -Ithird_party/mesa/
# include if present, falling back to the NDK's own <vulkan/vulkan_core.h>.
# Cross-compile with the NDK, same as the driver_*/render_*_probe targets -
# this is not a native/WSL build.
icd_shim: ./src/tests/icd_shim/panvk_kbase_icd_shim.c
	$(CC) $(CFLAGS) -shared -fPIC \
	  -o ./build/libpanvk_kbase_icd_shim.so $< -ldl

clean:
	rm -f first_test