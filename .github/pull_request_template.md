## What and why

<!-- What does this change do, and why? Link the issue/milestone (docs/04). -->

## Checklist

- [ ] Builds without warnings (`debug` and `release` presets)
- [ ] New/updated tests; a bug fix includes a test that reproduces the bug
- [ ] `asan` preset is green
- [ ] Every Windows resource is in a RAII wrapper; no raw owning pointers
- [ ] External input has limits (`Limits.hpp`) and, if it is a parser, a fuzz target
- [ ] No `<windows.h>` in `libs/domain`
- [ ] Changes to the system implement the `IModule` contract and pass the contract test
- [ ] The feature is extensible (providers / actions / config schema) — docs/12
- [ ] `.\scripts\format.ps1` applied
- [ ] Docs / ADR updated if the design changed
