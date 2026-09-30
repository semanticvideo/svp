# Shared helpers for the runtime-bundle build scripts. Sourced, not executed.
#
# Owns: logging, argument defaults, host checks, verified downloads, and the
# sanitized build environment. Component-specific build steps live in the
# build-*.sh scripts.

RUNTIME_BUNDLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$RUNTIME_BUNDLE_DIR/../.." && pwd)"

# shellcheck source=../versions.env
source "$RUNTIME_BUNDLE_DIR/versions.env"

log() { printf '[runtime-bundle] %s\n' "$*" >&2; }
die() { printf '[runtime-bundle] ERROR: %s\n' "$*" >&2; exit 1; }

# Defaults: downloads and build trees stay in an ignored work directory inside
# this folder; outputs go to the ignored dist/runtime folder at the repo root.
WORK_DIR="${WORK_DIR:-$RUNTIME_BUNDLE_DIR/work}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/dist/runtime}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
INCLUDE_SOURCE=0

usage_common() {
  cat <<EOF
Options:
  --work-dir DIR    Downloads and build trees (default: $RUNTIME_BUNDLE_DIR/work)
  --out-dir DIR     Runtime output root (default: $REPO_ROOT/dist/runtime)
  --jobs N          Parallel build jobs (default: host CPU count)
  --with-source     Copy the verified upstream source archives to OUT_DIR/source
  -h, --help        Show this help
EOF
}

parse_common_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --work-dir) WORK_DIR="$2"; shift 2 ;;
      --out-dir) OUT_DIR="$2"; shift 2 ;;
      --jobs) JOBS="$2"; shift 2 ;;
      --with-source) INCLUDE_SOURCE=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown argument: $1" ;;
    esac
  done
  mkdir -p "$WORK_DIR" "$OUT_DIR"
  WORK_DIR="$(cd "$WORK_DIR" && pwd)"
  OUT_DIR="$(cd "$OUT_DIR" && pwd)"
  DOWNLOAD_DIR="$WORK_DIR/downloads"
  mkdir -p "$DOWNLOAD_DIR"
}

require_macos_arm64() {
  [ "$(uname -s)" = "Darwin" ] || die "runtime bundle builds run on macOS only"
  [ "$(uname -m)" = "$SVP_RUNTIME_ARCH" ] || die "runtime bundle builds run on $SVP_RUNTIME_ARCH hosts only"
}

# Resolve a tool from the caller's PATH once, before the sanitized environment
# drops that PATH. Prints the absolute path.
resolve_tool() {
  local path
  path="$(command -v "$1" 2>/dev/null)" || die "required tool not found on PATH: $1"
  printf '%s\n' "$path"
}

# A PATH that contains the system tool directories plus a private directory of
# symlinks to explicitly named extra tools. Package-manager prefixes such as
# /opt/homebrew/bin are never on it, so upstream configure scripts cannot
# discover optional libraries or tools from them.
setup_clean_path() {
  local toolbin="$WORK_DIR/toolbin"
  rm -rf "$toolbin"
  mkdir -p "$toolbin"
  local tool
  for tool in "$@"; do
    ln -s "$(resolve_tool "$tool")" "$toolbin/$tool"
  done
  CLEAN_PATH="$toolbin:/usr/bin:/bin:/usr/sbin:/sbin"
}

# Run a command with an empty environment except for the variables a build
# needs. CFLAGS, LDFLAGS, CPATH, LIBRARY_PATH, PKG_CONFIG_PATH, and similar
# caller settings are dropped so they cannot leak into the build.
run_clean() {
  env -i \
    HOME="$HOME" \
    PATH="$CLEAN_PATH" \
    TMPDIR="${TMPDIR:-/tmp}" \
    LANG=C LC_ALL=C \
    MACOSX_DEPLOYMENT_TARGET="$SVP_RUNTIME_MACOS_DEPLOYMENT_TARGET" \
    ${RUN_CLEAN_EXTRA_ENV[@]+"${RUN_CLEAN_EXTRA_ENV[@]}"} \
    "$@"
}

sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }

# download_verified URL SHA256 FILENAME -> prints the local path.
# Reuses a cached file only when its SHA-256 matches; never trusts a partial
# or mismatched file.
download_verified() {
  local url="$1" expected="$2" name="$3"
  local dest="$DOWNLOAD_DIR/$name"
  if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$expected" ]; then
    log "using cached $name (sha256 ok)"
    printf '%s\n' "$dest"
    return
  fi
  rm -f "$dest" "$dest.part"
  log "downloading $url"
  curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 \
    --output "$dest.part" "$url" || die "download failed: $url"
  local actual
  actual="$(sha256_of "$dest.part")"
  if [ "$actual" != "$expected" ]; then
    rm -f "$dest.part"
    die "SHA-256 mismatch for $name: expected $expected, got $actual"
  fi
  mv "$dest.part" "$dest"
  log "verified $name sha256=$actual"
  printf '%s\n' "$dest"
}

# extract_fresh ARCHIVE DEST_PARENT -> prints the single top-level directory.
extract_fresh() {
  local archive="$1" parent="$2"
  rm -rf "$parent"
  mkdir -p "$parent"
  tar -xf "$archive" -C "$parent"
  local top
  top="$(find "$parent" -mindepth 1 -maxdepth 1 -type d)"
  [ "$(printf '%s\n' "$top" | wc -l | tr -d ' ')" = "1" ] || die "unexpected archive layout: $archive"
  printf '%s\n' "$top"
}

# Fail unless every Mach-O load command dependency of FILE is an OS library,
# or one of the ALLOWED_RPATH_LIBS names (for bundled sibling dylibs).
assert_system_linkage() {
  local file="$1"; shift
  local allowed=" $* "
  local dep bad=0 self_id
  self_id="$(otool -D "$file" | tail -n +2)"
  while IFS= read -r dep; do
    [ -n "$self_id" ] && [ "$dep" = "$self_id" ] && continue
    case "$dep" in
      /usr/lib/*|/System/Library/*) ;;
      @rpath/*|@loader_path/*)
        if [[ "$allowed" != *" ${dep##*/} "* ]]; then
          log "unexpected bundled dependency in $file: $dep"; bad=1
        fi ;;
      *) log "non-system dependency in $file: $dep"; bad=1 ;;
    esac
  done < <(otool -L "$file" | tail -n +2 | awk '{print $1}')
  [ "$bad" = 0 ] || die "$file links libraries outside the OS"
  log "linkage ok: $file"
}

minos_of() {
  otool -l "$1" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}'
}

copy_license() {
  local src="$1" dest_name="$2"
  mkdir -p "$OUT_DIR/licenses"
  cp "$src" "$OUT_DIR/licenses/$dest_name"
}

copy_source_archive() {
  [ "$INCLUDE_SOURCE" = 1 ] || return 0
  mkdir -p "$OUT_DIR/source"
  cp "$1" "$OUT_DIR/source/"
}

# write_component_record NAME JSON -> OUT_DIR/share/svp-runtime/components/NAME.json
# The manifest script merges these records with per-file BLAKE3 digests.
write_component_record() {
  local dir="$OUT_DIR/share/svp-runtime/components"
  mkdir -p "$dir"
  printf '%s\n' "$2" | /usr/bin/jq . > "$dir/$1.json"
}
