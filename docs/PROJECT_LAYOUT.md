# Project layout and dependency direction

The repository is organized around libraries first and entry points second.
New commands should reuse library APIs; they should not duplicate engine logic
inside a CLI or Viewer.

```text
apps/
  skrtg/                  unified human/agent CLI
include/skrtg/            public engine headers
src/
  core/                   pose, skeleton, math, validation
  retarget/               route and OpStack runtime
  fbx/                    FBX import, write, and review packaging
  cli/                    low-level diagnostic/worker entry points
native_viewer/
  include/skrtg/viewer/   Viewer, Profile, Batch, Bridge, SKRV APIs
  src/                    reusable implementations
  tools/                  GUI and legacy compatibility executables
  tests/                  Viewer/Profile/Bridge contract tests
schemas/                  versioned machine-facing JSON Schemas
examples/                 asset-free request and definition examples
docs/                     architecture, contracts, and operating guides
tests/                    engine contract tests
```

Dependency direction:

```text
core math/skeleton
      ↓
retarget + FBX runtime
      ↓
Profile / Bridge / Batch / SKRV libraries
      ↓
apps/skrtg and native Viewer
```

The Viewer reads verified SKRV data and orchestrates existing APIs. It does not
own a second retarget algorithm. The unified CLI similarly calls library APIs
directly. Small historic executables remain thin compatibility shells.

Build directories stay out of source. Character FBX, animations, Unreal
projects, exported production definitions, Golden data, profiles, SKRV output,
and compiled distributions stay outside the public repository and enter
through explicit paths or build configuration.

The structure follows the same broad ideas used by mature command-line and
DCC infrastructure projects: a shallow application entry, reusable packages,
versioned machine contracts, out-of-source builds, and acceptance tests. Useful
primary references include [GitHub CLI](https://github.com/cli/cli),
[CLI11](https://github.com/CLIUtils/CLI11), and
[OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD). SKRTG does not
vendor CLI11 in this stage; avoiding a new runtime dependency keeps the first
unified entry point easy to audit.
