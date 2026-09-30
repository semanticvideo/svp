#!/usr/bin/env bash
# Write OUT_DIR/components.json: every file in the runtime output with its
# BLAKE3 digest and size, grouped by the component that produced it, plus the
# component source versions recorded by the build scripts.
#
# Run after build-ffmpeg.sh and/or build-sherpa-onnx.sh. BLAKE3 comes from
# `svp-models-tool hash` (pass --hash-tool) or from `b3sum` on PATH.
# Digests use SVP's "blake3:<hex>" form.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

HASH_TOOL=""
usage() {
  echo "Usage: $0 [--out-dir DIR] [--hash-tool /path/to/svp-models-tool]"
}
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --hash-tool) HASH_TOOL="$2"; shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done
parse_common_args ${args[@]+"${args[@]}"}

blake3_of() {
  if [ -n "$HASH_TOOL" ]; then
    "$HASH_TOOL" hash --file "$1"
  elif command -v b3sum >/dev/null 2>&1; then
    printf 'blake3:%s\n' "$(b3sum --no-names "$1")"
  else
    die "no BLAKE3 tool: pass --hash-tool <svp-models-tool> or install b3sum"
  fi
}

records_dir="$OUT_DIR/share/svp-runtime/components"
[ -d "$records_dir" ] || die "no component records in $records_dir; run a build script first"

# file_entry RELPATH -> {"path","blake3","size_bytes"}
file_entry() {
  local rel="$1" digest size
  digest="$(blake3_of "$OUT_DIR/$rel")"
  size="$(stat -f %z "$OUT_DIR/$rel")"
  /usr/bin/jq -n --arg p "$rel" --arg d "$digest" --argjson s "$size" \
    '{path: $p, blake3: $d, size_bytes: $s}'
}

components="[]"
claimed=" components.json "
for record in "$records_dir"/*.json; do
  entries="[]"
  while IFS= read -r rel; do
    [ -f "$OUT_DIR/$rel" ] || die "$(basename "$record") lists missing file $rel"
    entries="$(/usr/bin/jq --argjson e "$(file_entry "$rel")" '. + [$e]' <<<"$entries")"
    claimed="$claimed$rel "
  done < <(/usr/bin/jq -r '.files[]' "$record")
  components="$(/usr/bin/jq --slurpfile r "$record" --argjson f "$entries" \
    '. + [($r[0] + {files: $f})]' <<<"$components")"
done

# Licenses, build records, and optional source archives are part of the
# bundle too; hash them so the manifest covers every shipped byte.
support="[]"
while IFS= read -r rel; do
  case "$claimed" in *" $rel "*) continue ;; esac
  support="$(/usr/bin/jq --argjson e "$(file_entry "$rel")" '. + [$e]' <<<"$support")"
done < <(cd "$OUT_DIR" && find . -type f ! -name '.DS_Store' | sed 's|^\./||' | LC_ALL=C sort)

/usr/bin/jq -n \
  --arg arch "$SVP_RUNTIME_ARCH" \
  --arg target "$SVP_RUNTIME_MACOS_DEPLOYMENT_TARGET" \
  --argjson components "$components" \
  --argjson support "$support" \
  '{
     schema: "svp.runtime.components/1",
     arch: $arch,
     macos_deployment_target: $target,
     components: ($components | sort_by(.component)),
     support_files: $support
   }' > "$OUT_DIR/components.json"
log "wrote $OUT_DIR/components.json"
