#!/usr/bin/env python3
"""Run VM and strict native conformance against the configured interpreter."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
NAME = ROOT.name
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sputnik', default=os.environ.get('SPUTNIK', str(ROOT.parent / 'sputnik-lang/build/sputnik')))
p.add_argument('--mode', choices=['vm', 'native', 'both'], default='both')
a = p.parse_args()
compiler = str(Path(a.sputnik).resolve()) if '/' in a.sputnik else a.sputnik
configuration = ROOT / 'sputnik.build.json'
if not configuration.exists():
    p.error('run configure.py first')
base = json.loads(configuration.read_text())
for module in base['modules']:
    module['path'] = str(ROOT / module['path'])
for extension in base['native_extensions']:
    for key in ['sources', 'headers', 'include_dirs', 'library_dirs', 'runtime_library_dirs']:
        extension[key] = [str(ROOT / path) for path in extension.get(key, [])]
work = ROOT / 'build/tests'
work.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
env.setdefault('SPUTNIK_NATIVE_RT_CACHE', str(ROOT.parent / 'sputnik-lang/build/interop-runtime-cache'))
cases = [('smoke', NAME + '.smoke', None),
         ('invalid_handle', NAME + '.invalid_handle', 'destroy')]
def check_run(result, expected_error):
    if expected_error:
        assert result.returncode > 0 and expected_error in result.stdout.lower(), result.stdout
        print('PASS invalid handle callback terminates with a lifetime error')
    else:
        if result.returncode: raise SystemExit(result.stdout)
        print(result.stdout.strip())

for case, module_name, expected_error in cases:
    manifest = json.loads(json.dumps(base))
    manifest['name'] = manifest['root'] = module_name
    manifest['modules'].append(dict(name=module_name, path=str(ROOT / 'test' / (case + '.s'))))
    path = work / (case + '.build.json')
    path.write_text(json.dumps(manifest, indent=2) + '\n')
    if a.mode in ['vm', 'both']:
        result = subprocess.run([compiler, 'run', str(path), '--grant', 'ffi'], cwd=ROOT.parent, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
        (work / (case + '.vm.log')).write_text(result.stdout)
        check_run(result, expected_error)
    if a.mode in ['native', 'both']:
        out = work / (case + '.native')
        result = subprocess.run([compiler, 'build', str(path), '--target', 'native', '--require-full-native',
                                 '--out-dir', str(out), '--cache-dir', str(work / 'cache')],
                                cwd=ROOT.parent, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=600)
        (work / (case + '.build.log')).write_text(result.stdout)
        if result.returncode: raise SystemExit(result.stdout)
        summary = json.loads(result.stdout[result.stdout.index('{'):])
        assert summary['native_graph_vm_fallback_code_count'] == 0, summary
        (work / (case + '.build.json')).write_text(json.dumps(summary, indent=2) + '\n')
        result = subprocess.run([str(out / module_name)], cwd=ROOT.parent, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
        (work / (case + '.native.log')).write_text(result.stdout)
        check_run(result, expected_error)
print(NAME + ': all requested checks passed')
