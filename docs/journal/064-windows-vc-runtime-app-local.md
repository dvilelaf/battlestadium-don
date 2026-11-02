# 064 · The Windows zip carries the Visual C++ runtime DLLs it imports

- **Date:** 2026-10-28
- **Task:** T-770
- **Status:** resolved

## Symptom
The Windows zip shipped no Microsoft C/C++ runtime DLLs (knowledge 17 P13); a PC without the VC++ redistributable could not start it, and Wine refused to
start it without `msvcp140_atomic_wait.dll` (journals 056, 058, 063). Owner's decision 2026-10-28: ship them app-local.

## Investigation
- Source: `https://aka.ms/vs/17/release/vc_redist.x64.exe` redirects to Microsoft's content-addressed URL (the SHA-256 is part of the path);
  that URL, the SHA-256 `cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b` and the version `14.44.35211.0` are pinned in
  `tools/pipeline/vcredist-windows.sha256`. It is the same file as the owner's earlier `work/vcredist/` extraction.
- Unpacking, without running it (`tools/pipeline/fetch_vcredist.py` into `third_party/vcredist/x64/`): the PE is a WiX burn bundle with two cabinets after
  its last section (offsets 479744 and 686152, with 10368 bytes of padding between). The first holds the bundle manifest (`0`), with a SHA-1 per payload and
  the English `license.rtf`; the second (the attached container) holds the x64 packages' cabinets `vcRuntimeMinimum_amd64` (12 DLLs, members named
  `<file>.dll_amd64`) and `vcRuntimeAdditional_amd64` (MFC). 7-Zip opens both cabinets. Each payload's SHA-1 is checked against the manifest; each of the
  26 DLLs is checked to be an x64 PE with file version `14.44.35211.0`, so all come from one redistributable version.
- Which DLLs: computed, not listed. `package_checks.pe_imports` reads the import and delay-import tables of every shipped PE (not `toolchain/`), and
  `package_windows.place_vc_runtime` copies the matching runtime DLLs beside the PE that imports them, repeating until nothing is missing (the runtime DLLs
  import each other). Measured on this package: `Battle Stadium DON.exe`, `don_gs/vu/spu2.dll` and the dependency DLLs need `msvcp140.dll`,
  `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`; `bin/ps2_recomp.exe` (built `/MT`) imports none, so `bin/` holds none. No
  `concrt140`, `vcomp140` or other appears. The UCRT (`api-ms-win-crt-*`, `ucrtbase.dll`) is part of Windows 10+ and is not shipped.
- Licence: the bundle's `license.rtf` ("Microsoft Software License Terms, Microsoft Visual C++ 2015-2022 Runtime") ships as
  `licenses/Microsoft-VCRedist-license.rtf`; `THIRD_PARTY_NOTICES.md` and `LEGAL.md` say the files are Microsoft's, unmodified, under Microsoft's terms and not the GPL.
- Check: `package_checks.check_vc_runtime` fails the packaging when a shipped PE imports a runtime DLL that is not in its folder (pytest on fake PEs,
  `tests/pipeline/test_vcredist.py`).
- The sandbox's `third_party/` is read-only, so the run used `fetch_vcredist.py --cache ... --dest ...` and `package_windows.sh --vcredist ...` pointing at a scratch
  folder; the defaults are `third_party/vcredist/x64`.

## Verification
- Zip of commit `40f41b4` (`package_windows.sh --jobs 4`, tools build and `fetch_toolchain.py --platform windows` as in journal 063): `don-windows-40f41b4.zip`,
  86101776 bytes, sha256 `84423f3330b058d0897b89f7415d842d9d6f2581838edfbc82caad98c87f84f2`. Build id `40f41b4df753-ffmpeg` (no `-dirty`). It holds
  `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` beside the exe and `licenses/Microsoft-VCRedist-license.rtf`.
- Wine 6.0.3, fresh prefix, `WINEDLLOVERRIDES="msvcp140,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n"`, empty `DON_USER_DIR` (`Z:\` path), `--headless`: exit 2,
  `[don] the game is not prepared: run  don --prepare <your disc image>`. `WINEDEBUG=+loaddll` shows the four DLLs loaded from the unzipped folder. The prefix's
  `msvcp140*`/`vcruntime140*` files are the same 8 before and after (nothing copied in; `msvcp140_atomic_wait.dll` is not in a fresh prefix at all).
- Not run: `--prepare`, or a Windows PC without the redistributable (the orchestrator's check on the PC).
- The packaged commit predates one log-only edit (the "placed" list no longer repeats a name); the zip's contents do not depend on it.
