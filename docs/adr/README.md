# Architecture Decision Records

Short records of the load-bearing decisions behind this exporter, with the
context and consequences that make them hard to reverse casually. Read these
before changing core behavior. The running log of *what changed* lives in
`../changelog.md`; these ADRs explain *why*.

| ADR | Title | Status |
| --- | --- | --- |
| [0001](0001-usd-only-scope.md) | USD-only export scope | Accepted |
| [0002](0002-self-written-usda.md) | Write `.usda` ourselves; usd-core only validates | Accepted |
| [0003](0003-module-encapsulation.md) | Encapsulate in one Base COMP with a Python extension | Accepted |
| [0004](0004-streaming-export.md) | Streaming, memory-bounded export to a single `.usda` | Accepted |
| [0005](0005-native-mesh-tessellation.md) | Native TD Mesh tessellation; single Poly ingestion path | Accepted |
| [0006](0006-usd-core-validation-venv.md) | usd-core validation in an isolated venv | Accepted |
| [0007](0007-binary-usdc-via-transcode.md) | Static binary `.usdc` via sidecar transcode | Accepted |
| [0008](0008-distribution-tox-external-source-sidecar.md) | Distribution via `.tox`, external source, and reproducible sidecar | Accepted |
| [0009](0009-playback-driven-animated-export.md) | Playback-driven animated export | Accepted |
| [0010](0010-animated-usdc-binary-chunks.md) | Animated `.usdc` via binary chunk sidecar | Accepted |
| [0011](0011-experimental-native-backends.md) | Experimental native backends are opt-in acceleration paths | Accepted |
