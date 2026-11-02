"""The game's sceSifFreeIopHeap (0x1146C8) must be bound to the host handler (journal 037).

Its own code sends an RPC to a heap server the port does not serve, so every free was lost and the sound-bank
loader's 0x8000 blocks filled the IOP heap by the third stage. The regression route (work/t210 c1) shows it end to
end; this test pins the binding.
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "game" / "don_overrides.cpp"


def test_free_iop_heap_is_bound():
    text = SRC.read_text()
    table = re.search(r"kHandlerBindings\[\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    assert re.search(r'\{\s*0x001146C8u\s*,\s*"sceSifFreeIopHeap"\s*\}', table)
