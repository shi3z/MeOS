# Shared application support

Depends on the existing foundation/SD integration through upstream PR #14.
Adds locale-independent number conversion/math, shell numeric expressions, and
cooperative fullscreen claims shared by WASM and future native applications.
Legacy GUI applications do not participate in this claim; it is not isolation.
The main component watches source globs only outside IDF script discovery mode.
An explicit M5OS_IDF_P4 overrides the locally installed default SDK.

Run `sh tools/common/host_test.sh`. Native clients and their tests follow in
separate PRs. This does not assign or change the MeOS license.
