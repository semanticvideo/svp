#!/usr/bin/env bash
# Build static, relocatable, LGPL arm64 ffmpeg and ffprobe for the SVP runtime
# bundle, with libdav1d as the only external codec library.
#
# Inputs:  pinned FFmpeg and dav1d source archives (versions.env), verified by
#          SHA-256 before use.
# Outputs: OUT_DIR/bin/ffmpeg, OUT_DIR/bin/ffprobe,
#          OUT_DIR/licenses/{FFmpeg-*,dav1d-COPYING},
#          OUT_DIR/share/svp-runtime/ffmpeg-buildconf.txt,
#          OUT_DIR/share/svp-runtime/components/ffmpeg.json
#
# Host tools: Apple clang (Command Line Tools), make, meson, ninja, pkg-config.
# Nothing from a package-manager prefix is linked; the script fails unless
# both binaries depend only on /usr/lib and /System/Library.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  echo "Usage: $0 [options]"
  echo "Builds FFmpeg $FFMPEG_VERSION (static, LGPL) with dav1d $DAV1D_VERSION."
  usage_common
}

parse_common_args "$@"
require_macos_arm64

PKG_CONFIG_BIN="$(command -v pkg-config || command -v pkgconf || true)"
[ -n "$PKG_CONFIG_BIN" ] || die "pkg-config (or pkgconf) is required"
setup_clean_path meson ninja
ln -s "$PKG_CONFIG_BIN" "$WORK_DIR/toolbin/pkg-config"

TARGET="$SVP_RUNTIME_MACOS_DEPLOYMENT_TARGET"
MIN_FLAG="-mmacosx-version-min=$TARGET"
FFMPEG_WORK="$WORK_DIR/ffmpeg"
DEPS_PREFIX="$FFMPEG_WORK/deps-prefix"
# FFmpeg bakes its prefix into the programs (FFMPEG_DATADIR, used only by the
# -pre/-fpre preset options SVP never passes). Configure with FFmpeg's default
# prefix and stage the install with DESTDIR, so no work-tree path is embedded
# and the binaries are the same wherever the work tree lives.
FFMPEG_INSTALL_PREFIX=/usr/local
FFMPEG_STAGE="$FFMPEG_WORK/ffmpeg-stage"
SVP_RUNTIME_SHARE="$OUT_DIR/share/svp-runtime"
mkdir -p "$FFMPEG_WORK"
rm -rf "$DEPS_PREFIX" "$FFMPEG_STAGE"

# pkg-config sees only the private dependency prefix. PKG_CONFIG_LIBDIR
# replaces the default search path, so .pc files from /opt/homebrew or
# /usr/local cannot be picked up.
RUN_CLEAN_EXTRA_ENV=(PKG_CONFIG_LIBDIR="$DEPS_PREFIX/lib/pkgconfig")

# --- sources -------------------------------------------------------------------
ffmpeg_archive="$(download_verified "$FFMPEG_URL" "$FFMPEG_SHA256" "ffmpeg-$FFMPEG_VERSION.tar.xz")"
dav1d_archive="$(download_verified "$DAV1D_URL" "$DAV1D_SHA256" "dav1d-$DAV1D_VERSION.tar.bz2")"
ffmpeg_src="$(extract_fresh "$ffmpeg_archive" "$FFMPEG_WORK/src-ffmpeg")"
dav1d_src="$(extract_fresh "$dav1d_archive" "$FFMPEG_WORK/src-dav1d")"

# --- dav1d (static) --------------------------------------------------------------
# release buildtype and default bitdepths (8 and 16) match a stock dav1d
# build; tools, examples, tests, and docs are not needed to decode.
log "building dav1d $DAV1D_VERSION"
dav1d_build="$FFMPEG_WORK/build-dav1d"
rm -rf "$dav1d_build"
run_clean meson setup "$dav1d_build" "$dav1d_src" \
  --prefix="$DEPS_PREFIX" \
  --libdir=lib \
  --buildtype=release \
  --default-library=static \
  --wrap-mode=nofallback \
  -Dc_args="$MIN_FLAG" \
  -Dc_link_args="$MIN_FLAG" \
  -Denable_tools=false \
  -Denable_examples=false \
  -Denable_tests=false \
  -Denable_docs=false
run_clean ninja -C "$dav1d_build" -j "$JOBS"
run_clean ninja -C "$dav1d_build" install

