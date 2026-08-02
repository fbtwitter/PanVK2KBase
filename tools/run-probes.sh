#!/usr/bin/env bash
#
# The regression runner this repo did not have.
#
# Until now, "did that change break anything?" was answered by hand-running
# ~20 probes and remembering which ones matter. That was tolerable while the
# backend was being built; it stops being tolerable the moment something
# edits pan_kmod_kbase.c, which every working capability - compute,
# rendering, sync - runs through.
#
# What this does: builds, pushes (md5-verified), and runs a declared set of
# probes on the attached device with per-probe timeouts, interleaves device
# health checks, and prints a pass/fail summary. Exit 0 iff everything
# selected passed.
#
# What this deliberately does NOT do: run tests/alias_cs_probe. See the
# manifest below - that exclusion is enforced, not decorative.
#
# Usage:
#   bash tools/run-probes.sh [options]
#
#     --tier=raw,driver     which tiers to run (default: raw,driver)
#     --with-render         also run the render tier (passes --i-know-it-hangs)
#     --only=a,b            run only these probes by name
#     --list                print the manifest and exit
#     --skip-build          use whatever is already in build/ and on-device
#     --so=PATH             on-device driver path
#                           (default /data/local/tmp/libvulkan_panfrost.so)
#
# Exit codes:  0 = all selected passed
#              1 = one or more FAILED or TIMED OUT
#              2 = ABORTED (pre-flight refused, or a health check failed -
#                  these mean "look at the device", not "look at the diff")
#
# Host: WSL is canonical (no path mangling). Git Bash works - MSYS_NO_PATHCONV
# is forced below - but see docs/kbase-notes.md on adb path rewriting there.

set -uo pipefail
export MSYS_NO_PATHCONV=1   # Git Bash rewrites adb remote paths; see header.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 2

