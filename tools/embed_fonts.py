#!/usr/bin/env python3
# Writes src/ext/fonts_data.cpp from OLAAF's UI fonts in third_party/olaaf-fonts (built from Mulish and
# Red Hat Mono by tools/make_fonts.py; SIL Open Font License 1.1).
import os
root = os.path.join(os.path.dirname(__file__), '..')
d = os.path.join(root, 'third_party/olaaf-fonts')
out = ['// Generated from third_party/olaaf-fonts (Mulish OLAAF, Red Hat Mono OLAAF; SIL Open Font License 1.1).',
       '// Do not edit: regenerate with tools/embed_fonts.py.', '#include <cstddef>', 'namespace olaaf {']
names = []
for f in ['MulishOLAAF-Regular', 'MulishOLAAF-Bold', 'RedHatMonoOLAAF-Regular', 'RedHatMonoOLAAF-Bold']:
    b = open(os.path.join(d, f + '.ttf'), 'rb').read()
    v = f.replace('-', '_')
    names.append((v, len(b)))
    out.append(f'extern const unsigned char k{v}[{len(b)}] = {{')
    for i in range(0, len(b), 24):
        out.append(''.join(f'{x},' for x in b[i:i + 24]))
    out.append('};')
out.append('struct EmbeddedFont { const unsigned char *data; size_t size; };')
out.append('extern const EmbeddedFont kEmbeddedFonts[] = {' + ', '.join(f'{{k{v}, {n}}}' for v, n in names) + ', {nullptr, 0}};')
out.append('} // namespace olaaf')
open(os.path.join(root, 'src/ext/fonts_data.cpp'), 'w').write('\n'.join(out) + '\n')