# --- FFmpeg ----------------------------------------------------------------------
# Every flag and its reason. Anything not listed keeps FFmpeg's default, which
# enables all internal (FFmpeg-native) decoders, encoders, parsers, muxers,
# demuxers, filters, and bitstream filters, the same internal set Homebrew
# builds. That keeps decoder selection identical: FFmpeg picks the first
# registered decoder for a codec, native decoders come first, and libdav1d is
# registered ahead of the native AV1 decoder.
FFMPEG_CONFIGURE_FLAGS=(
  # FFmpeg's default prefix; the install is staged with DESTDIR (see above)
  # and only bin/ffmpeg and bin/ffprobe are copied out.
  "--prefix=$FFMPEG_INSTALL_PREFIX"
  # Apple clang from the Command Line Tools, the compiler Homebrew uses.
  "--cc=clang"
  # Optimize for size, replacing FFmpeg's default -O3. Homebrew's bottle
  # build replaces -O3 with -Os (superenv), and SVP's current reference
  # outputs come from that bottle. Integer decoders are bit-exact at any
  # level, but float decoders are not: with -O3 the native Opus decoder
  # differs from the bottle by up to 4.5e-8 in about 17% of samples, which
  # changes extracted FLAC bytes. With -Os the output is byte-identical.
  "--optflags=-Os"
  # License: LGPL v2.1 or later only. No GPL components (x264, x265,
  # GPL filters), no version-3 upgrade, no non-redistributable components.
  "--disable-gpl"
  "--disable-version3"
  "--disable-nonfree"
  # Static libraries linked into the two programs: no FFmpeg dylibs to ship,
  # no rpaths, and the binaries are relocatable.
  "--disable-shared"
  "--enable-static"
  # Do not probe the host for optional libraries. Without this, configure
  # would link whatever it finds (SDL2, xz, libxcb, OpenSSL, ...) from
  # package-manager prefixes. Each needed system library is enabled below.
  "--disable-autodetect"
  # POSIX threads for frame/slice threading (libSystem). Homebrew enables it.
  "--enable-pthreads"
  # System zlib (/usr/lib/libz): PNG decode/encode, compressed MOV headers.
  "--enable-zlib"
  # System bzip2 (/usr/lib/libbz2): bzip2-compressed Matroska tracks.
  "--enable-bzlib"
  # System iconv (/usr/lib/libiconv): subtitle and MPEG-TS text character-set
  # conversion. configure's iconv probe passes without -liconv on macOS, but
  # the static program link then fails, so name the library explicitly.
  "--enable-iconv"
  "--extra-libs=-liconv"
  # AV1 decoding through dav1d (BSD-2-Clause), the decoder Homebrew's ffmpeg
  # selects for AV1 today.
  "--enable-libdav1d"
  # Link dav1d statically via its .pc Libs.private.
  "--pkg-config-flags=--static"
  # SVP invokes only the ffmpeg and ffprobe programs. ffplay needs SDL2.
  "--disable-ffplay"
  # No man pages or HTML docs in the runtime.
  "--disable-doc"
  # SVP reads local files only; no network protocols, no TLS dependency.
  "--disable-network"
  # Pin the minimum macOS for every object and the final link.
  "--extra-cflags=$MIN_FLAG"
  "--extra-ldflags=$MIN_FLAG"
)
# Not enabled, on purpose: videotoolbox and audiotoolbox (hardware decode and
# AudioToolbox codecs; SVP never requests a hwaccel and native decoders are
# selected first), lzma (TIFF LZMA only; macOS has no system liblzma headers),
# securetransport (network is disabled), avfoundation/appkit/coreimage/metal
# (capture devices and GPU filters SVP does not use).

log "configuring FFmpeg $FFMPEG_VERSION"
# Build in the freshly extracted source tree: an in-tree build compiles with
# relative source paths, so assertion messages (__FILE__) do not embed the
# work-tree location.
ffmpeg_build="$ffmpeg_src"
(cd "$ffmpeg_build" && run_clean ./configure "${FFMPEG_CONFIGURE_FLAGS[@]}") \
  > "$FFMPEG_WORK/configure.log" 2>&1 || { tail -40 "$ffmpeg_build/ffbuild/config.log" >&2 || true; die "FFmpeg configure failed; see $FFMPEG_WORK/configure.log"; }
grep -q '^License: LGPL version 2.1 or later' "$FFMPEG_WORK/configure.log" \
  || die "FFmpeg configure did not report LGPL v2.1+; see $FFMPEG_WORK/configure.log"

