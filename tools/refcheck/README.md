# Reference check

`run.sh` compiles HyperHDR v22.0.0.0's own colour arithmetic and diffs its results against the C port in `core/`,
value by value, across 20 comparisons: transfer functions, colour-space conversions, the YUV matrices, the HDR
tone mapper over hundreds of thousands of points, the smoothing trajectory and the LED output processing.

Nothing of HyperHDR's is kept in this repository. `run.sh` makes a shallow clone of tag `v22.0.0.0` into
`$WORK` (default `/tmp/hl_refcheck`) and takes everything from there:

- **Whole files**, compiled as they are: `ColorSpace.cpp`, `YuvConverter.cpp`, `InfiniteHybridRgbInterpolator.cpp`.
- **Four line ranges**, cut out at build time. `hdr_to_srgb`, `doToneMapping` and `prepareLCH` sit inside
  `LutCalibrator.cpp`, and the output processing inside `InfiniteProcessing.cpp`; those files pull in Qt and will
  not compile on their own, so the functions are extracted and `#include`d instead.

Each extract is pinned by the SHA-256 of exactly those lines, listed in `run.sh`. If upstream's line numbers move,
the file is edited, or the tag is re-pointed, the check stops and prints the hash it expected rather than quietly
compiling the wrong text.

`harness.cpp` is ours: it restates the upstream structs the extracts need (minus Qt), feeds identical inputs to
both implementations and reports the worst difference per comparison.

HyperHDR is MIT, copyright (c) 2020-2026 awawa-dev; the notice is in `core/LICENSE.HyperHDR`, since `core/` is the
port derived from it.

## Running it

```bash
tools/refcheck/run.sh
```

Needs git and a C++20 compiler with a working libc++ — `CXX` overrides it, which Apple's Command Line Tools
usually make necessary (they can ship empty `c++/v1` include dirs). `WORK` moves the clone and build directory.
