#!/usr/bin/env bash
# Build the sherpa-onnx C API dynamic library for the SVP runtime bundle,
# together with the ONNX Runtime dylib it links.
#
# Inputs:  pinned sherpa-onnx source and ONNX Runtime release archives
#          (versions.env), verified by SHA-256 before use. sherpa-onnx's own
#          CMake fetches its small build dependencies (kaldi-native-fbank,
#          kaldi-decoder, simple-sentencepiece, nlohmann-json, eigen,
#          hclust-cpp, openfst, ...) and verifies each with the SHA-256 pinned
#          in its cmake/*.cmake files.
# Outputs: OUT_DIR/lib/libsherpa-onnx-c-api.dylib and the @rpath dylibs it
#          needs (including libonnxruntime.1.dylib),
#          OUT_DIR/licenses/{sherpa-onnx-LICENSE,onnxruntime-*},
#          OUT_DIR/share/svp-runtime/components/sherpa-onnx.json
#
# The script fails unless the shipped dylibs depend only on OS libraries and
# each other, and unless the compatibility check against SVP's dlopen/dlsym
# contract passes (lib/sherpa-compat-check.cpp).
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  echo "Usage: $0 [options]"
  echo "Builds sherpa-onnx $SHERPA_ONNX_VERSION C API with ONNX Runtime $ONNXRUNTIME_VERSION."
  usage_common
}

parse_common_args "$@"
require_macos_arm64
setup_clean_path cmake

TARGET="$SVP_RUNTIME_MACOS_DEPLOYMENT_TARGET"
SHERPA_WORK="$WORK_DIR/sherpa-onnx"
SHERPA_PREFIX="$SHERPA_WORK/prefix"
SVP_RUNTIME_SHARE="$OUT_DIR/share/svp-runtime"
mkdir -p "$SHERPA_WORK"
rm -rf "$SHERPA_PREFIX"

# --- sources -------------------------------------------------------------------
sherpa_archive="$(download_verified "$SHERPA_ONNX_URL" "$SHERPA_ONNX_SHA256" "sherpa-onnx-$SHERPA_ONNX_VERSION.tar.gz")"
ort_archive="$(download_verified "$ONNXRUNTIME_URL" "$ONNXRUNTIME_SHA256" "onnxruntime-osx-arm64-$ONNXRUNTIME_VERSION.tgz")"
sherpa_src="$(extract_fresh "$sherpa_archive" "$SHERPA_WORK/src-sherpa-onnx")"
ort_root="$(extract_fresh "$ort_archive" "$SHERPA_WORK/onnxruntime")"

grep -q "set(SHERPA_ONNX_VERSION \"$SHERPA_ONNX_VERSION\")" "$sherpa_src/CMakeLists.txt" \
  || die "sherpa-onnx source does not declare version $SHERPA_ONNX_VERSION"
grep -q "v$ONNXRUNTIME_VERSION/onnxruntime-osx-arm64-$ONNXRUNTIME_VERSION" "$sherpa_src/cmake/onnxruntime-osx-arm64.cmake" \
  || die "sherpa-onnx $SHERPA_ONNX_VERSION does not pin ONNX Runtime $ONNXRUNTIME_VERSION for osx-arm64"
[ "$(cat "$ort_root/VERSION_NUMBER")" = "$ONNXRUNTIME_VERSION" ] || die "unexpected ONNX Runtime package version"

# Point sherpa-onnx at the verified official ONNX Runtime package instead of
# letting it download its own copy.
RUN_CLEAN_EXTRA_ENV=(
  SHERPA_ONNXRUNTIME_LIB_DIR="$ort_root/lib"
  SHERPA_ONNXRUNTIME_INCLUDE_DIR="$ort_root/include"
)

