# Reproduce registry variants

Build the Sputnik SDK and select the exact interpreter installation:

```sh
export SPUTNIK_ROOT=/path/to/sputnik-lang
python3 release/providers.py --interpreter /path/to/ruby --out build/release-providers
python3 release/prepare.py build/release-config.json --native source --modules source
python3 release/prepare.py build/release-config.json --native prebuilt --modules source
```

Repeat with `--modules bytecode` to produce bytecode delivery. Use
`--platform linux-x86_64-gnu` when building on Linux x86-64. Published macOS
variants use macOS arm64 >=26.0; Linux variants require glibc >=2.36.
Provider descriptors pin exact interpreter version, ABI and build hash. Keep
standard libraries and third-party modules in the selected installation.
No interpreter or SDK is redistributed in these package archives.

Set `SPUTNIK_NATIVE_PROVIDERS` to the runtime and SDK directories printed by the
provider recipe. On macOS these bindings and the cache must share the
interpreter filesystem, so native modules use the same runtime image.
