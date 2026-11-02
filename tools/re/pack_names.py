#!/usr/bin/env python3
"""Propose the mapping from the FPK pack names of the executable to the indices of DATA.AFS.

Usage: uv run --no-project python -I tools/re/pack_names.py

The executable holds the names as zero-terminated strings such as "fpack/chr/gok/1000.fpk". The
PS2 DATA.AFS does not carry names, so the mapping is a hypothesis. Two hypotheses are tested:

  alphabetical: index N is the N-th unique name in alphabetical order
  appearance:   index N is the N-th unique name in order of appearance in the executable

Excluded names take no part in the numbering: templates (names with '%', such as
fpack/story/00%d/%d%d00.fpk) and every name under fpack/story/ (the templates and the literal
fpack/story/001/1500.fpk and fpack/story/001/1502.fpk). No pack of DATA.AFS has a story folder, so
these names do not refer to a pack of DATA.AFS. They are printed and written as comment lines.

A pair (index, name) is consistent if the expected folder of the name is among the inner folders
of the pack at that index (data_afs_ps2.tsv). The expected folder is the folder of the path
(fpack/chr/gok/1000.fpk -> chr/gok), except for stage packs fpack/stg/NNN....fpk, where it is
stg/NNN (the first three digits of the file name). The hypothesis with more consistent indices is
written to docs/knowledge/data/data_afs_names.tsv. Names are file names only; no game data is
written.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / 'disc' / 'SLPS_256.75'
AFS_TSV = ROOT / 'docs' / 'knowledge' / 'data' / 'data_afs_ps2.tsv'
OUT_TSV = ROOT / 'docs' / 'knowledge' / 'data' / 'data_afs_names.tsv'

N_PACKS = 135
NAME_RX = re.compile(rb'fpack/[a-z0-9_/%]+\.fpk')
STRING_RX = re.compile(rb'[\x20-\x7e]{5,200}\x00')
STAGE_RX = re.compile(r'fpack/stg/(\d{3})\d*\.fpk')


def find_names(data):
    """Return every zero-terminated string of the executable that matches the pack name pattern, in order."""
    names = []
    for m in STRING_RX.finditer(data):
        s = m.group()[:-1]
        if NAME_RX.fullmatch(s):
            names.append(s.decode('ascii'))
    return names


def expected_folder(name):
    """fpack/chr/gok/1000.fpk -> chr/gok; fpack/stg/0010000.fpk -> stg/001"""
    stage = STAGE_RX.fullmatch(name)
    if stage:
        return f'stg/{stage.group(1)}'
    return name[len('fpack/'):].rsplit('/', 1)[0]


def read_pack_folders(path):
    """Return {index: set of inner folders} from the 'folders(n)' column, e.g. 'font(1), charsel(2)'."""
    packs = {}
    with open(path, encoding='ascii') as f:
        next(f)  # header
        for line in f:
            cols = line.rstrip('\n').split('\t')
            idx = int(cols[0])
            packs[idx] = {part.rsplit('(', 1)[0] for part in cols[3].split(', ')}
    return packs


def is_consistent(name, folders):
    """True if the expected folder of the name is one of the folders of the pack. '%d' in a template matches digits."""
    pattern = r'\d+'.join(re.escape(part) for part in expected_folder(name).split('%d'))
    return any(re.fullmatch(pattern, f) for f in folders)


def assign(names, order):
    """Return the ordered names; index i of the list is the AFS index i (only the first N_PACKS are used)."""
    return order(names)


def score(ordered, packs):
    """Return the number of consistent indices over the N_PACKS indices."""
    return sum(1 for i in range(N_PACKS) if i < len(ordered) and is_consistent(ordered[i], packs[i]))


def main():
    data = ELF.read_bytes()
    all_names = find_names(data)
    unique = list(dict.fromkeys(all_names))  # keeps the first appearance
    excluded = [n for n in unique if '%' in n or n.startswith('fpack/story/')]
    names = [n for n in unique if n not in excluded]
    print(f'fpack/ strings found: {len(all_names)} ({len(unique)} unique)')
    print(f'excluded from the numbering (templates with %, and fpack/story/): {len(excluded)}')
    for e in excluded:
        print(f'  excluded: {e}')
    print(f'pack names used for numbering: {len(names)}')

    packs = read_pack_folders(AFS_TSV)
    if len(packs) != N_PACKS:
        print(f'WARNING: {len(packs)} DATA.AFS packs in {AFS_TSV.name}, expected {N_PACKS}')

    hypotheses = {
        'alphabetical': assign(names, sorted),
        'appearance': assign(names, lambda ns: ns),
    }
    results = {}
    for name, ordered in hypotheses.items():
        ok = score(ordered, packs)
        results[name] = (ok, ordered)
        print(f'hypothesis {name}: {ok}/{N_PACKS} consistent ({100 * ok / N_PACKS:.1f} %)')

    winner = max(results, key=lambda h: results[h][0])  # ties go to the first, alphabetical
    ok, ordered = results[winner]
    pct = 100 * ok / N_PACKS
    print(f'winner: {winner} ({pct:.1f} %)')

    mapping = {i: n for i, n in enumerate(ordered) if i < N_PACKS}
    leftover = ordered[N_PACKS:]
    if leftover:
        print(f'{len(ordered)} names for {N_PACKS} packs; leftover after index {N_PACKS - 1}:')
        for n in leftover:
            print(f'  leftover: {n}')

    order_text = 'in alphabetical order' if winner == 'alphabetical' else 'in order of appearance in the executable'
    if len(names) == N_PACKS:
        count_note = f'{len(names)} names for {N_PACKS} packs'
    else:
        count_note = f'{len(names)} names for {N_PACKS} packs, NOT EQUAL, {len(leftover)} leftover'
    lines = [f'# hypothesis: {winner} (index N = N-th unique fpack name {order_text}, '
             f'names under fpack/story/ and templates excluded: {count_note}); '
             f'consistent: {ok}/{N_PACKS} = {pct:.1f} %']
    lines += [f'# excluded (fpack/story/, no pack has a story folder): {e}' for e in excluded]
    lines += [f'# leftover (no index, after index {N_PACKS - 1}): {n}' for n in leftover]
    lines.append('\t'.join(['afs_index', 'proposed_name', 'consistent']))
    for i in range(N_PACKS):
        name = mapping.get(i, '')
        cons = 'yes' if name and is_consistent(name, packs[i]) else 'no'
        lines.append(f'{i}\t{name}\t{cons}')
    OUT_TSV.write_text('\n'.join(lines) + '\n', encoding='ascii')
    print(f'wrote {OUT_TSV.relative_to(ROOT)}: {len(lines)} lines')

    bad = [i for i in range(N_PACKS) if not (i in mapping and is_consistent(mapping[i], packs[i]))]
    print(f'inconsistent rows: {len(bad)}')
    for i in bad:
        name = mapping.get(i, '')
        print(f'inconsistent: index {i} {name or "(no name)"} folders={sorted(packs[i])}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