# --- configure -----------------------------------------------------------------
SHERPA_CMAKE_FLAGS=(
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_PREFIX=$SHERPA_PREFIX"
  "-DCMAKE_OSX_ARCHITECTURES=$SVP_RUNTIME_ARCH"
  # sherpa-onnx defaults this cache variable to 10.14; pin the bundle target.
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$TARGET"
  # CMake searches its own install prefix (for example /opt/homebrew) by
  # default; keep package-manager prefixes out of every find_* call.
  "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local"
  # SVP dlopens a C API dylib.
  "-DBUILD_SHARED_LIBS=ON"
  "-DSHERPA_ONNX_ENABLE_C_API=ON"
  "-DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=ON"
  # Use the ONNX Runtime package selected through SHERPA_ONNXRUNTIME_*_DIR.
  "-DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=ON"
  # Not used by SVP: TTS (pulls espeak-ng and piper), CLI binaries and C API
  # examples, Python, JNI, tests, microphone input, websocket server, GPU.
  "-DSHERPA_ONNX_ENABLE_TTS=OFF"
  "-DSHERPA_ONNX_ENABLE_BINARY=OFF"
  "-DSHERPA_ONNX_BUILD_C_API_EXAMPLES=OFF"
  "-DSHERPA_ONNX_ENABLE_PYTHON=OFF"
  "-DSHERPA_ONNX_ENABLE_JNI=OFF"
  "-DSHERPA_ONNX_ENABLE_TESTS=OFF"
  "-DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF"
  "-DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF"
  "-DSHERPA_ONNX_ENABLE_GPU=OFF"
)

sherpa_build="$SHERPA_WORK/build"
rm -rf "$sherpa_build"
log "configuring sherpa-onnx $SHERPA_ONNX_VERSION"
run_clean cmake -S "$sherpa_src" -B "$sherpa_build" "${SHERPA_CMAKE_FLAGS[@]}" \
  > "$SHERPA_WORK/configure.log" 2>&1 || { tail -40 "$SHERPA_WORK/configure.log" >&2; die "sherpa-onnx configure failed"; }
grep -q "location_onnxruntime_lib: $ort_root/lib/libonnxruntime.dylib" "$SHERPA_WORK/configure.log" \
  || die "sherpa-onnx did not select the pinned ONNX Runtime; see $SHERPA_WORK/configure.log"

log "building sherpa-onnx"
run_clean cmake --build "$sherpa_build" -j "$JOBS" \
  > "$SHERPA_WORK/build.log" 2>&1 || { tail -40 "$SHERPA_WORK/build.log" >&2; die "sherpa-onnx build failed"; }
run_clean cmake --install "$sherpa_build" >> "$SHERPA_WORK/build.log" 2>&1 \
  || { tail -40 "$SHERPA_WORK/build.log" >&2; die "sherpa-onnx install failed"; }