# ---------------------------------------------------------------- manifest
#
# name | tier | make-target | build-env | argv | timeout_s
#
# tiers:
#   raw     - kbase ioctl probes that never make the GPU execute anything:
#             no CS_QUEUE_KICK, no command stream, no doorbell. That is what
#             makes "unattended-safe" an earned claim rather than a hopeful
#             one, and it is why queue_group/fence_probe/event_probe are in
#             'manual' below despite passing - they call CS_QUEUE_KICK, and
#             the fact that the kick is inert (they never write CS_INSERT, so
#             the ring reads empty) is a property worth not depending on.
#             Need KBASE_VERSION=r49p1 explicitly: the makefile defaults to
#             r44p0 and would silently build against the wrong UAPI.
#   driver  - load the built Vulkan driver and drive it through the real API.
#   render  - as driver, but enter a render pass. Opt-in (--with-render) and
#             followed by a health check, because these carry
#             --i-know-it-hangs for a reason even though all of them pass.
#   manual  - real regression value, but either drives the GPU directly
#             (cs_builder streams, live kicks) or is an open investigation.
#             Built and pushed on request, never auto-run.
#   never   - see below.
#
# @SO@ is substituted with the on-device driver path.
#
read -r -d '' MANIFEST <<'EOF'
first_test             | raw    | first_test             | KBASE_VERSION=r49p1 |                            | 30
glb_iface_probe        | raw    | glb_iface_probe        | KBASE_VERSION=r49p1 |                            | 30
memory                 | raw    | memory                 | KBASE_VERSION=r49p1 |                            | 30
memory2                | raw    | memory2                | KBASE_VERSION=r49p1 |                            | 30
double_handshake_probe | raw    | double_handshake_probe | KBASE_VERSION=r49p1 |                            | 30
same_va_probe          | raw    | same_va_probe          | KBASE_VERSION=r49p1 |                            | 60
remap_probe            | raw    | remap_probe            | KBASE_VERSION=r49p1 |                            | 60
fixed_va_probe         | raw    | fixed_va_probe         | KBASE_VERSION=r49p1 |                            | 60
alias_probe            | raw    | alias_probe            | KBASE_VERSION=r49p1 |                            | 60
dmabuf_import_probe    | raw    | dmabuf_import_probe    | KBASE_VERSION=r49p1 | --source=heap              | 60
sync_fd_probe          | raw    | sync_fd_probe          | KBASE_VERSION=r49p1 |                            | 60
driver_load_probe      | driver | driver_load_probe      | NDK                 | @SO@                       | 30
driver_enum_probe      | driver | driver_enum_probe      | NDK                 | @SO@                       | 60
driver_extmem_probe    | driver | driver_extmem_probe    | NDK                 | @SO@                       | 60
driver_sync_probe      | driver | driver_sync_probe      | NDK                 | @SO@                       | 90
driver_compute_probe   | driver | driver_compute_probe   | NDK                 | @SO@ --submit --fill       | 120
driver_pipeline_probe  | driver | driver_pipeline_probe  | NDK                 | @SO@                       | 120
driver_semaphore_probe | driver | driver_semaphore_probe | NDK                 | @SO@                       | 120
driver_dmabuf_probe    | driver | driver_dmabuf_probe    | NDK                 | @SO@ --source=heap         | 120
driver_android_wsi_probe | driver | driver_android_wsi_probe | NDK             | @SO@                       | 120
driver_namespace_probe | driver | driver_namespace_probe | NDK                 | @SO@                       | 120
render_clear_probe     | render | render_clear_probe     | NDK                 | @SO@ --i-know-it-hangs     | 120
render_triangle_probe  | render | render_triangle_probe  | NDK                 | @SO@ --i-know-it-hangs     | 120
render_vbo_probe       | render | render_vbo_probe       | NDK                 | @SO@ --i-know-it-hangs     | 120
render_push_probe      | render | render_push_probe      | NDK                 | @SO@ --i-know-it-hangs     | 120
render_ubo_probe       | render | render_ubo_probe       | NDK                 | @SO@ --i-know-it-hangs     | 120
render_texture_probe   | render | render_texture_probe   | NDK                 | @SO@ --i-know-it-hangs     | 120
render_depth_probe     | render | render_depth_probe     | NDK                 | @SO@ --i-know-it-hangs     | 120
render_multidraw_probe | render | render_multidraw_probe | NDK                 | @SO@ --i-know-it-hangs     | 120
render_msaa_probe      | render | render_msaa_probe      | NDK                 | @SO@ --i-know-it-hangs     | 120
queue_group            | manual | queue_group            | KBASE_VERSION=r49p1 |                            | 60
fence_probe            | manual | fence_probe            | KBASE_VERSION=r49p1 |                            | 60
event_probe            | manual | event_probe            | KBASE_VERSION=r49p1 |                            | 60
cs_encode_probe        | manual | cs_encode_probe        | KBASE_VERSION=r49p1 |                            | 60
live_kick_probe        | manual | live_kick_probe        | KBASE_VERSION=r49p1 |                            | 120
user_io_probe          | manual | user_io_probe          | KBASE_VERSION=r49p1 |                            | 120
event_slot_probe       | manual | event_slot_probe       | KBASE_VERSION=r49p1 |                            | 120
kick_pipeline_probe    | manual | kick_pipeline_probe    | KBASE_VERSION=r49p1 |                            | 120
render_secondary_warmup_probe | manual | render_secondary_warmup_probe | NDK | @SO@ --i-know-it-hangs | 180
driver_present_loop_probe | manual | driver_present_loop_probe | NDK       | @SO@ --i-know-it-hangs     | 300
# ---------------------------------------------------------------------------
# NEVER RUN. Not "not yet" - never. Not by this script, not by --only, and
# there is no flag that overrides it.
#
# alias_cs_probe has wedged the kbase context both times it was run on real
# hardware: the process ends in uninterruptible D state, kill -9 will not
# reap it, and the phone needs a physical reboot. The cause is still not
# understood - BASE_MEM_NEED_MMAP was the obvious suspect, the probe now
# checks for it, and it hung again anyway.
#
# Its own file header says it must never be run by a script walking the
# probe list. This line is that promise, written down and enforced by
# assert_never_row_intact() below.
alias_cs_probe         | never  | alias_cs_probe         | KBASE_VERSION=r49p1 | --i-know-it-hangs          | 0
EOF

# ------------------------------------------------------------------ config
TIERS="raw,driver"
ONLY=""
LIST_ONLY=0
SKIP_BUILD=0
SO="/data/local/tmp/libvulkan_panfrost.so"
DEVDIR="/data/local/tmp"
NDK_CC="${NDK_CC:-/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang}"

