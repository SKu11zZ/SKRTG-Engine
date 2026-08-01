# Contributing to SKRTG Engine

SKRTG is strict by design: a clear failure with useful evidence is better than
an animation produced from an unproven mapping. Contributions should keep that
property intact.

## Development loop

1. Create a focused branch.
2. Keep builds and test data outside tracked source.
3. Put reusable behavior in a library; keep `apps/` and `tools/` entry points
   thin.
4. Add tests at the same layer as the contract being changed.
5. Build the complete tree and run CTest with `--output-on-failure`.
6. Run `skrtg capabilities --json` and the relevant preflight against a real,
   private fixture without copying that fixture into Git.
7. Review `git diff --check` and the asset boundary before publishing.

See [Project layout](docs/PROJECT_LAYOUT.md) and the
[unified CLI guide](docs/CLI.md) for the current entry points.

## Public data boundary

The repository may contain source, tests, neutral synthetic examples, JSON
Schemas, and documentation. It must not contain character/animation assets,
Unreal projects, production exports, generated profiles or review packages,
compiled binaries, or release archives. `.gitignore` is a guardrail, not a
substitute for reviewing what is staged.

## Machine contracts

Do not change an existing schema in a way that gives old fields new meaning.
Add a new schema version when compatibility cannot be preserved. CLI stdout in
`--json` mode must remain one valid success or error envelope; diagnostics go
to stderr. New failures need a stable uppercase error code, a stage, details,
and a useful next action.

## Character definitions

Supporting another source file type means adding a registered importer, not a
file-extension guess. Importers produce the normalized Character Definition;
the common validator decides whether it is runtime-complete. A standalone rest
skeleton remains inspectable even when it cannot yet be packaged.

## Pull requests

Describe the user-visible behavior, the contracts affected, the tests run,
and any remaining quality gate. Avoid claiming that a candidate algorithm is
adopted unless that decision is explicitly part of the change.
