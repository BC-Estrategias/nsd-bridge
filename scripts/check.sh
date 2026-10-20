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
  # the installer's write pipeline (threads and a bounded queue) is the kind of code that deadlocks in subtle ways: its real
  # source runs here against a mock of the 3DS services, with slow/failing writes and aborts, and under ThreadSanitizer
  mkdir -p build
  awk '/^#define BLOCK/{p=1} /^\/\* -+ probing/{p=0} p' agent/3ds/source/installer.c > build/pipeline_section.inc
  cc -O1 -g -pthread -Wno-unused-function -I build -I agent/common/include agent/3ds/tests/installer_pipeline_test.c agent/common/src/ndp_sha256.c -o build/installer_pipeline_test
  ./build/installer_pipeline_test
  if cc -fsanitize=thread -g -pthread -Wno-unused-function -I build -I agent/common/include agent/3ds/tests/installer_pipeline_test.c agent/common/src/ndp_sha256.c -o build/installer_pipeline_test_tsan 2>/dev/null; then
    ./build/installer_pipeline_test_tsan
  fi
  # which pieces of a download are on the card (pure C)
  cc -Wall -Wextra -I agent/3ds/source agent/3ds/tests/dlmap_test.c agent/3ds/source/dlmap.c -o build/dlmap_test && ./build/dlmap_test
  # UTF-16 to UTF-8 for the title names (pure C)
  cc -Wall -Wextra -I agent/3ds/source agent/3ds/tests/utf16_test.c agent/3ds/source/utf16.c -o build/utf16_test && ./build/utf16_test
  # the Translations logic: WebDAV answer, pack names, safe paths, pacote.json (pure C), also under ASan/UBSan
  cc -Wall -Wextra -I agent/3ds/source agent/3ds/tests/trpack_test.c agent/3ds/source/trpack.c -o build/trpack_test && ./build/trpack_test
  cc -g -fsanitize=address,undefined -fno-sanitize-recover=all -I agent/3ds/source agent/3ds/tests/trpack_test.c agent/3ds/source/trpack.c -o build/trpack_test_asan 2>/dev/null && ./build/trpack_test_asan
  # the CIA header parser (pure C)
  cc -Wall -I agent/3ds/source agent/3ds/tests/cia_info_test.c agent/3ds/source/cia_info.c -o build/cia_info_test && ./build/cia_info_test
  # the update-manifest logic is pure C; it needs a host Jansson (e.g. brew/apt libjansson-dev) to run
  if cc -I agent/3ds/source agent/3ds/tests/update_manifest_test.c agent/3ds/source/update_manifest.c -ljansson -o build/update_manifest_test 2>/dev/null; then
    ./build/update_manifest_test
  else
    echo "(skipping update_manifest_test: no host Jansson)"
  fi
fi

echo "==> all checks passed"
