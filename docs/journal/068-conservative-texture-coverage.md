# 068 · Expanding texture coverage without wrong-content replacements

- Date: 2026-11-16
- Task: local owner pack expansion / texture conversion support
- Status: done

## Problem

The first converter selected candidates using average16x16 thumbnail differences. Sparse masks and
palette-corrupted dumps could look close after averaging, so an unrelated picture could be accepted.
Rerunning into an existing pack also left previously accepted files active even when later rejected.

## Fix

Native-size validation now checks alpha intersection/union, centred spatial RGB correlation and
edge correlation. Exact constant colours have a narrowly scoped RGBA fallback. Uncertain matches
keep the original PS2 image. Reports include metrics and rejected-content rows. Existing PNG outputs
are refused instead of silently retaining stale replacements; non-PNG notes are preserved.

## Verification

Synthetic-only tests exercise a true scaled/alpha128 match, unrelated content, sparse wrong shapes
with identical thumbnails, flat colours, transparency mismatches, one-pixel/thin images and existing
output protection. Eight tests passed after the final code change. No game images are in the tests.

A full save exposes20 characters and11 stages. Existing dumps/route logs were reused, and additional
practice routes cover the eight initially locked characters and missing stages. The texture pack
itself remains outside this repository. Final counts and in-game evidence are recorded with the
local pack manifest, because assets must not be committed.

## Limits

The source HD pack is an older filtered enlargement, not newly redrawn artwork. A successful content
match does not certify its artistic quality. Some flat/low-detail legitimate pairs need individual
visual review. Covering every character/stage does not imply every texture has an HD counterpart.
