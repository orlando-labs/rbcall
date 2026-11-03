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
# Native SDK/runtime descriptors retain local host paths outside the package.
provider_root = ROOT / 'build/native-providers'
subprocess.run([sys.executable, str(ROOT / 'release/providers.py'), '--interpreter', a.ruby, '--out', str(provider_root)], check=True)
provider_facts = json.loads((provider_root / 'facts.json').read_text())
requirements = provider_facts['requirements']
extension.update(requirements=[item['id'] for item in requirements], include_dirs=[], library_dirs=[], runtime_library_dirs=[], link_libraries=[])
manifest.pop('modules', None)
manifest.update(version='0.1.0', source_root='src', files=['src/' + NAME + '.s', 'native/' + NAME + '.cpp', 'README.md'], native_requirements=requirements)
(ROOT / 'sputnik.build.yaml').write_text(''.join(key + ': ' + json.dumps(value) + '\n' for key, value in manifest.items()))
print('Prepared ' + NAME + '; bind native providers with SPUTNIK_NATIVE_PROVIDERS=' + str(provider_root / 'runtime') + ':' + str(provider_root / 'sdk'))
