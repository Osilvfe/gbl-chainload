#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ -e "$REPO_ROOT/docker/Dockerfile" || -e "$REPO_ROOT/scripts/build-inside-docker.sh" ]]; then
  echo 'FAIL: obsolete container build files remain' >&2
  exit 1
fi
if grep -RnE 'docker[[:space:]]+(run|build|load|image)|uses:[[:space:]]+docker/|docker/Dockerfile|build-inside-docker.sh' \
    "$REPO_ROOT/scripts" "$REPO_ROOT/.github/workflows" "$REPO_ROOT/.cargo" "$REPO_ROOT/GblChainloadPkg"; then
  echo 'FAIL: active build surfaces still reference the removed container workflow' >&2
  exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/scripts" "$TMP/bin" "$TMP/Build/unixnu"
cp "$REPO_ROOT"/scripts/build{,-cross-tools,-recovery-tools}.sh "$TMP/scripts/"
cd "$TMP"
printf 'keep\n' > Build/unixnu/sentinel
export MOCK_LOG="$TMP/commands.log"
export MOCK_TARGETS=$'aarch64-linux-android\nx86_64-pc-windows-gnu\nx86_64-apple-darwin\naarch64-apple-darwin\nx86_64-unknown-linux-musl'
export CARGO_TARGET_DIR="$TMP/cargo-output"
export PATH="$TMP/bin:$PATH"
unset ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK NDK_BIN
unset CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER
unset CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER
unset CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER

cat > scripts/build-native.sh <<'SH'
#!/usr/bin/env bash
set -euo pipefail
echo "native $*" >> "$MOCK_LOG"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output) mkdir -p "$(dirname "$2")"; printf '%s\n' firmware > "$2"; shift 2 ;;
    *) shift ;;
  esac
done
SH
cat > bin/cargo <<'SH'
#!/usr/bin/env bash
set -euo pipefail
echo "cargo $*" >> "$MOCK_LOG"
[[ " $* " == *" --release "* && " $* " == *" --locked "* ]]
target=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) target="$2"; shift 2 ;;
    *) shift ;;
  esac
done
name=gbl
[[ "$target" != x86_64-pc-windows-gnu ]] || name=gbl.exe
if [[ "$target" == aarch64-linux-android ]]; then
  "$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER" --fixture
fi
if [[ "$target" == *apple-darwin && "${MOCK_HOST:-Linux}" == Linux ]]; then
  case "$target" in
    x86_64-*) "$CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER" --fixture ;;
    aarch64-*) "$CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER" --fixture ;;
  esac
fi
out="$CARGO_TARGET_DIR/${target:+$target/}release"
mkdir -p "$out"
printf '%s\n' "binary $target" > "$out/$name"
SH
cat > bin/rustup <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$MOCK_TARGETS"
SH
cat > bin/uname <<'SH'
#!/usr/bin/env bash
echo "${MOCK_HOST:-Linux}"
SH
cat > bin/llvm-lipo <<'SH'
#!/usr/bin/env bash
set -euo pipefail
echo "lipo $*" >> "$MOCK_LOG"
[[ "$1" == -create && "$2" == -output && $# -eq 5 ]]
cat "$4" "$5" > "$3"
SH
cat > bin/zig <<'SH'
#!/usr/bin/env bash
echo "zig $*" >> "$MOCK_LOG"
SH
cat > bin/docker <<'SH'
#!/usr/bin/env bash
echo "FAIL: container invocation" >&2
exit 99
SH
for tool in musl-gcc x86_64-w64-mingw32-gcc x86_64-w64-mingw32-dlltool clang; do
  cp bin/zig "bin/$tool"
done
chmod +x bin/*

expect_failure() {
  if "$@" > failure.log 2>&1; then
    echo "FAIL: command unexpectedly succeeded: $*" >&2
    exit 1
  fi
}

expect_failure env CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=missing-ndk-linker bash scripts/build.sh
[[ ! -f "$MOCK_LOG" ]]
grep -q 'Android NDK linker not found' failure.log
bash scripts/build.sh --auto --debug --verbose --no-recovery --no-host
grep -qx 'native --release --auto --debug --verbose --output dist/gbl-chainload-auto-debug-verbose.efi' "$MOCK_LOG"
[[ -f dist/gbl-chainload-auto-debug-verbose.efi && ! -d dist/host && ! -d dist/recovery ]]
[[ "$(cat Build/unixnu/sentinel)" == keep ]]

bash scripts/build.sh --experimental-usb-msc-partition-export --no-recovery --no-host
grep -qx 'native --release --experimental-usb-msc-partition-export --output dist/gbl-chainload-msc-partitions.efi' "$MOCK_LOG"
[[ -f dist/gbl-chainload-msc-partitions.efi && ! -d dist/host && ! -d dist/recovery ]]
bash scripts/build.sh --auto --debug --verbose --experimental-usb-msc-partition-export --no-recovery --no-host
grep -qx 'native --release --auto --debug --verbose --experimental-usb-msc-partition-export --output dist/gbl-chainload-auto-debug-verbose-msc-partitions.efi' "$MOCK_LOG"
[[ -f dist/gbl-chainload-auto-debug-verbose-msc-partitions.efi ]]
[[ "$(cat Build/unixnu/sentinel)" == keep ]]

NDK_FIXTURE="$TMP/Android NDK"
mkdir -p "$NDK_FIXTURE/toolchains/llvm/prebuilt/linux-x86_64/bin"
cp bin/zig "$NDK_FIXTURE/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang"
for variable in ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK; do
  env "$variable=$NDK_FIXTURE" bash scripts/build-recovery-tools.sh --check-only
done
env NDK_BIN="$NDK_FIXTURE/toolchains/llvm/prebuilt/linux-x86_64/bin" bash scripts/build-recovery-tools.sh
(cd dist/recovery && sha256sum -c SHA256SUMS)
export ANDROID_NDK_HOME="$NDK_FIXTURE"
bash scripts/build.sh
[[ -f dist/gbl-chainload.efi && -f dist/recovery/gbl && -f dist/host/gbl ]]
grep -qx 'native --release --output dist/gbl-chainload.efi' "$MOCK_LOG"
[[ "$(cat Build/unixnu/sentinel)" == keep ]]
env CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$NDK_FIXTURE/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang" bash scripts/build-recovery-tools.sh --check-only
expect_failure env MOCK_TARGETS=none bash scripts/build-recovery-tools.sh --check-only
grep -q 'rustup target add aarch64-linux-android' failure.log

bash scripts/build-cross-tools.sh all
for platform in windows macos linux; do
  (cd "dist/$platform" && sha256sum -c SHA256SUMS)
done
grep -qx 'zig cc -target x86_64-macos --fixture' "$MOCK_LOG"
grep -qx 'zig cc -target aarch64-macos --fixture' "$MOCK_LOG"
env MOCK_HOST=Darwin bash scripts/build-cross-tools.sh macos
expect_failure env MOCK_TARGETS=none bash scripts/build-cross-tools.sh linux
grep -q 'rustup target add x86_64-unknown-linux-musl' failure.log
expect_failure bash scripts/build.sh --mode 2
expect_failure bash scripts/build-cross-tools.sh unknown
expect_failure bash scripts/build-recovery-tools.sh --unknown
for script in build build-native build-recovery-tools; do
  bash "$REPO_ROOT/scripts/$script.sh" --help >/dev/null
done
echo 'ok 100_native_build — native orchestration, NDK discovery, cross targets, checksums and cache preservation'
