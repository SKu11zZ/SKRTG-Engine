# Asset policy

This repository stores implementation source, build files, tests, and
third-party source notices.

It does not store:

- character meshes or skeleton FBX files;
- animation FBX or BVH files;
- Unreal projects, packages, maps, or direct engine assets;
- exported IK Rig, IK Retargeter, or animation Golden JSON;
- SKRV review packages;
- `.skrtgprofile` character packages and their installed contents;
- raw screenshots, videos, audit captures, or sample output;
- compiled executables, SDK binaries, or redistributable archives.

A small, explicitly approved set of sanitized application captures may be
tracked under `docs/` as documentation. A documentation capture must not show
private filesystem paths, account identifiers, unpublished configuration, or
embed a redistributable source asset. This exception does not permit raw
capture sets, animation/character files, SKRV packages, or compiled binaries.

Private data belongs in a directory outside the repository and is supplied
through `SKRTG_RETARGET_ASSET_CATALOG_DIR` at configure time. The catalog and
all files it references remain local build inputs. Git ignore rules provide a
second guard, but release owners must still run the repository audit before
publishing.

The application does not download example assets or synthesize mappings when
the catalog is absent.

The `.skrtgprofile` implementation, schema documentation, packer, inspector,
installer, and tests are public source. Real character packages remain private
because they contain FBX and exported UE configuration. `*.skrtgprofile` is
therefore ignored even if a package is created inside the repository by
mistake.
