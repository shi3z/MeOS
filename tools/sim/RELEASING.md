# Publication preparation

Use the name **MeOS App Simulator**. It runs shared application code on the host;
it does not boot MeOS or emulate ESP32-P4 hardware. Keep it in the MeOS repository
while Scheme is the only connected app. No push or release has been performed by
the preparation scripts.

## Reviewable source boundary

`source-files.txt` is an explicit source/headers/tests/notices manifest. It includes
shared MeOS dependencies needed by the simulator, including numeric conversion;
it excludes unrelated BASIC, Doom, hardware-driver and firmware integration work.
Shared header snapshots may contain declarations not exercised by this target.
The manifest is a dependency boundary, not an authorship claim for shared files.

```sh
python3 tools/sim/source_bundle.py --output /tmp/new-meos-source-candidate
sh tools/sim/check-clean.sh
```

The exporter requires a new directory, rejects missing/undeclared simulator files
and symlinks, and records per-file SHA-256 hashes and the checkout's base commit.
This intentionally snapshots declared uncommitted files; it does not pretend they
already belong to the base commit. The clean check creates a local candidate Git
repository and a fresh clone, then builds/tests only from that clone. It leaves
the original checkout, its index and other sessions' changes untouched.

To integrate into the main repository, review the manifest and commit the required
new files and relevant shared changes separately from concurrent features. A
passing local candidate clone is not evidence that those commits have been pushed.

## Technical checks

- `sh tools/scheme/host_test.sh`: ASan/UBSan interpreter/editor model tests.
- `sh tools/sim/replay_test.sh`: strict parser tests, UBSan native build, independent
  record/replay with matching source and four BMPs, modifier/F12 fixture, and a
  nonzero exit on source-load failure.
- `sh tools/sim/check-clean.sh`: declared source closure in a fresh local clone.
- `.github/workflows/simulator.yml`: the clean-clone check on Linux and macOS.
  Merely adding this workflow does not establish that hosted CI has run.
- Each build has its own output directory under `M5OS_SIM_BUILD_ROOT` (default
  `build-host-sim`). Multiple sessions do not replace a common executable.

The current image comparison establishes same-revision replay reproducibility.
Cross-revision golden images and hardware/host visual comparisons remain separate
work. Do not advertise the current test as physical-device qualification.

## Remaining publication decisions

- Confirm the project license and rights over existing/shared MeOS code. Do not
  apply a new blanket license merely because a simulator has been added.
- Resolve the originating font/BDF and generation provenance; preserve all required
  notices. See `THIRD_PARTY_NOTICES.md` and the copied upstream documents.
- Run the added hosted Linux/macOS CI after the candidate has been integrated.
- Confirm the final public commit and source inventory before publishing an
  announcement or distributing binaries.

Source candidates are marked `license-review-required` in `SOURCE_MANIFEST.json`.
They are preparation/test artifacts, not approved releases or blanket licensing
statements. No root LICENSE is generated automatically.
