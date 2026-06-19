# Command Checklist

## Baseline

```bash
cd /Users/domesposito/Projects/svp
scripts/verify-spec-files.sh
git status
```

## Configure and build

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

## Validator smoke

```bash
./build/tools/svp-validator/svp-validator --help
./build/tools/svp-validator/svp-validator --version
./build/tools/svp-validator/svp-validator validate missing.svp --json
```

## Fixture validation

```bash
./build/tools/svp-validator/svp-validator validate fixtures/static-card/example.svp --json
./build/tools/svp-validator/svp-validator validate fixtures/invalid/missing-depth.svp --json
```

## First video trial

```bash
mkdir -p runs/first-video-trial
./build/tools/svp-builder/svp-builder build samples/dom-30s.mov --out runs/first-video-trial/dom-30s.svp 2>&1 | tee runs/first-video-trial/build.log
./build/tools/svp-validator/svp-validator validate runs/first-video-trial/dom-30s.svp --json > runs/first-video-trial/validation.json
./build/tools/svp-inspector/svp-inspector inspect runs/first-video-trial/dom-30s.svp > runs/first-video-trial/inspect.txt
```
