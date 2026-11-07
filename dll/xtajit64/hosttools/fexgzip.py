#
# PROJECT:     ReactOS x64 emulator
# LICENSE:     MIT (https://spdx.org/licenses/MIT)
# PURPOSE:     The part of gzip the FEX build uses: compress files next to themselves
# COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
#

import gzip
import os
import shutil
import sys

flags = ''.join(arg[1:] for arg in sys.argv[1:] if arg.startswith('-'))
files = [arg for arg in sys.argv[1:] if not arg.startswith('-')]
level = int(next((c for c in flags if c.isdigit()), '6'))

for name in files:
    # -n leaves the name and time out of the header, so the output only depends on the input
    with open(name, 'rb') as source, open(name + '.gz', 'wb') as raw:
        with gzip.GzipFile(filename='' if 'n' in flags else name, mode='wb', compresslevel=level,
                           fileobj=raw, mtime=0 if 'n' in flags else None) as target:
            shutil.copyfileobj(source, target)
    if 'k' not in flags:
        os.remove(name)
