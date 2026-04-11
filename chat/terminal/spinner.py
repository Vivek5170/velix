"""
spinner.py — Velix KawaiiSpinner

SPINNER DESIGN CONSTRAINTS (from patch_stdout architecture)
============================================================
patch_stdout installs a StdoutProxy that buffers writes and only
flushes when it sees \\n.  \\r-based in-place animation is therefore
impossible from a background thread — each frame would land on its
own line and create a wall of garbage.

We use a "heartbeat" model instead:
  - start()     → prints one "starting…" line immediately via _pt_print [B]
  - bg thread   → every TICK_SECONDS prints a new heartbeat line via
                  sys.stdout.write() [C], each ending with \\n
  - stop()      → joins thread, prints final summary line via _pt_print [B]

This is called from the main thread (start/stop) and the bg spinner
thread (heartbeat writes).  _pt_print is NOT safe from bg threads —
sys.stdout.write() IS, because StdoutProxy uses run_in_terminal
internally for thread safety.
"""

from __future__ import annotations

import sys
import threading
import time
from typing import Optional

from . import style as S
from .display import _pt_print


class KawaiiSpinner:
    """
    Tool-execution feedback spinner compatible with patch_stdout.

    Usage::

        spinner = KawaiiSpinner()
        spinner.start("web_search", "looking up cats")
        ...
        spinner.stop("web_search", 1.4, failed=False)
    """

    TICK_SECONDS: float = 3.0   # heartbeat interval

    def __init__(self) -> None:
        self._running   = False
        self._tool      = ""
        self._preview   = ""
        self._started   = 0.0
        self._frame_idx = 0
        self._thread: Optional[threading.Thread] = None
        self._current_stop_event: Optional[threading.Event] = None

    # ── Public interface ────────────────────────────────────────────────────

    def start(self, tool: str, preview: str = "") -> None:
        """Context [B]: start the spinner for a tool call."""
        if self._running:
            return
        self._tool    = tool
        self._preview = preview
        self._started = time.time()
        self._running = True
        self._frame_idx = 0
        
        # Unique event for THIS specific thread to avoid resuscitation races
        stop_event = threading.Event()
        self._current_stop_event = stop_event

        # Immediate "starting" line — context [B]
        emoji = S.get_tool_emoji(tool)
        det   = f"  {S.DIM}{preview[:60]}{S.RST}" if preview else ""
        _pt_print(
            f"  {S.DIM_CYAN}┊{S.RST} {emoji} "
            f"{S.WHITE}{tool}{S.RST}{det} "
            f"{S.DIM}starting…{S.RST}"
        )

        self._thread = threading.Thread(
            target=self._bg_loop, 
            args=(stop_event,),
            daemon=True, 
            name="velix-spinner"
        )
        self._thread.start()

    def stop(self, tool: str = "", duration: float = 0.0,
             failed: bool = False) -> None:
        """Context [B]: stop the spinner and print the final summary line."""
        if not self._running:
            return  # Prevent duplicate lines if already stopped (e.g. by approval)

        self._running = False
        if self._current_stop_event:
            self._current_stop_event.set()
            
        if self._thread:
            self._thread.join(timeout=1.0)
            self._thread = None
        
        self._current_stop_event = None

        tool = tool or self._tool
        emoji = S.get_tool_emoji(tool)
        if failed:
            status = f"{S.BOLD_RED}✗ failed{S.RST}"
        else:
            status = f"{S.GREEN}✓{S.RST}"
        dur = f"  {S.DIM}{duration:.1f}s{S.RST}" if duration > 0 else ""

        _pt_print(
            f"  {S.DIM_CYAN}┊{S.RST} {emoji} "
            f"{S.WHITE}{tool:<14}{S.RST} {status}{dur}"
        )

    def is_running(self) -> bool:
        return self._running

    # ── Background heartbeat ────────────────────────────────────────────────

    def _bg_loop(self, stop_event: threading.Event) -> None:
        """
        Context [C]: background thread.  Every write MUST end with \n.
        NO \r, NO ANSI cursor movement — StdoutProxy won't handle them.
        """
        tick = 0
        while not stop_event.is_set():
            # Wait for TICK_SECONDS or until interrupted by stop()
            if stop_event.wait(self.TICK_SECONDS):
                break

            elapsed = time.time() - self._started
            char    = S.BRAILLE_FRAMES[tick % len(S.BRAILLE_FRAMES)]
            face    = S.KAWAII_SPINNER_FACES[tick % len(S.KAWAII_SPINNER_FACES)]
            emoji   = S.get_tool_emoji(self._tool)

            # Every write MUST end with \n
            line = (
                f"  {char} {face} {emoji} {self._tool} ({elapsed:.0f}s)…\n"
            )
            sys.stdout.write(line)
            sys.stdout.flush()
            tick += 1