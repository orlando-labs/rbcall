#!/usr/bin/env python3
"""Bind an installed interpreter's exact native runtime and development headers.

The interpreter's standard library and third-party modules remain installed on
this host. These descriptors do not constitute a redistributable interpreter.
"""
import argparse,hashlib,json,os,platform,shutil,subprocess,sys
from pathlib import Path
Q=json.dumps
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--interpreter',required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();ruby=Path(__file__).resolve().parents[1].name=='rbcall'
if ruby:
 code="c=RbConfig::CONFIG; puts JSON.generate({version:RUBY_VERSION,includes:[c['rubyhdrdir'],c['rubyarchhdrdir']],library:File.join(c['libdirname'] ? c[c['libdirname']] : c['libdir'],c['LIBRUBY_SO']),link:c['RUBY_SO_NAME']})"
 facts=json.loads(subprocess.check_output([a.interpreter,'-rjson','-rrbconfig','-e',code],text=True));name='cruby'
 if not Path(facts['library']).exists():
  candidates=list(Path('/usr/lib').glob('*/'+Path(facts['library']).name))
  if len(candidates)!=1:raise SystemExit('Cannot locate selected CRuby shared library')
  facts['library']=str(candidates[0])
else:
 code="import json,sys,sysconfig,os; lib=sysconfig.get_config_var('LDLIBRARY'); libdir=sysconfig.get_config_var('LIBDIR'); framework=sysconfig.get_config_var('PYTHONFRAMEWORK'); print(json.dumps(dict(version=platform_version if False else '.'.join(map(str,sys.version_info[:3])),includes=[sysconfig.get_path('include')],library=os.path.realpath(os.path.join(libdir,lib)),link='python'+'.'.join(map(str,sys.version_info[:2])))))"
 facts=json.loads(subprocess.check_output([a.interpreter,'-c',code],text=True));name='cpython'
mac=sys.platform=='darwin';target='macos-aarch64' if mac else 'linux-x86_64-gnu'
root=a.out.resolve();root.mkdir(parents=True,exist_ok=True)
runtime=root/'runtime';sdk=root/'sdk'
for d in [runtime,sdk]:d.mkdir(exist_ok=True)
linkname='lib'+facts['link']+('.dylib' if mac else '.so')
libname=linkname if mac else Path(facts['library']).resolve().name
lib=runtime/'lib'/libname;lib.parent.mkdir(exist_ok=True)
if lib.exists():lib.unlink()
# Interpreter native modules identify their runtime by its original file.
# A hard link retains that identity without mutating the installed library.
if mac:os.link(Path(facts['library']).resolve(),lib)
else:shutil.copy2(Path(facts['library']).resolve(),lib)
# Preserve the interpreter's image identity: extension modules from this
# installation must bind to this same VM instead of loading a second runtime.
if not mac and libname!=linkname: shutil.copy2(lib,runtime/'lib'/linkname)
runtime_id='sha256:'+hashlib.sha256(lib.read_bytes()).hexdigest()
headers=[]
for i,include in enumerate(facts['includes']):
 dest=sdk/'include'/str(i)
 if dest.exists():shutil.rmtree(dest)
 shutil.copytree(include,dest)
 headers.append(dest.relative_to(sdk).as_posix())
# Link against the same install-name-adjusted bytes as the runtime descriptor.
slib=sdk/'lib'/libname;slib.parent.mkdir(exist_ok=True);shutil.copy2(lib,slib)
abi=name+'-'+facts['version']+'-'+target
sdk_id='sha256:'+hashlib.sha256(''.join(str(p.relative_to(sdk))+hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(sdk.rglob('*')) if p.is_file() and p.name not in ['sputnik.toml','checksums.sha256']).encode()).hexdigest()
requirements=[]
for kind,d,build in [('runtime',runtime,runtime_id),('sdk',sdk,sdk_id)]:
 identity=name if kind=='runtime' else name+'-sdk'
 files=[p.relative_to(d).as_posix() for p in sorted(d.rglob('*')) if p.is_file() and p.name not in ['sputnik.toml','checksums.sha256']]
 lines=['schema = "sputnik.package-manifest.v1"','[package]','kind = "native"','name = "local-'+identity+'"','version = "'+facts['version']+'"','[target]','os = '+Q('macos' if mac else 'linux'),'arch = '+Q('aarch64' if mac else 'x86_64'),'[[native_providers]]','id = '+Q(identity),'kind = '+Q(kind),'version = '+Q(facts['version']),'abi = '+Q(abi),'build_id = '+Q(build)]
 if kind=='runtime':
  lines+=['library_dirs = ["lib"]','link_libraries = '+Q([facts['link']]),'files = '+Q(files),'[[native_providers.libraries]]','path = '+Q('lib/'+libname),'role = "dependency"']
 else:
  lines+=['runtime_build_id = '+Q(runtime_id),'include_dirs = '+Q(headers),'library_dirs = ["lib"]','link_libraries = '+Q([facts['link']]),'files = '+Q(files)]
 (d/'sputnik.toml').write_text('\n'.join(lines)+'\n')
 (d/'checksums.sha256').write_text(''.join(hashlib.sha256((d/f).read_bytes()).hexdigest()+'  '+f+'\n' for f in sorted(files+['sputnik.toml'])))
 req=dict(id=identity,kind=kind,version=facts['version'],abi=abi,build_id=build,provider='external')
 if kind=='sdk':req['runtime']=name
 else:req['libraries']=[libname]
 requirements.append(req)
(root/'facts.json').write_text(json.dumps(dict(facts=facts,requirements=requirements),indent=2)+'\n')
print('SPUTNIK_NATIVE_PROVIDERS='+str(runtime)+':'+str(sdk))

bridge=Path(__file__).resolve().parents[1].name
extension=dict(name=bridge,language='c++',sources=['native/'+bridge+'.cpp'],requirements=[r['id'] for r in requirements],defines=['SPUTNIK_'+bridge.upper()+'_ERROR_NAMESPACE="Slowpilot.'+bridge.capitalize()+'"'])
cfg=dict(name=bridge,version='0.1.0',root=bridge,sources=['src/*.s'],native=[extension],native_requirements=requirements,build_include_dirs=facts['includes'],build_library_dirs=[str(sdk/'lib')],build_link_libraries=[facts['link']],install_names={facts['library']:'@rpath/'+libname})
(root.parent/'release-config.json').write_text(json.dumps(cfg,indent=2)+'\n')
