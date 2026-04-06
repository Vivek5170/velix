"""
display.py — Velix Terminal output helpers.

THREE OUTPUT CONTEXTS — never mix them:

  [A] Before patch_stdout() is active (banner, session-picker, startup print)
      → Use rich Console or plain print() freely.

  [B] Main thread INSIDE patch_stdout()
      → Use _pt_print() ONLY.
        It routes through the proxy: erase prompt → write → redraw prompt.

  [C] Background thread INSIDE patch_stdout()
      → Use sys.stdout.write() via the StdoutProxy installed by patch_stdout.
        StdoutProxy only flushes on \\n.  No \\r, no ANSI cursor movement.
        Every line MUST end with \\n.

All functions here that say "context [B]" must only be called from the
main/event thread.  Functions that say "context [C]" must end every
write with \\n and never use \\r.
"""

from __future__ import annotations

import re
import sys

from prompt_toolkit import print_formatted_text
from prompt_toolkit.formatted_text import ANSI

from . import style as S

# ─────────────────────────────────────────────────────────────────────────────
# ANSI / control-char sanitiser (strips escape sequences from LLM output)
# ─────────────────────────────────────────────────────────────────────────────

_ANSI_CSI_RE      = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
_ANSI_OSC_RE      = re.compile(r"\x1B\][^\x07\x1B]*(?:\x07|\x1B\\)")
_CONTROL_CHARS_RE = re.compile(r"[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]")


def sanitize(text: object) -> str:
    """Strip ANSI escape codes and bare control chars from arbitrary text."""
    if text is None:
        return ""
    s = str(text).replace("\r\n", "\n").replace("\r", "\n")
    s = _ANSI_CSI_RE.sub("", s)
    s = _ANSI_OSC_RE.sub("", s)
    return _CONTROL_CHARS_RE.sub("", s)


# ─────────────────────────────────────────────────────────────────────────────
# Context [B] printer — main thread inside patch_stdout()
# ─────────────────────────────────────────────────────────────────────────────

def _pt_print(text: str, end: str = "\n") -> None:
    """Context [B]: route ANSI text through prompt_toolkit's renderer."""
    print_formatted_text(ANSI(text + end), end="")


def _cprint(text: str, end: str = "\n") -> None:
    """Alias for _pt_print.  Use anywhere inside the live TUI (context [B])."""
    _pt_print(text, end=end)


# ─────────────────────────────────────────────────────────────────────────────
# Context bar (printed after each LLM turn)
# ─────────────────────────────────────────────────────────────────────────────

def format_context_bar(current: int, maximum: int, pct: float, width: int = 20) -> str:
    """
    Build a single-line context-usage indicator.

    Returns an ANSI string suitable for _pt_print in context [B].
    """
    if maximum <= 0:
        return ""

    filled   = int(pct / 5)            # 20-char bar @ 5% per block
    bar      = "█" * filled + "░" * (width - filled)

    # Colour tiers
    if pct >= 90:
        colour = S.BOLD_RED
    elif pct >= 75:
        colour = S.BOLD_YELLOW
    elif pct >= 50:
        colour = S.YELLOW
    else:
        colour = S.DIM_CYAN

    warn = f"  {S.BOLD_YELLOW}⚠ context almost full{S.RST}" if pct >= 80 else ""
    cur_k  = f"{current / 1000:.1f}k" if current >= 1000 else str(current)
    max_k  = f"{maximum / 1000:.1f}k" if maximum >= 1000 else str(maximum)

    return (
        f"  {colour}[{bar}]{S.RST}"
        f"  {S.DIM}{cur_k}/{max_k}{S.RST}"
        f"  {colour}{int(pct):3d}%{S.RST}"
        f"{warn}"
    )


# ─────────────────────────────────────────────────────────────────────────────
# Turn separator helpers
# ─────────────────────────────────────────────────────────────────────────────

def _hr(colour: str = S.BRONZE, char: str = "─") -> str:
    """Return a full-width horizontal rule as an ANSI string."""
    import shutil
    w = shutil.get_terminal_size().columns
    return f"{colour}{char * w}{S.RST}"


def print_separator(colour: str = S.BRONZE) -> None:
    """Context [B]: print a full-width bronze rule."""
    _pt_print(_hr(colour))


def print_response_header() -> None:
    """Context [B]: print the 'Assistant:' label before streaming tokens."""
    _pt_print(f"\n{S.BOLD_CYAN}Assistant:{S.RST} ", end="")


def print_turn_end() -> None:
    """Context [B]: newline after a completed streaming turn."""
    _pt_print("")


# ─────────────────────────────────────────────────────────────────────────────
# Tool-event lines (context [B])
# ─────────────────────────────────────────────────────────────────────────────

def print_tool_start(tool: str, preview: str = "", mode: str = "full") -> None:
    """Print the '⚙ tool starting…' line.  mode: full | summary | silent."""
    if mode == "silent":
        return
    emoji = S.get_tool_emoji(tool)
    if mode == "summary":
        _pt_print(f"  {S.DIM_CYAN}┊{S.RST} {emoji} {S.WHITE}{tool}{S.RST} {S.DIM}started{S.RST}")
    else:
        detail = f"  {S.DIM}{preview}{S.RST}" if preview else ""
        _pt_print(f"  {S.DIM_CYAN}┊{S.RST} {emoji} {S.WHITE}{tool}{S.RST}{detail} {S.DIM}starting…{S.RST}")


def print_tool_finish(tool: str, duration: float = 0.0,
                      failed: bool = False, mode: str = "full") -> None:
    """Print the '✓ / ✗ tool finished' line."""
    if mode == "silent":
        return
    emoji = S.get_tool_emoji(tool)
    if failed:
        status = f"{S.BOLD_RED}✗ failed{S.RST}"
    else:
        status = f"{S.GREEN}✓{S.RST}"
    dur = f"{duration:.1f}s" if duration > 0 else ""
    _pt_print(f"  {S.DIM_CYAN}┊{S.RST} {emoji} {S.WHITE}{tool:<14}{S.RST} {status}  {S.DIM}{dur}{S.RST}")