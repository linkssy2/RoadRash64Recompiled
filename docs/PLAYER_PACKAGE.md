# Player package layout

Use [package_player.py](../scripts/package_player.py) to create a tidy Windows
or Linux player ZIP from a previously audited clean package. It verifies the
complete input archive, retains required runtime files, consolidates supporting
documents under `docs/`, and verifies the resulting archive. It does not start
the game or publish a release.

## Build a package

Run from the source root with Python 3.11 or newer:

```text
python -B scripts/package_player.py BASELINE_MANIFEST NEW_FOLDER NEW_ZIP REPORT_JSON
```

The baseline manifest must contain `archive`, `archiveSHA256`, and `files`, a
complete map from package-relative filenames to SHA256 hashes. Use an audited
clean archive, never a personalized installation. The output folder, ZIP and
report must be new; the report belongs outside the player folder.

For a rebuilt candidate, pass `--replacements REPLACEMENTS_JSON`. For example:

```json
{
  "RoadRash64Recompiled.exe": {
    "path": "verified-build/bin/RoadRash64Recompiled.exe",
    "sha256": "SHA256_FROM_THE_VERIFIED_BUILD"
  },
  "BUILD_INFO.json": {
    "path": "candidate-info.json",
    "sha256": "SHA256_OF_CANDIDATE_INFO"
  }
}
```

For Linux, replace `RoadRash64Recompiled-x86_64.AppImage` instead. Supply current
`BUILD_INFO.json`, `README.md` and `RELEASE_NOTES.md` when binary identity or
candidate status changes. Replacement names refer to the baseline layout. The
helper accepts only existing executables and supporting root documents; it
regenerates checksums itself. Asset or dependency updates need a separately
audited baseline, rather than an incidental replacement.

Run the offline packaging checks with:

```text
python -B scripts/test_package_player.py
```

## Folder contents

| Location | Contents |
| --- | --- |
| Windows root | `README.md`, both EXEs, `SDL3.dll`, `dxcompiler.dll`, `dxil.dll`, `portable.txt`, and the four folders below. (Packages built before the SDL3 port carry `SDL2.dll` instead.) |
| `assets/` | Shipped UI artwork, achievement badges, fonts and their notices. Keep their runtime paths and verified bytes. |
| `tools/mk64-importer/` | Complete frozen importer and its runtime. Its Python/NumPy files are required; they are not a development SDK. |
| `music/` | The clean package includes only `README.txt`; players may add their own music. |
| `docs/` | Release notes, controller help, credits, legal notices, licenses, build identity and checksums. |
| Linux root | `README.md`, the AppImage, `docs/`, and importer notices under `tools/`. Runtime assets and tools are inside the AppImage. |

Linux dependency sources and notices reside under `docs/redistribution/`.
`docs/SHA256SUMS.txt` paths are relative to the **game-folder root** and cover
every other packaged file. The external report records the complete manifest
and the ZIP's SHA256. The Windows layout has 11 top-level entries; Linux has 4.

## Player data and updates

This packaging change preserves runtime storage paths. Windows portable builds
keep settings and the configured ROM beside the EXE, with saves, installed mods,
custom music and imported race packs in their existing folders. Linux AppImage
settings and the configured ROM use the normal configuration directory; custom
music and imported race packs remain beside the writable AppImage location.

Do not put ROMs, extracted gameplay assets, imported course packs, installed
texture mods, saves, settings, custom music, logs or captures in a distributable
ZIP. Preserve the shipped achievement badges and launcher art. The optional
texture mod remains a separate download with its own identity and notices.

Keep the previous installation intact when preparing an update. For a local
private candidate, create and verify the clean archive first, then use
[stage_private_test_state.py](../scripts/stage_private_test_state.py) with its
complete destination manifest to copy the explicitly supported player files.
That helper excludes imported course packs: preserve existing c40 `race-packs/`
separately with hash verification. Never package the personalized folder.

For final verification, compare every Windows asset/importer/DLL against the
audited baseline and the new executable against its build manifest. Linux also
requires checking the new AppImage's internal assets, importer and library
inventory; verifying the outer ZIP alone does not establish those identities.
Successful packaging and offline tests do not establish visual acceptance.
