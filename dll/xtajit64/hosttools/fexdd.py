#
# PROJECT:     ReactOS x64 emulator
# LICENSE:     MIT (https://spdx.org/licenses/MIT)
# PURPOSE:     The part of dd the FEX build uses: copy blocks into a file in place
# COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
#

import sys

args = dict(arg.split('=', 1) for arg in sys.argv[1:])
block = int(args.get('bs', '512'))
count = int(args['count']) if 'count' in args else None
skip = int(args.get('skip', '0'))
seek = int(args.get('seek', '0'))

with open(args['if'], 'rb') as source:
    source.seek(skip * block)
    data = source.read(count * block if count is not None else -1)

# conv=notrunc is the only conversion FEX asks for, the rest of the file stays
with open(args['of'], 'r+b') as target:
    target.seek(seek * block)
    target.write(data)
