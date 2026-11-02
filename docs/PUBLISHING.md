# Publishing checklist

Run through this list before making the repository public or before publishing a release.
Each item has the command that verifies it. Run the commands from the repository root. An item
is checked only when its command gives the expected result, and the result is noted in the
release notes.

Legal model: the repository never contains the game, its data, the generated C++ or binaries
linked with it. Users generate everything from their own disc (see the decision D8 in
`docs/DECISIONS.md`).

## 1. No game file or derivative in the history

- [ ] No game file, disc image or derivative was ever committed (history, all branches).
  Command, review the output by eye (no `.iso`, `.bin`, `.elf`, `.irx`, `.afs`, `.fpk`,
  `generated/` or memory dumps in the list):
  ```bash
  git log --all --name-only --format= | sort -u
  ```
- [ ] The ignored folders that hold game material are not tracked:
  ```bash
  git ls-files -- disc en_patch gc_rvz generated work build third_party
  ```
  Expected: no output.
- [ ] Nothing that is ignored is staged:
  ```bash
  git status --short --ignored
  ```
  Expected: the ignored folders appear only with `!!`, never as added or modified.

## 2. No binary linked with the game in any download

- [ ] No executable, shared library or object file is in the source tree or in a release
  artifact:
  ```bash
  git ls-files | grep -E '\.(exe|dll|so|dylib|elf|o|a|lib|irx|iso|bin)$'
  ```
  Expected: no output.
- [ ] Each release artifact is built from the source by the user's own build, with no
  game data in it: list the archive and check its names (`unzip -l <archive>` or
  `tar -tf <archive>`). Expected: no `.iso`, `.bin`, `.afs`, `.fpk`, `.irx`, texture,
  model, sound or video file, and no `generated/` folder.

## 3. Third-party credits and licenses

- [ ] `LICENSE` is the GNU GPL version 3 text:
  ```bash
  sha256sum LICENSE
  ```
  Expected: `8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903`.
- [ ] PS2Recomp and PCSX2 are GPL-3.0 and are credited in `README.md`:
  ```bash
  grep -n -E 'PS2Recomp|PCSX2' README.md
  ```
  Expected: the credits section names both, with their links.
- [ ] Every patch under `patches/` that comes from another project names its source
  (repository, commit or pull request, file, license) in its header. Check each header:
  ```bash
  head -n 12 patches/ps2recomp/*.patch patches/pcsx2/*.patch
  ```
  Expected: each header that is based on other work names it. Note: `0001` has no comment
  header and `0002` takes its change from an upstream pull request by its author; both need a
  check by a person before publishing.
- [ ] The projects that the README says were studied (Fukami and KillzoneRecomp) are credited,
  and any code taken from them keeps its notice:
  ```bash
  grep -rn -i -E 'fukami|killzone' --include='*' . --exclude-dir=.git
  ```
  Expected: the README credits and, for any copied code, the notice in the file header.

## 4. Metadata up to date

- [ ] `.recomp.json` is valid JSON and its `status` is the real one at publishing time
  (`exploring`, `in-progress`, `playable`, `released`, `complete`, `paused`):
  ```bash
  uv run --no-project python -I -m json.tool .recomp.json
  ```
  Expected: the file printed without an error, and `status` checked by hand.

## 5. No machine-specific paths or user names in the documentation

State on 2026-10-11: the machine-specific paths were removed before the first commit. The
disc image and the BIOS folder are now environment variables (`DON_ISO`, `DON_PS2_BIOS_DIR`)
set in `local.env`, which git ignores (`local.env.example` documents them). To check that no
file that git tracks, or would track, contains such a path, run:

```bash
git ls-files --cached --others --exclude-standard | while IFS= read -r f; do
  [ -f "$f" ] || continue
  n=$(grep -c -e '/media/' -e '/home/' -- "$f")
  [ "$n" -gt 0 ] && echo "$n $f"
done
```

Expected: only this document (the patterns in the command above).

- [ ] The command above prints only this document. A file that needs a path of the machine
  reads it from `local.env` instead of naming it.
- [ ] No e-mail address of the author is left in the repository. Set `AUTHOR_EMAIL` to the
  address to look for (kept out of this document on purpose), then run:
  ```bash
  git ls-files --cached --others --exclude-standard | while IFS= read -r f; do
    [ -f "$f" ] || continue
    n=$(grep -c -F "$AUTHOR_EMAIL" -- "$f")
    [ "$n" -gt 0 ] && echo "$n $f"
  done
  ```
  Expected: no output.
- [ ] Mail addresses that remain belong to upstream authors of commits in patch headers and
  are not personal contacts of the author. Count, by file:
  ```bash
  git ls-files --cached --others --exclude-standard | while IFS= read -r f; do
    [ -f "$f" ] || continue
    n=$(grep -c -E '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' -- "$f")
    [ "$n" -gt 0 ] && echo "$n $f"
  done
  ```
  Expected: only patch headers from upstream authors, each one checked by hand.
- [ ] The author's name and local account name are not left in the documentation. Set
  `AUTHOR_NAME` to the name and account to look for, then run:
  ```bash
  git ls-files --cached --others --exclude-standard | xargs grep -l -i -F "$AUTHOR_NAME" 2>/dev/null
  ```
  Expected: no output once the paths in section 5 are fixed.