for arg in "$@"; do
  case "$arg" in
    --tier=*)     TIERS="${arg#*=}" ;;
    --with-render) TIERS="$TIERS,render" ;;
    --only=*)     ONLY="${arg#*=}" ;;
    --list)       LIST_ONLY=1 ;;
    --skip-build) SKIP_BUILD=1 ;;
    --so=*)       SO="${arg#*=}" ;;
    -h|--help)    sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

# ----------------------------------------------------------------- helpers
c_red=$'\033[31m'; c_grn=$'\033[32m'; c_yel=$'\033[33m'; c_off=$'\033[0m'
[ -t 1 ] || { c_red=""; c_grn=""; c_yel=""; c_off=""; }

say()  { printf '%s\n' "$*"; }
fail() { printf '%s\n' "${c_red}$*${c_off}" >&2; }

manifest_rows() {
  # strip comments and blank lines, normalise whitespace around |
  printf '%s\n' "$MANIFEST" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$'
}

field() { printf '%s' "$1" | awk -F'|' -v n="$2" '{gsub(/^[ \t]+|[ \t]+$/,"",$n); print $n}'; }

# The exclusion is only real if removing it is noisy. If someone "tidies the
# manifest" by deleting the alias_cs_probe row, this refuses to run at all
# rather than quietly losing the protection.
assert_never_row_intact() {
  local row tier
  row="$(manifest_rows | awk -F'|' '$1 ~ /alias_cs_probe/')"
  if [ -z "$row" ]; then
    fail "REFUSING TO RUN: the alias_cs_probe row is missing from the manifest."
    fail "That row is a safety interlock, not a list entry - it is what keeps"
    fail "a probe that has twice required a device reboot out of automated runs."
    fail "Restore it (tier 'never') before using this script."
    exit 2
  fi
  tier="$(field "$row" 2)"
  if [ "$tier" != "never" ]; then
    fail "REFUSING TO RUN: alias_cs_probe is listed as tier '$tier', not 'never'."
    fail "It wedges the kbase context and needs a physical reboot. Set it back."
    exit 2
  fi
}

# Under WSL the Linux $USER is not the Windows user, so guessing
# /mnt/c/Users/$USER/... does not work. Glob instead, and let ADB= override.
find_adb() {
  if [ -n "${ADB:-}" ] && [ -x "${ADB}" ]; then echo "$ADB"; return; fi
  if command -v adb >/dev/null 2>&1; then echo adb; return; fi
  if command -v adb.exe >/dev/null 2>&1; then echo adb.exe; return; fi
  local p
  for p in \
    /mnt/c/Users/*/AppData/Local/Android/Sdk/platform-tools/adb.exe \
    /c/Users/*/AppData/Local/Android/Sdk/platform-tools/adb.exe \
    "$HOME"/AppData/Local/Android/Sdk/platform-tools/adb.exe; do
    [ -x "$p" ] && { echo "$p"; return; }
  done
  echo ""
}

ADB="$(find_adb)"

adbsh() { "$ADB" shell "$@" 2>/dev/null | tr -d '\r'; }

# Under WSL the adb we find is usually the Windows adb.exe, which cannot
# resolve /mnt/c/... - it needs a real Windows path. Everything else (md5sum,
# file tests) still uses the Linux path, so translate only at the adb call.
hostpath() {
  case "$ADB" in
    *.exe)
      if command -v wslpath >/dev/null 2>&1; then wslpath -w "$1"; else printf '%s' "$1"; fi ;;
    *) printf '%s' "$1" ;;
  esac
}

