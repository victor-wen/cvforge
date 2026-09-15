# cvforwin documentation

cvforwin is a reusable Windows x64 visual-inspection DLL with a frozen C ABI v1.
A native C host performs one inspection call; the DLL owns camera acquisition,
recipe selection, OpenCV processing, diagnostics, result encoding, and optional
image persistence.

| Document | Contents |
| --- | --- |
| [usage.md](usage.md) | C API lifecycle, configuration and recipe schema summary, example host |
| [build.md](build.md) | Prerequisites, presets, WSL vs Windows, test backends |
| [packaging.md](packaging.md) | Package layout, verification, CI release flow, symbols |

The public contract is `include/cvforwin/cvf_api.h`. Examples live under
`examples/c_host/` (template host) and `config/examples/` (deployment template
configuration and recipes). License notices are in `LICENSES/`.

Nothing in `examples/` or `config/examples/` is a production inspection
algorithm; they are templates that demonstrate the calling and configuration
contracts.