log "building FFmpeg"
(cd "$ffmpeg_build" && run_clean make -j "$JOBS" && run_clean make install DESTDIR="$FFMPEG_STAGE") > "$FFMPEG_WORK/build.log" 2>&1 \
  || { tail -40 "$FFMPEG_WORK/build.log" >&2; die "FFmpeg build failed"; }

# --- install into the runtime layout ------------------------------------------------
mkdir -p "$OUT_DIR/bin" "$SVP_RUNTIME_SHARE"
for prog in ffmpeg ffprobe; do
  install -m 0755 "$FFMPEG_STAGE$FFMPEG_INSTALL_PREFIX/bin/$prog" "$OUT_DIR/bin/$prog"
  assert_system_linkage "$OUT_DIR/bin/$prog"
  [ -z "$(otool -l "$OUT_DIR/bin/$prog" | grep LC_RPATH || true)" ] || die "$prog has an rpath"
  minos="$(minos_of "$OUT_DIR/bin/$prog")"
  [ "$minos" = "$TARGET" ] || die "$prog minos is $minos, expected $TARGET"
done

"$OUT_DIR/bin/ffmpeg" -hide_banner -buildconf > "$SVP_RUNTIME_SHARE/ffmpeg-buildconf.txt"
"$OUT_DIR/bin/ffmpeg" -hide_banner -L | grep -q 'GNU Lesser General Public' \
  || die "built ffmpeg does not report the LGPL"

# --- feature check against SVP's invocations ----------------------------------------
component_list() {
  local tool="$1" kind="$2"
  case "$kind" in
    decoder|encoder)
      "$tool" -hide_banner "-${kind}s" | awk 'p{print $2} /^ *-+$/{p=1}' ;;
    filter)
      "$tool" -hide_banner -filters | awk 'p{print $2} /^ *-+$/{p=1}' ;;
    demuxer|muxer)
      "$tool" -hide_banner "-${kind}s" | awk 'p{print $2} /^ *-+$/{p=1}' | tr ',' '\n' ;;
    indev)
      "$tool" -hide_banner -devices | awk 'p&&$1~/D/{print $2} /^ *-+$/{p=1}' | tr ',' '\n' ;;
    *) die "unknown feature kind: $kind" ;;
  esac
}
missing=0
while read -r kind name; do
  case "$kind" in ''|'#'*) continue ;; esac
  if ! component_list "$OUT_DIR/bin/ffmpeg" "$kind" | grep -qx "$name"; then
    log "missing $kind: $name"; missing=1
  fi
done < "$RUNTIME_BUNDLE_DIR/ffmpeg-svp-features.txt"
[ "$missing" = 0 ] || die "built ffmpeg lacks components SVP invokes"
log "all SVP ffmpeg components present"

# --- licenses, sources, component record -------------------------------------------
copy_license "$ffmpeg_src/COPYING.LGPLv2.1" "FFmpeg-COPYING.LGPLv2.1"
copy_license "$ffmpeg_src/LICENSE.md" "FFmpeg-LICENSE.md"
copy_license "$dav1d_src/COPYING" "dav1d-COPYING"
copy_source_archive "$ffmpeg_archive"
copy_source_archive "$dav1d_archive"

flags_json="$(printf '%s\n' "${FFMPEG_CONFIGURE_FLAGS[@]}" | /usr/bin/jq -R . | /usr/bin/jq -s .)"
write_component_record ffmpeg "$(cat <<EOF
{
  "component": "ffmpeg",
  "version": "$FFMPEG_VERSION",
  "license": "LGPL-2.1-or-later",
  "files": ["bin/ffmpeg", "bin/ffprobe"],
  "source": {"url": "$FFMPEG_URL", "sha256": "$FFMPEG_SHA256"},
  "configure_flags": $flags_json,
  "linked_libraries": [
    {
      "name": "dav1d",
      "version": "$DAV1D_VERSION",
      "license": "BSD-2-Clause",
      "linkage": "static",
      "source": {"url": "$DAV1D_URL", "sha256": "$DAV1D_SHA256"}
    }
  ],
  "macos_deployment_target": "$TARGET",
  "arch": "$SVP_RUNTIME_ARCH",
  "compiler": "$(clang --version | head -1)"
}
EOF
)"
log "ffmpeg and ffprobe written to $OUT_DIR/bin"
