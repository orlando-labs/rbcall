# rbcall

Embed CRuby in Sputnik and call existing Ruby libraries with ordinary blocks:

```sputnik
from rbcall.json import JSON as json
from rbcall.tempfile import Tempfile

print(json.generate([1, 2, 3]))
text = Tempfile.create("sputnik") |file|:
  file.write("hello")
  file.rewind()
  file.read()
print(text)
```

`from rbcall.json import JSON` requires `json` and fetches the actual top-level
Ruby constant `JSON`. No constant names are guessed. Dots in the library path
become require-path separators: `from rbcall.net.http import Net` requires
`net/http`; use `Net.HTTP` for its nested constant. Dependencies must already be
installed in the selected Ruby environment. Ruby's normal `require` cache applies.
Imported aliases are read-only. A plain `import rbcall.json` is rejected because
a require path has no module value. For explicit dynamic loading use
`rbcall.require(path)` followed by `rbcall.constant(name)`.

## Prepare and install

Use the updated sibling Sputnik compiler and a CRuby installation with headers
and a shared `libruby`:

```sh
python3 configure.py --ruby /path/to/ruby
../sputnik-lang/build/sputnik pkg install "$PWD"
```

Configuration generates local `sputnik.build.json` and `sputnik.toml`; regenerate
these for each host/Ruby installation. No Ruby dependency is added to the Sputnik
compiler. On the development Mac this was tested with CRuby 4.0.5, macOS arm64.
Linux shared-library layouts are handled by configuration but have not been
exercised here. Windows is not supported.

Run `sputnik run app.s --grant ffi` or build with
`sputnik build app.s --require-full-native -o app`. The resulting binary depends
on the configured CRuby library and its installed gems.

## Methods, objects and values

Methods, positional arguments, keyword arguments and the anonymous block channel
are forwarded separately. Ruby modules/classes/objects remain `rbcall.Object`
proxies; `.new(...)` and Sputnik call syntax both construct Ruby classes.
`getattr("Name")` retrieves a Ruby constant; `invoke(name, ...)` explicitly sends
a method and resolves collisions with bridge helpers. Attribute setters use
`setattr!(name, value)`; indexing and item assignment use normal `[]`/`[]=`.
`repr()` returns Ruby `inspect`.

Simple values convert automatically: nil/Null, booleans, Int64, floats, UTF-8
strings, binary strings/Bytes and Symbols. Sputnik arrays/tuples and maps copy
recursively into Ruby arrays/hashes; cyclic/deep input containers are rejected.
Returned Ruby collections remain proxies. `to_a` and `to_map` copy one level;
nested foreign objects remain proxies. `rbcall.wrap(value)` explicitly makes a
Ruby proxy from Sputnik data. `rbcall.eval(source)` executes Ruby in the embedded
VM and returns a converted value. Ruby exceptions become rescuable `rbcall.Error`
containing their Ruby class, message and backtrace.

## Blocks

```sputnik
import rbcall

items = rbcall.wrap([1, 2, 3])
doubled = items.map |x|: x * 2
print(doubled.to_a)

found = items.each |x|:
  break x if x > 1
print(found)  # 2
```

The adapter unpacks a single Ruby Array yield for multi-parameter blocks (for
example `Hash#each |key, value|`) and fills missing required parameters with
Null. Extra arguments follow Sputnik's block binding rules.

`next` returns from the current Sputnik block invocation. `break value` exits the
Ruby invocation to which the block was originally supplied and becomes that
invocation's result, including for `map`. Both runtimes' `ensure` blocks run.
`return` retains Sputnik's non-local return target. A `break` inside an explicit
Sputnik `while`/`loop` still exits that loop.

Sputnik iterator blocks continue to reject foreign `break`: known literal
receivers produce a compile-time diagnostic, dynamic receivers reject at block
entry. The compiler records this effect in the block rather than guessing from
the method name `each`/`map`.

Ruby may retain the block (`&block`) and invoke it later. Sputnik captures are
rooted until Ruby releases the Proc. A retained block cannot `break` after its
original Ruby invocation has returned: this produces `rbcall.Error` containing
Ruby `LocalJumpError`. Calling it cannot attach `break` to a later invocation.
Ruby DSLs that change `self` with `instance_eval` do not rebind Sputnik `self`;
write an explicit receiver adapter for such DSLs. Foreign callbacks currently
support positional yield arguments, not keyword yields.

## Execution and lifetime

A dedicated carrier owns the Ruby VM; arbitrary Sputnik worker threads never
enter CRuby directly. Blocking FFI waits free scheduler workers. When Ruby yields,
Sputnik executes the block on its invoking FFI thread, preserving task context.
The carrier services reentrant calls while waiting for that block. Foreign calls
on this carrier serialize. Autonomous Ruby threads cannot invoke Sputnik blocks
outside an active bridge invocation on the carrier.

Ruby objects live in a GC-rooted handle table. Sputnik GC queues releases without
entering Ruby. `destroy!` tombstones the proxy immediately and queues release;
Ruby applies queued releases at later bridge entries. Callback roots are removed
after Ruby Proc finalization and a later bridge entry. The VM and trampoline
library intentionally live until process exit. Restart/unload, multiple Ruby VMs,
automatic `self` rebinding and arbitrary-thread callbacks are not implemented.
An uncooperative foreign operation must finish before task cancellation retires it.

## Tests

```sh
python3 test/run.py
```

Checks run in the VM and strict full native, including retained callbacks after
Ruby GC, kwargs, nested calls, `break`/`return`, exceptions and `ensure` in both
runtimes. Build summaries and execution logs are saved under `build/tests/`.

## Notebook environments

Notebook workers discover the project's Gemfile and activate `bundler/setup`
before the first Ruby call, using the project's .bundle/config and frozen lockfile
resolution. Worker restart is required to change the environment. Remote core
installs a per-notebook bundle on the server and preserves it across reconnects
and worker restarts. Interpreter and native-gem ABI must match this bridge's SDK.
See the [environment contract](../sputnik-notebook/docs/engineering/notebook-foreign-environments.md).

## Registry release

The registry reference is `slowpilot/rbcall@0.1.0`. Source and prebuilt
adapters pin the exact interpreter version, ABI and native build digest. The
interpreter, its standard library and installed third-party libraries stay on
the consumer machine. No developer include/library path is in the archive.

Create local provider descriptors from the selected installed interpreter:

```sh
python3 release/providers.py --interpreter /path/to/ruby --out build/release-providers
export SPUTNIK_NATIVE_PROVIDERS="$PWD/build/release-providers/runtime:$PWD/build/release-providers/sdk"
sputnik pkg install slowpilot/rbcall --version 0.1.0 --allow-native
```

Add `--build-from-source` for an adapter built locally. Descriptor identity must
match the selected published variant; merely sharing a major version does not
substitute for the declared runtime and SDK. Registry imports use the publisher
prefix, for example `from slowpilot.rbcall.json import JSON`.

On macOS the provider/cache and selected interpreter must be on the same
filesystem. The runtime image is hard-linked so installed native modules use
the same interpreter instance; no interpreter library bytes are modified.