# ---------------------------------------------------------------- pre-flight
preflight() {
  [ -n "$ADB" ] || { fail "no adb found (PATH, or the Android SDK platform-tools)"; exit 2; }

  local state
  state="$("$ADB" get-state 2>&1 | tr -d '\r')"
  [ "$state" = "device" ] || { fail "adb get-state = '$state' (need 'device')"; exit 2; }

  local n
  n="$("$ADB" devices | tr -d '\r' | grep -c '\sdevice$')"
  if [ "$n" -ne 1 ] && [ -z "${ANDROID_SERIAL:-}" ]; then
    fail "$n devices attached; set ANDROID_SERIAL to pick one"; exit 2
  fi

  adbsh "ls /dev/mali0" | grep -q mali0 || { fail "/dev/mali0 not present"; exit 2; }

  # Stale-wedge check. MUST be scoped to /data/local/tmp: this device
  # permanently has hang_detect, wdtk-0..7 and cmdq_buffer_usage in D state,
  # so an unscoped 'ps | grep D' would refuse to run 100% of the time.
  local wedged
  wedged="$(adbsh "ps -A -o stat= -o cmd= 2>/dev/null | grep '^D' | grep $DEVDIR")"
  if [ -n "$wedged" ]; then
    fail "A previous run appears to have wedged the kbase context:"
    fail "$wedged"
    fail "kill -9 will not clear this. Reboot the device before running again."
    exit 2
  fi
}

# --------------------------------------------------------------- run stages
STAMP="$(date +%Y%m%d-%H%M%S)"
LOGDIR="$REPO/build/regress/$STAMP"

declare -a R_NAME R_STATUS
PASS=0; FAILED=0; SKIPPED=0

build_one() {
  local target="$1" env="$2" log="$3"
  local -a mk=(make "$target")
  if [ "$env" = "NDK" ]; then
    mk=(env "CC=$NDK_CC" make "$target")
  else
    mk=(env "CC=$NDK_CC" "$env" make "$target")
  fi
  "${mk[@]}" >>"$log" 2>&1 </dev/null
}

push_one() {
  local name="$1" log="$2"
  local local_bin="$REPO/build/$name"
  [ -f "$local_bin" ] || { echo "no built binary at build/$name" >>"$log"; return 1; }
  "$ADB" push "$(hostpath "$local_bin")" "$DEVDIR/$name" >>"$log" 2>&1 || return 1
  # md5 both ends: a mangled push still reports success and leaves a stale
  # binary running, which has made two earlier conclusions in this repo wrong.
  local lm dm
  lm="$(md5sum "$local_bin" | cut -d' ' -f1)"
  dm="$(adbsh "md5sum $DEVDIR/$name" | awk '{print $1}')"
  if [ "$lm" != "$dm" ]; then
    echo "md5 mismatch: local=$lm device=$dm" >>"$log"; return 1
  fi
  adbsh "chmod 755 $DEVDIR/$name" >/dev/null
  return 0
}

run_one() {
  local name="$1" argv="$2" timeout_s="$3" log="$4"
  local out code
  # Timeout runs ON THE DEVICE. A host-side timeout would kill adb and leave
  # the device-side process running, which is how you end up with a wedged
  # context you did not notice.
  out="$("$ADB" shell "cd $DEVDIR && timeout $timeout_s ./$name $argv 2>&1; echo __EXIT=\$?" 2>&1 </dev/null | tr -d '\r')"
  printf '%s\n' "$out" >>"$log"
  code="$(printf '%s\n' "$out" | sed -n 's/^__EXIT=//p' | tail -1)"
  [ -n "$code" ] || code=999
  echo "$code"
}

health_check() {
  local log="$1"
  local code
  echo "--- health check: driver_compute_probe --submit --fill ---" >>"$log"
  code="$(run_one driver_compute_probe "$SO --submit --fill" 120 "$log")"
  [ "$code" = "0" ]
}

# ------------------------------------------------------------------- main
assert_never_row_intact

# Answer "you asked for a forbidden probe" BEFORE pre-flight. Refusing must
# not depend on a device being attached, or the refusal is contingent on
# something irrelevant to why it is refused.
if [ -n "$ONLY" ]; then
  while IFS= read -r row; do
    [ "$(field "$row" 2)" = "never" ] || continue
    nm="$(field "$row" 1)"
    if [[ ",$ONLY," == *",$nm,"* ]]; then
      fail "REFUSED: $nm is tier 'never' and cannot be run by this script."
      fail "It has twice wedged the kbase context past kill -9, needing a"
      fail "physical reboot, and the cause is still not understood. If you"
      fail "really mean to run it, do so by hand, having first read the"
      fail "header of src/tests/$nm/$nm.c."
      exit 2
    fi
  done < <(manifest_rows)
fi

if [ "$LIST_ONLY" = "1" ]; then
  printf '%-32s %-8s %s\n' NAME TIER ARGV
  manifest_rows | while IFS= read -r row; do
    printf '%-32s %-8s %s\n' "$(field "$row" 1)" "$(field "$row" 2)" "$(field "$row" 5)"
  done
  exit 0
