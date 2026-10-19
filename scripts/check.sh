#!/usr/bin/env bash
# Full local verification: vectors are up to date, C builds (-Werror, ASan/UBSan) and passes,
# TypeScript typechecks and passes (including integration against the C host agent).
# Pass --3ds to also build the console agent (needs devkitPro).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> vectors up to date?"
python3 docs/protocol/test-vectors/generate.py
if [ -d .git ] && ! git diff --quiet -- docs/protocol/test-vectors agent/common/tests/vectors_gen.h; then
  echo "vectors changed after regeneration — commit the regenerated files" >&2
  git --no-pager diff --stat -- docs/protocol/test-vectors agent/common/tests/vectors_gen.h >&2
  exit 1
fi

echo "==> web page scripts parse?"
for f in web/js/*.js; do node --check "$f"; done

echo "==> web page up to date?"
python3 scripts/build-web.py --check

echo "==> C core + host agent"
cmake -S agent -B build/agent -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build/agent
./build/agent/common/test_ndp
./build/agent/posix/test_fs
./build/agent/posix/test_write

echo "==> TypeScript bridge"
(cd bridge && npm install --no-audit --no-fund >/dev/null && npx tsc -p tsconfig.json --noEmit && npm test)

if [ "${1:-}" = "--3ds" ]; then
  echo "==> 3DS build"
  ./scripts/build-3ds.sh
  # the update-manifest logic is pure C; it needs a host Jansson (e.g. brew/apt libjansson-dev) to run
  if cc -I agent/3ds/source agent/3ds/tests/update_manifest_test.c agent/3ds/source/update_manifest.c -ljansson -o build/update_manifest_test 2>/dev/null; then
    ./build/update_manifest_test
  else
    echo "(skipping update_manifest_test: no host Jansson)"
  fi
fi

echo "==> all checks passed"
