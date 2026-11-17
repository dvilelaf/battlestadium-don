# 069 · Garbage textures after a special move: the DMA chain walker cut chains at 4096 tags

- **Date:** 2026-11-17
- **Task:** the "textures turn to garbage after the special move" reports (journal 041: a friend's video, Naruto;
  now the owner's videos, Luffy on the Hidden Leaf stage)
- **Status:** resolved (patch 0094)

## Symptom

Four fighters on the Hidden Leaf stage, P1 = Luffy. From the hit of his special move on, the stage, the fighters and
the HUD are drawn with other textures (colour noise, "LUFFY" tiles on the floor, white/grey surfaces), every frame of
the move. The same with 1x and 5x, with and without a texture pack, with widescreen on or off, at 100 % battle speed,
with the Vulkan and the software renderer, and with the v1.1 release (`c63905814ee9`): not a renderer or option issue.

## Reproduction

`tools/run/scripts/konoha_ougi.txt` with P1 = Luffy: `4360/4375/4390 tap RIGHT 4` before `4400 tap CIRCLE 6`
(the character select row is Sasuke, Naruto, Goku, Gohan, Luffy, Zoro; P1 starts on Naruto). `DON_PLAY_MOVIES=1`,
`--players 4 --sound off`. The red cut-in plays at ~6540-6630, the hit at ~6650; garbage from blank 6653 on.

## Cause

Per frame the game sends its texture uploads (image + palette) as one GIF-channel chain while PATH3 is masked, and a
VIF1 chain of the shape lift / mask / FLUSHA / DIRECT draw, one lifting per upload packet, ending with the mask lifted
(patch 0042 releases one packet per lifting). Measured per frame (a temporary trace in the chain walker and in
`processVIF1Data`):

| Blank | VIF1 chain tags | GIF packets (EOP) | Liftings | Mask at the end of the VIF1 transfer |
|---|---|---|---|---|
| 6500 | 3726 | 84 | 85 | off |
| 6650 | 4062 | 79 | 80 | off |
| 6652 | **4096** | 79 | 67 | on |
| 6653 | **4096** | 80 | 53 | on |

`PS2Memory`'s chain walker stopped at `kMaxChainTags = 4096` and treated the chain as finished. The tags after that
(the end of the frame: its last draws and liftings, and the final lift of the mask) were never processed, so upload
packets stayed in the masked queue, and the next frame's liftings released the previous frame's uploads: draws used
texture addresses that had not been uploaded that frame (0 such draws per frame before 6653, 17-45 after, in a GIF
capture of 6600-6663).

## Fix

`kMaxChainTags = 1 << 20`: still a stop for a corrupt chain that loops, far above any frame of this game. The console
has no limit. Any character, move or stage that makes a frame's chain longer than 4096 tags was affected.

## Verification

The reproduction on Windows (Vulkan 1x without options; Vulkan 5x with a texture pack and widescreen): the special
move (green fists) and the stage draw correctly through and after the move. `fight_short.txt` (4 fighters) unchanged
in length and ends normally. Built and run on the local battle-speed tree, which carries this same change.