fi

preflight
mkdir -p "$LOGDIR"

say "regression run $STAMP"
say "  tiers: $TIERS${ONLY:+   only: $ONLY}"
say "  logs:  build/regress/$STAMP/"
case ",$TIERS," in *,render,*)
  say "  ${c_yel}note: render tier selected - these are run with --i-know-it-hangs${c_off}" ;;
esac
say ""

# Read the manifest into an array BEFORE the loop. Iterating it with
# `while read < <(...)` instead means the first `make` or `adb` inside the
# body eats the remaining rows off stdin and the run silently stops after
# one probe - which is exactly what happened the first time this ran.
mapfile -t ROWS < <(manifest_rows)

for row in "${ROWS[@]}"; do
  name="$(field "$row" 1)"; tier="$(field "$row" 2)"
  target="$(field "$row" 3)"; benv="$(field "$row" 4)"
  argv="$(field "$row" 5)"; tmo="$(field "$row" 6)"
  argv="${argv//@SO@/$SO}"

  # tier 'never' is unreachable by any selection. --only is already refused
  # up front (see above); this is the belt to that braces.
  [ "$tier" = "never" ] && continue

  if [ -n "$ONLY" ]; then
    [[ ",$ONLY," == *",$name,"* ]] || continue
  else
    [[ ",$TIERS," == *",$tier,"* ]] || continue
  fi

  log="$LOGDIR/$name.log"
  : >"$log"
  printf '%-32s ' "$name"

  if [ "$SKIP_BUILD" != "1" ]; then
    if ! build_one "$target" "$benv" "$log"; then
      say "${c_red}BUILD-FAIL${c_off}"; R_NAME+=("$name"); R_STATUS+=("BUILD-FAIL")
      FAILED=$((FAILED+1)); continue
    fi
    if ! push_one "$name" "$log"; then
      say "${c_red}PUSH-FAIL${c_off}"; R_NAME+=("$name"); R_STATUS+=("PUSH-FAIL")
      FAILED=$((FAILED+1)); continue
    fi
  fi

  code="$(run_one "$name" "$argv" "$tmo" "$log")"
  case "$code" in
    0)   say "${c_grn}PASS${c_off}";           R_NAME+=("$name"); R_STATUS+=("PASS"); PASS=$((PASS+1)) ;;
    124) say "${c_red}TIMEOUT (${tmo}s)${c_off}"; R_NAME+=("$name"); R_STATUS+=("TIMEOUT"); FAILED=$((FAILED+1)) ;;
    *)   say "${c_red}FAIL (exit $code)${c_off}"; R_NAME+=("$name"); R_STATUS+=("FAIL:$code"); FAILED=$((FAILED+1)) ;;
  esac

  # After anything that entered a render pass, prove the device still works
  # before continuing. Carrying on into a sick device is how the second
  # reboot happened.
  if [ "$tier" = "render" ]; then
    printf '%-32s ' "  ^ health check"
    if health_check "$LOGDIR/health-after-$name.log"; then
      say "${c_grn}ok${c_off}"
    else
      say "${c_red}FAILED${c_off}"
      fail ""
      fail "ABORTING: device health check failed after $name."
      fail "The GPU or the kbase context is in a bad state. Do not keep running."
      fail "See build/regress/$STAMP/health-after-$name.log"
      exit 2
    fi
  fi
done

# ---------------------------------------------------------------- summary
say ""
say "---------------- summary ----------------"
for i in "${!R_NAME[@]}"; do
  st="${R_STATUS[$i]}"
  case "$st" in
    PASS) printf '  %-32s %s\n' "${R_NAME[$i]}" "${c_grn}PASS${c_off}" ;;
    *)    printf '  %-32s %s\n' "${R_NAME[$i]}" "${c_red}$st${c_off}"
          tail -n 3 "$LOGDIR/${R_NAME[$i]}.log" 2>/dev/null | sed 's/^/      | /' ;;
  esac
done
say ""
say "  $PASS passed, $FAILED failed   (logs: build/regress/$STAMP/)"

[ "$FAILED" -eq 0 ] || exit 1
exit 0
