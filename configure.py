#!/usr/bin/env python3
"""Prepare this package against an explicitly selected installed interpreter."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
NAME = 'rbcall'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ruby', default=os.environ.get('RUBY', 'ruby'))
a = p.parse_args()
facts = json.loads(subprocess.check_output([a.ruby, '-rjson', '-rrbconfig', '-e', '''c=RbConfig::CONFIG; puts JSON.generate({include:c['rubyhdrdir'],archinclude:c['rubyarchhdrdir'],libdir:c['libdir'],library:c['RUBY_SO_NAME'],shared:c['ENABLE_SHARED']})'''], text=True))
if facts['shared'] != 'yes':
    p.error('selected CRuby must provide a shared libruby')
includes = [facts['include'], facts['archinclude']]
lib = facts['library']
defines = []
for directory in includes:
    if not Path(directory).is_dir():
        p.error('missing interpreter headers: ' + directory)
extension = dict(name=NAME, language='c++', sources=['native/' + NAME + '.cpp'],
                 include_dirs=includes, library_dirs=[facts['libdir']],
                 runtime_library_dirs=[facts['libdir']], link_libraries=[lib] + (['dl'] if sys.platform.startswith('linux') else []), defines=defines)
manifest = dict(schema='sputnik.build.v1', name=NAME, root=NAME,
                profiles=dict(required=['core.v1', 'ffi.v1'], optional=[], forbidden=[]),
                modules=[dict(name=NAME, path='src/' + NAME + '.s')], native_extensions=[extension])
(ROOT / 'sputnik.build.json').write_text(json.dumps(manifest, indent=2) + '\n')
# Package installation uses the same native build facts as consumer manifests.
quote = lambda v: json.dumps(v, ensure_ascii=False)
lines = ['[package]', 'name = ' + quote(NAME), 'version = "0.1.0"', 'root = ' + quote(NAME),
         '', '[[modules]]', 'name = ' + quote(NAME), 'path = ' + quote('src/' + NAME + '.s'), '', '[[native]]']
for key, value in extension.items():
    lines.append(key + ' = ' + quote(value))
(ROOT / 'sputnik.toml').write_text('\n'.join(lines) + '\n')
print('Prepared ' + NAME + ' with ' + lib + ' in ' + facts['libdir'])