# --- collect the C API dylib and its @rpath closure --------------------------------
# Search order for @rpath dependencies: sherpa-onnx install prefix, then the
# ONNX Runtime package. Each file is copied under its install name so that
# @loader_path resolves it next to libsherpa-onnx-c-api.dylib.
mkdir -p "$OUT_DIR/lib"
shipped=()
find_rpath_lib() {
  local name="$1" dir
  for dir in "$SHERPA_PREFIX/lib" "$ort_root/lib"; do
    if [ -e "$dir/$name" ]; then
      # Resolve symlinks so the bundle holds one regular file per name.
      (cd "$dir" && printf '%s/%s\n' "$(pwd -P)" "$(basename "$(readlink -f "$name")")")
      return
    fi
  done
  die "cannot find @rpath dependency $name"
}
queue=("$SHERPA_PREFIX/lib/libsherpa-onnx-c-api.dylib")
while [ ${#queue[@]} -gt 0 ]; do
  src="${queue[0]}"; queue=("${queue[@]:1}")
  name="$(basename "$(otool -D "$src" | tail -n +2)")"
  [ -n "$name" ] || name="$(basename "$src")"
  case " ${shipped[*]-} " in *" $name "*) continue ;; esac
  install -m 0644 "$src" "$OUT_DIR/lib/$name"
  shipped+=("$name")
  while IFS= read -r dep; do
    case "$dep" in
      @rpath/*) queue+=("$(find_rpath_lib "${dep#@rpath/}")") ;;
    esac
  done < <(otool -L "$src" | tail -n +2 | awk '{print $1}')
done
log "shipped dylibs: ${shipped[*]}"

# Only @loader_path may remain as an rpath. CMake can add absolute build-tree
# rpaths (CMAKE_INSTALL_RPATH_USE_LINK_PATH); remove them and re-sign ad hoc,
# since editing load commands invalidates the arm64 code signature.
for name in "${shipped[@]}"; do
  f="$OUT_DIR/lib/$name"
  changed=0
  while IFS= read -r rp; do
    [ "$rp" = "@loader_path" ] && continue
    install_name_tool -delete_rpath "$rp" "$f"
    changed=1
  done < <(otool -l "$f" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}')
  if [ "$changed" = 1 ]; then
    codesign --force --sign - "$f"
    log "removed build rpaths and re-signed $name"
  fi
  assert_system_linkage "$f" "${shipped[@]}"
done
c_api="$OUT_DIR/lib/libsherpa-onnx-c-api.dylib"
otool -l "$c_api" | grep -q "path @loader_path " || die "libsherpa-onnx-c-api.dylib lacks an @loader_path rpath"
minos="$(minos_of "$c_api")"
[ "$minos" = "$TARGET" ] || die "libsherpa-onnx-c-api.dylib minos is $minos, expected $TARGET"

# --- compatibility with SVP's dlopen contract ---------------------------------------
# SVP's mirror header needs nlohmann/json_fwd.hpp: take it from sherpa-onnx's
# fetched dependencies, or from an existing SVP build's vcpkg tree.
json_include="$(find "$sherpa_build/_deps" "$REPO_ROOT/build" -path '*/include/nlohmann/json_fwd.hpp' -print -quit 2>/dev/null | sed 's|/nlohmann/json_fwd.hpp$||' || true)"
[ -n "$json_include" ] || die "nlohmann/json_fwd.hpp not found (needed to compile SVP's sherpa mirror header)"
checker="$SHERPA_WORK/sherpa-compat-check"
run_clean clang++ -std=c++17 -Wall -Werror \
  "-mmacosx-version-min=$TARGET" \
  -I "$sherpa_src" \
  -I "$REPO_ROOT/packages/svp-audio/src/sherpa_diarization" \
  -I "$REPO_ROOT/packages/svp-audio/include" \
  -I "$REPO_ROOT/packages/svp-models/include" \
  -I "$json_include" \
  "$RUNTIME_BUNDLE_DIR/lib/sherpa-compat-check.cpp" -o "$checker"
svp_symbols=()
while IFS= read -r sym; do svp_symbols+=("$sym"); done < <(
  sed -n 's/.*load(api\.[a-z_]*, "\([A-Za-z]*\)");.*/\1/p' \
    "$REPO_ROOT/packages/svp-audio/src/sherpa_diarization/api.cpp")
[ ${#svp_symbols[@]} -gt 0 ] || die "no dlsym names found in api.cpp"
"$checker" "$c_api" "${svp_symbols[@]}" || die "sherpa-onnx library does not satisfy SVP's C API contract"

# --- licenses, sources, component record -------------------------------------------
copy_license "$sherpa_src/LICENSE" "sherpa-onnx-LICENSE"
copy_license "$ort_root/LICENSE" "onnxruntime-LICENSE"
copy_license "$ort_root/ThirdPartyNotices.txt" "onnxruntime-ThirdPartyNotices.txt"
copy_source_archive "$sherpa_archive"

files_json="$(printf 'lib/%s\n' "${shipped[@]}" | /usr/bin/jq -R . | /usr/bin/jq -s .)"
flags_json="$(printf '%s\n' "${SHERPA_CMAKE_FLAGS[@]}" \
  | sed "s|$SHERPA_PREFIX|<prefix>|" | /usr/bin/jq -R . | /usr/bin/jq -s .)"
# Build dependencies sherpa-onnx fetched through its own SHA-256-pinned
# cmake/<name>.cmake files.
fetched_json="$(find "$sherpa_build/_deps" -mindepth 1 -maxdepth 1 -type d -name '*-src' \
  | sed 's|.*/||; s|-src$||' | sort | /usr/bin/jq -R . | /usr/bin/jq -s .)"
write_component_record sherpa-onnx "$(cat <<EOF
{
  "component": "sherpa-onnx",
  "version": "$SHERPA_ONNX_VERSION",
  "license": "Apache-2.0",
  "files": $files_json,
  "source": {"url": "$SHERPA_ONNX_URL", "sha256": "$SHERPA_ONNX_SHA256"},
  "cmake_flags": $flags_json,
  "fetched_build_dependencies": $fetched_json,
  "linked_libraries": [
    {
      "name": "onnxruntime",
      "version": "$ONNXRUNTIME_VERSION",
      "license": "MIT",
      "linkage": "dynamic (prebuilt official release, shipped as lib/libonnxruntime.1.dylib)",
      "source": {"url": "$ONNXRUNTIME_URL", "sha256": "$ONNXRUNTIME_SHA256"}
    }
  ],
  "svp_abi_check": "lib/sherpa-compat-check.cpp passed (${#svp_symbols[@]} symbols)",
  "macos_deployment_target": "$TARGET",
  "arch": "$SVP_RUNTIME_ARCH",
  "compiler": "$(clang --version | head -1)"
}
EOF
)"
log "sherpa-onnx C API written to $OUT_DIR/lib"
