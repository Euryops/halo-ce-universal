#!/bin/bash
# Syntax-check every game unit for the Wii's CPU. Run from the repo root (any
# checkout of halo-ce-universal that has port/wii/); needs docker and the
# devkitpro/devkitppc image (docker pull devkitpro/devkitppc).
#   ./port/wii/check.sh        -> prints "N OK, M FAIL"; the failures are in build/ppc/result.txt
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/ppc
cp port/wii/ppc-syntax-check.sh build/ppc/
python3 - <<'PY'
import json, pathlib
c = json.load(open('port/linux/port.json'))
g = c['game']; ex = set(g['exclude'])
fs = sorted(p.as_posix() for p in pathlib.Path(g['root']).rglob('*.c') if p.as_posix() not in ex)
fs += sorted(p.as_posix() for p in pathlib.Path(c['game_sources']).glob('*.c'))
open('build/ppc/files.txt', 'w').write('\n'.join(fs) + '\n')
open('build/ppc/incs.txt', 'w').write(' '.join('-I"%s"' % d for d in g['include_dirs']))
PY
python3 tools/linux_msvc_semantics.py --output build/ppc/sem.h --all-inlines --tags source --inlines source --inlines port/include/xdk
docker run --rm -v "$PWD":/src -v "$PWD/build/ppc/incs.txt":/incs.txt:ro devkitpro/devkitppc \
  bash /src/build/ppc/ppc-syntax-check.sh > build/ppc/result.txt 2>&1 || true
echo "$(grep -c '^OK' build/ppc/result.txt) OK, $(grep -c '^FAIL' build/ppc/result.txt) FAIL"
