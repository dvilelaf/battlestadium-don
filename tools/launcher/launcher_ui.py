"""The variant pickers: Tk window with a terminal fallback."""
from __future__ import annotations

from launcher_common import AVAILABLE, CODES, HELP, MISSING, PROMPT, TITLE, VARIANTS, Cancelled


def make_flag(tk, kind: str, w: int = 26, h: int = 18, gap: int = 8):
    """A small flag drawn pixel by pixel (Tk has no emoji flags on X11)."""
    img = tk.PhotoImage(width=w + gap, height=h)
    put = img.put
    if kind == "jp":
        put("#FFFFFF", to=(0, 0, w, h))
        cx, cy, r = (w - 1) / 2.0, (h - 1) / 2.0, h * 0.3
        for y in range(h):
            for x in range(w):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    put("#BC002D", to=(x, y))
        for x in range(w):                      # a thin border so the white shows on a white window
            put("#999999", to=(x, 0))
            put("#999999", to=(x, h - 1))
        for y in range(h):
            put("#999999", to=(0, y))
            put("#999999", to=(w - 1, y))
        return img
    blue, white, red = "#012169", "#FFFFFF", "#C8102E"
    put(blue, to=(0, 0, w, h))
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    span = max(w - 1, 1)
    for y in range(h):
        for x in range(w):
            d1 = abs(y - (h - 1) * x / span)
            d2 = abs(y - (h - 1) * (w - 1 - x) / span)
            if d1 <= 3 or d2 <= 3:
                put(white, to=(x, y))
            if d1 <= 1.2 or d2 <= 1.2:
                put(red, to=(x, y))
            if abs(x - cx) <= 3 or abs(y - cy) <= 2:
                put(white, to=(x, y))
            if abs(x - cx) <= 1.5 or abs(y - cy) <= 1:
                put(red, to=(x, y))
    return img


def pick_gui(table, preselect: str) -> str:
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    style = ttk.Style(root)
    for theme in ("clam", "alt", "default"):
        if theme in style.theme_names():
            style.theme_use(theme)
            break
    try:
        from tkinter import font as tkfont
        tree_font = tkfont.Font(size=-14)   # negative sizes are pixels: HiDPI does not clip rows
        style.configure(".", font=tkfont.Font(size=-12))
        style.configure("TLabel", font=tkfont.Font(size=-16))
        style.configure("TButton", font=tkfont.Font(size=-14), padding=(12, 7))
        style.configure("Treeview", font=tree_font, rowheight=tree_font.metrics("linespace") + 10)
    except Exception:
        pass
    root.title(TITLE)
    root.resizable(False, False)
    chosen = {"code": None}
    states = {code: (state, detail) for code, _label, state, detail in table}

    ttk.Label(root, text=PROMPT).pack(padx=20, pady=(14, 8))
    flags = {code: make_flag(tk, VARIANTS[code][1]) for code in CODES}
    tree = ttk.Treeview(root, show="tree", height=len(CODES), selectmode="browse")
    tree.column("#0", width=380, minwidth=300, anchor="w", stretch=True)
    for code, label, state, _detail in table:
        shown = label if state != MISSING else f"{label}  (not available)"
        tree.insert("", "end", iid=code, text=shown, image=flags[code])
    usable = [c for c, _l, s, _d in table if s != MISSING]
    pre = preselect if preselect in usable else (usable[0] if usable else CODES[0])
    tree.selection_set(pre)
    tree.focus(pre)
    tree.pack(padx=20, pady=6, fill="x")
    status = tk.StringVar(value=states[pre][1] if states[pre][0] == MISSING else "")
    ttk.Label(root, textvariable=status, foreground="#a02020", font=None).pack(padx=20)
    ttk.Label(root, text=HELP).pack(padx=20, pady=(6, 0))

    def on_select(_evt=None):
        sel = tree.selection()
        if sel:
            state, detail = states[sel[0]]
            status.set(detail if state == MISSING else "")

    def accept(_evt=None):
        sel = tree.selection()
        if sel and states[sel[0]][0] != MISSING:
            chosen["code"] = sel[0]
            root.destroy()

    buttons = ttk.Frame(root)
    buttons.pack(pady=18)
    ttk.Button(buttons, text="OK", width=12, command=accept).pack(side="left", padx=8)
    ttk.Button(buttons, text="Cancel", width=12, command=root.destroy).pack(side="left", padx=8)
    tree.bind("<<TreeviewSelect>>", on_select)
    tree.bind("<Double-1>", accept)
    root.bind("<Return>", accept)
    root.bind("<Escape>", lambda _e: root.destroy())
    root.protocol("WM_DELETE_WINDOW", root.destroy)   # closing = cancel
    root.update_idletasks()
    w, h = root.winfo_reqwidth(), root.winfo_reqheight()
    x = max(0, (root.winfo_screenwidth() - w) // 2)
    y = max(0, (root.winfo_screenheight() - h) // 2)
    root.geometry(f"+{x}+{y}")
    root.lift()
    root.attributes("-topmost", True)
    root.after(500, lambda: root.attributes("-topmost", False))
    root.focus_force()
    root.mainloop()
    if not chosen["code"]:
        raise Cancelled()
    return chosen["code"]


def pick_terminal(table, preselect: str) -> str:
    print(PROMPT + ":")
    for i, (code, label, state, detail) in enumerate(table, 1):
        mark = "*" if code == preselect else " "
        note = "" if state == AVAILABLE else f"  [{detail}]"
        print(f" {mark}{i}) {code:<11} {label}{note}")
    print(HELP)
    try:
        answer = input("number or code, empty to cancel > ").strip()
    except EOFError:
        raise Cancelled()
    if not answer:
        raise Cancelled()
    if answer.isdigit() and 1 <= int(answer) <= len(table):
        code = table[int(answer) - 1][0]
    else:
        code = answer.lower()
    usable = [c for c, _l, s, _d in table if s != MISSING]
    return code if code in usable else ""


def pick(table, preselect: str) -> str:
    try:
        return pick_gui(table, preselect)
    except Cancelled:
        raise
    except Exception:                      # no Tk, or no display
        code = pick_terminal(table, preselect)
        if not code:
            raise Cancelled()
        return code

