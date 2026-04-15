"""
terminal_gateway.py — Velix TerminalGateway

The interactive TUI.  Inherits from Gateway and wires up:
  - prompt_toolkit PromptSession for input (with history, completion)
  - KawaiiSpinner for tool feedback
  - Streaming token display with flush reconciliation
  - Context-usage bar after every turn
  - Approval request flow
  - Full slash-command table (both client-side and forwarded)
  - In-band session switching (switch_session frame)
  - Tool-output display gated by tool_output_mode
  - Thread-safe print locking

OUTPUT CONTEXTS (see display.py for full explanation)
  [A] before patch_stdout → banner, session-picker, initial print()
  [B] main thread inside patch_stdout → _pt_print / _cprint
  [C] bg thread inside patch_stdout → sys.stdout.write() ending with \\n

All event-dispatch from _recv_loop runs in a daemon bg-thread → context [C].
All prompt_toolkit callbacks (deliver, on_*) are called from that bg-thread
BUT we route them through prompt_toolkit's run_in_terminal / print_formatted_text
which is thread-safe.  In practice _pt_print() is safe from any thread because
prompt_toolkit's ANSI printer itself synchronises.
"""

from __future__ import annotations

import os
import queue
import re
import shutil
import sys
import threading
import time
from pathlib import Path
from typing import Optional

from prompt_toolkit import PromptSession
from prompt_toolkit.formatted_text import ANSI
from prompt_toolkit.history import FileHistory
from prompt_toolkit.patch_stdout import patch_stdout

from gateway import Gateway  # type: ignore
from .display import (
    _pt_print,
    _cprint,
    sanitize,
    format_context_bar,
    print_separator,
)
from . import style as S
from .spinner import KawaiiSpinner

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

_HISTORY_FILE = Path.home() / ".velix_history"
_VALID_UI_APPROVE_MODES = {"all", "default"}


def _fmt_tokens(n: int) -> str:
    return f"{n / 1000:.1f}k" if n >= 1000 else str(n)


# ─────────────────────────────────────────────────────────────────────────────
# TerminalGateway
# ─────────────────────────────────────────────────────────────────────────────


class TerminalGateway(Gateway):
    """
    Full-featured interactive terminal for Velix.

    Construct, then call ``connect()`` followed by ``run()``.
    """

    def __init__(
        self,
        handler_host: str = "127.0.0.1",
        handler_port: int = 6060,
        user_id: Optional[str] = None,
        tool_output_mode: str = "full",  # full | summary | silent
        stream_enabled: bool = True,
        user_store: Path = Path.home() / ".velix_user",
    ) -> None:
        super().__init__(
            handler_host=handler_host,
            handler_port=handler_port,
            user_id=user_id,
        )
        self._tool_output_mode = tool_output_mode
        self._stream_enabled = stream_enabled
        self._user_store = user_store

        # Print state (protected by _print_lock)
        self._print_lock = threading.Lock()
        self._streaming = False  # True while tokens arriving
        self._streamed_buf = ""  # accumulated streamed text

        # Context window usage (updated by on_context_usage)
        self._ctx_current = 0
        self._ctx_max = 0
        self._ctx_session = 0
        self._ctx_system = 0
        self._ctx_tool_schema = 0
        self._ctx_request = 0

        # Spinner for tool calls
        self._spinner = KawaiiSpinner()
        self._tool_start_time = 0.0
        self._current_tool = ""

        # Session state
        self._prompt_session = PromptSession(history=FileHistory(str(_HISTORY_FILE)))
        # Gate input so approval prompts don't race with main prompt session.
        self._turn_done = threading.Event()
        self._turn_done.set()
        self._approval_queue = queue.Queue()
        self._approve_mode = "default"  # all | default (runtime only)

        # Register extra commands
        self._register_terminal_commands()

        # Valid slash commands handled by server-side command parser.
        # Unknown slash commands should be blocked locally.
        self._forwarded_commands = {
            "/new",
            "/compact",
            "/undo",
            "/sessions",
            "/delete",
            "/destroy_user",
            "/title",
            "/session_info",
            "/model_info",
            "/scheduler_info",
            "/context",
            "/terminals",
        }

    # ═══════════════════════════════════════════════════════════════════════
    # Gateway abstract interface
    # ═══════════════════════════════════════════════════════════════════════

    def get_next_input(self) -> Optional[str]:
        """Block until the user presses Enter.  Returns None on EOF/Ctrl-D."""
        ctx = self._fmt_ctx_meter()
        sid = self.user_id or "?"
        prompt_str = f"{S.BOLD_CYAN}[{sid}{ctx}]{S.RST} {S.GOLD}❯{S.RST} "
        try:
            line = self._prompt_session.prompt(ANSI(prompt_str)).strip()
        except (EOFError, KeyboardInterrupt):
            return None
        if line.lower() in ("/exit", "/quit", "/bye", "exit", "quit"):
            return None
        return line

    @staticmethod
    def _extract_super_user(identity: str) -> str:
        """Return super-user from either super_user or super_user_sN."""
        m = re.fullmatch(r"(.+)_s(\d+)", identity)
        return m.group(1) if m else identity

    def _current_super_user(self) -> str:
        """Prefer handler-provided super_user; fallback to parsed session_id."""
        if self.super_user:
            return self._extract_super_user(self.super_user)
        if self.user_id:
            return self._extract_super_user(self.user_id)
        return ""

    def deliver(self, event: dict) -> None:
        """
        Called from the bg recv-thread (context [C] effectively, but
        _pt_print is thread-safe via prompt_toolkit internals).
        Route to the appropriate hook; fall through to a dim status line.
        """
        t = event.get("type", "")

        if t == "token":
            self._handle_token(event.get("data", ""))

        elif t == "end":
            self._handle_end()

        elif t == "tool_start":
            self._handle_tool_start(event)

        elif t == "tool_finish":
            self._handle_tool_finish(event)

        elif t == "context_usage":
            self.on_context_usage(
                int(event.get("current_tokens", 0) or 0),
                int(event.get("max_tokens", 0) or 0),
                float(event.get("pct", 0.0) or 0.0),
                session_tokens=int(event.get("session_tokens", 0) or 0),
                system_prompt_tokens=int(event.get("system_prompt_tokens", 0) or 0),
                tool_schema_tokens=int(event.get("tool_schema_tokens", 0) or 0),
                request_tokens=int(event.get("request_tokens", 0) or 0),
                total_context_tokens=int(event.get("total_context_tokens", 0) or 0),
            )
            # Don't print the bar here; it's printed in _handle_end instead.

        elif t == "approval_request":
            payload = event.get("payload", {})
            trace = event.get("approval_trace", "")
            if not trace and isinstance(payload, dict):
                trace = str(payload.get("approval_trace", ""))
            self.on_approval_request(
                trace,
                payload,
            )

        elif t == "approval_ack":
            trace = event.get("approval_trace", "")
            _pt_print(
                f"\n{S.DIM_CYAN}  ✓ Approval acknowledged (trace {trace[:8]}…){S.RST}"
            )
            # DO NOT set _turn_done here; multiple tool calls may follow.
            # Only 'end' or 'error' frames should conclude the turn.

        elif t == "session_switched":
            sid = event.get("session_id", "")
            if sid:
                self._user_id = sid
            _pt_print(f"\n{S.DIM_CYAN}  ↻ Session → {sid}{S.RST}")

        elif t == "error":
            self._ensure_newline()
            _pt_print(
                f"\n{S.BOLD_RED}[Error]{S.RST} {sanitize(event.get('message', ''))}"
            )

        elif t in ("notify", "independent_msg"):
            self.on_notify(event)

        else:
            # Unknown frame — show dim status
            _pt_print(f"{S.DIM}  [{t}]{S.RST}")

    # ═══════════════════════════════════════════════════════════════════════
    # Hook overrides
    # ═══════════════════════════════════════════════════════════════════════

    def on_connected(self) -> None:
        """Called after registration ack — context [A] (still before patch_stdout)."""
        from rich.console import Console
        from rich.panel import Panel
        from rich.table import Table
        from datetime import datetime

        c = Console()
        tbl = Table.grid(padding=(0, 2))
        tbl.add_column("k", style="bold cyan")
        tbl.add_column("v", style="white")
        tbl.add_row("Session", self.user_id or "?")
        tbl.add_row("User", self.super_user or "?")
        tbl.add_row("Handler", f"{self._handler_host}:{self._handler_port}")
        tbl.add_row("Time", datetime.now().strftime("%H:%M:%S"))
        c.print(
            Panel(
                tbl,
                title="[bold white]Connected[/]",
                border_style="cyan",
                expand=False,
                padding=(1, 2),
            )
        )
        c.print()
        c.print(
            f"[dim cyan]Type a message and press Enter.  "
            f"/help for commands.  Ctrl-D to quit.[/]"
        )
        c.print()

    def on_disconnected(self) -> None:
        if self._spinner.is_running():
            self._spinner.stop(self._current_tool, failed=True)
        _pt_print(f"\n{S.BOLD_RED}Disconnected.{S.RST}")

    def on_token(self, text: str) -> None:
        self._handle_token(text)

    def on_end(self) -> None:
        self._handle_end()

    def on_tool_start(self, tool: str, args) -> None:
        if self._tool_output_mode == "silent":
            return
        self._current_tool = tool
        self._tool_start_time = time.time()
        preview = self._tool_preview(tool, args if isinstance(args, dict) else {})
        self._spinner.start(tool, preview)

    def on_tool_finish(self, tool: str, result) -> None:
        if self._tool_output_mode == "silent":
            return
        dur = time.time() - self._tool_start_time if self._tool_start_time > 0 else 0.0
        failed = False
        if isinstance(result, dict) and (
            result.get("error") or result.get("status") == "error"
        ):
            failed = True
        self._spinner.stop(tool, dur, failed=failed)
        if isinstance(result, dict):
            note = result.get("approval_note", "")
            if isinstance(note, str) and note.strip():
                _pt_print(f"  {S.DIM_CYAN}↳ approval: {sanitize(note)}{S.RST}")
        self._current_tool = ""
        self._tool_start_time = 0.0

    def on_context_usage(
        self,
        current: int,
        maximum: int,
        pct: float,
        *,
        session_tokens: int = 0,
        system_prompt_tokens: int = 0,
        tool_schema_tokens: int = 0,
        request_tokens: int = 0,
        total_context_tokens: int = 0,
    ) -> None:
        _ = pct  # kept for signature parity with base gateway hook
        self._ctx_current = max(
            0, total_context_tokens if total_context_tokens > 0 else current
        )
        self._ctx_max = max(0, maximum)
        self._ctx_session = max(0, session_tokens)
        self._ctx_system = max(0, system_prompt_tokens)
        self._ctx_tool_schema = max(0, tool_schema_tokens)
        self._ctx_request = max(0, request_tokens)

    def on_approval_request(self, approval_trace: str, payload: dict) -> None:
        """
        Non-blocking hook called from bg receiver thread.
        Enqueues the request for the main thread to handle interactively.
        """
        self._approval_queue.put((approval_trace, payload))

    def _handle_approval_interactively(
        self, approval_trace: str, payload: dict
    ) -> None:
        """
        Interactive approval prompt in main thread (Context [B]).
        Allows for user input while background streams continue (via patch_stdout).
        """
        self._ensure_newline()
        if self._spinner.is_running():
            dur = (
                time.time() - self._tool_start_time
                if self._tool_start_time > 0
                else 0.0
            )
            self._spinner.stop(self._current_tool, dur, failed=False)
            self._current_tool = ""
            self._tool_start_time = 0.0

        cmd = payload.get("command", payload.get("message", ""))
        desc = payload.get("description", "")
        _pt_print(f"\n{S.YELLOW}{'─' * 52}{S.RST}")
        _pt_print(f"  {S.BOLD_YELLOW}⚠  Action Required{S.RST}")
        _pt_print(f"{S.YELLOW}{'─' * 52}{S.RST}")
        if cmd:
            _pt_print(f"  Command : {S.WHITE}{sanitize(cmd)}{S.RST}")
        if desc:
            _pt_print(f"  Reason  : {sanitize(desc)}")
        if approval_trace:
            _pt_print(f"  Trace ID: {S.CYAN}{sanitize(approval_trace)}{S.RST}")
        _pt_print(f"{S.YELLOW}{'─' * 52}{S.RST}")

        if self._approve_mode == "all":
            _pt_print(f"  Mode    : {S.CYAN}all{S.RST} (auto-approved)")
            self.send_approval_reply(approval_trace, "once")
            return

        _pt_print("  Reply:  allow / deny / whitelist")
        ans = ""
        while ans not in ("allow", "deny", "whitelist"):
            try:
                # Use project's existing PromptSession for clean TUI state
                ans = self._prompt_session.prompt(ANSI("  > ")).strip().lower()
            except (EOFError, KeyboardInterrupt):
                ans = "deny"
                break
            if ans not in ("allow", "deny", "whitelist"):
                _pt_print("  Please type exactly: allow / deny / whitelist")
        scope = {
            "allow": "once",
            "deny": "deny",
            "whitelist": "always",
        }.get(ans, "deny")
        self.send_approval_reply(approval_trace, scope)

    def _wait_for_turn_completion(self) -> None:
        """
        Main thread polling loop: waits for self._turn_done while
        processing interactive signals (like approvals) from the receiver.
        Ensures the queue is drained even if the turn ends abruptly.
        """
        while not self._turn_done.is_set() or not self._approval_queue.empty():
            try:
                # Use non-blocking polling pattern for cleaner loop
                req = self._approval_queue.get_nowait()
                trace, payload = req
                self._handle_approval_interactively(trace, payload)
            except queue.Empty:
                time.sleep(0.05)
                continue
            except Exception as exc:
                _pt_print(
                    f"{S.BOLD_RED}[Internal Error]{S.RST} signal handling failed: {exc}"
                )
                break

    def on_session_switched(self, session_id: str) -> None:
        self._user_id = session_id
        self._approve_mode = "default"
        _pt_print(f"\n{S.DIM_CYAN}  ↻ Session → {session_id}{S.RST}")

    def on_notify(self, event: dict) -> None:
        """Generic bus notification — show a dim status line."""
        purpose = event.get("purpose", event.get("type", "notify"))
        ntype = event.get("notify_type", "")
        payload = event.get("payload", {})

        if ntype == "USER_DESTROYED":
            try:
                if self._user_store.exists():
                    self._user_store.unlink()
            except OSError:
                pass
            su = sanitize(str(payload.get("super_user", self._current_super_user())))
            _pt_print(
                f"\n{S.BOLD_YELLOW}[Session Ended]{S.RST} user '{su}' was destroyed; local saved user cleared."
            )
            return

        if purpose == "SYSTEM_EVENT" or ntype == "SYSTEM_EVENT":
            _pt_print(f"{S.DIM_CYAN}  ⚙  {sanitize(str(payload))}{S.RST}")
        elif ntype == "INDEPENDENT" or purpose == "independent_msg":
            content = sanitize(payload.get("content", str(payload)))
            _pt_print(f"\n{S.BOLD_BLUE}  📩 {content}{S.RST}")
        # Other bus events are silently dropped in the terminal.

    def on_error(self, message: str) -> None:
        self._ensure_newline()
        if self._spinner.is_running():
            dur = (
                time.time() - self._tool_start_time
                if self._tool_start_time > 0
                else 0.0
            )
            self._spinner.stop(self._current_tool, dur, failed=True)
            self._current_tool = ""
            self._tool_start_time = 0.0

        _pt_print(f"\n{S.BOLD_RED}[Error]{S.RST} {sanitize(message)}")
        self._turn_done.set()

    # ═══════════════════════════════════════════════════════════════════════
    # run() — main loop
    # ═══════════════════════════════════════════════════════════════════════

    def run(self) -> None:
        """
        Main input loop, wrapped in patch_stdout so background events
        print cleanly above the prompt.
        """
        with patch_stdout():
            while self._running:
                try:
                    text = self.get_next_input()
                    if text is None:
                        break
                    if not text:
                        continue

                    # Prevent stale approvals from previous turns from surfacing
                    while not self._approval_queue.empty():
                        try:
                            self._approval_queue.get_nowait()
                        except queue.Empty:
                            break

                    # Client-side slash commands first
                    if text.startswith("/"):
                        cmd, _, args = text.partition(" ")
                        fn = self._commands.get(cmd)
                        if fn is not None:
                            try:
                                fn(args.strip())
                            except Exception as exc:
                                _pt_print(f"{S.BOLD_RED}[Command error]{S.RST} {exc}")
                            continue
                        if cmd in self._forwarded_commands:
                            self._turn_done.clear()
                            self.send_message(text)
                            self._wait_for_turn_completion()
                            continue
                        _pt_print(
                            f"{S.BOLD_RED}[Invalid command]{S.RST} "
                            f"{sanitize(cmd)} is not a valid command. "
                            f"Use /help to see available commands."
                        )
                        continue

                    self._turn_done.clear()
                    self.send_message(text)
                    self._wait_for_turn_completion()

                except (KeyboardInterrupt, EOFError):
                    break
                except Exception as exc:
                    _pt_print(f"{S.BOLD_RED}[Input error]{S.RST} {exc}")
                    break

        self.disconnect()

    # ═══════════════════════════════════════════════════════════════════════
    # Internal token / end handling
    # ═══════════════════════════════════════════════════════════════════════

    def _handle_token(self, text: str) -> None:
        """Called for each streaming token (context [B/C])."""
        if self._spinner.is_running():
            # Spinner still going — stop it before first token arrives
            self._spinner.stop(self._current_tool, 0.0, failed=False)
        if not self._streaming:
            _pt_print(f"\n{S.BOLD_CYAN}Assistant:{S.RST} ", end="")
            self._streaming = True
            self._streamed_buf = ""
        clean = sanitize(text)
        self._streamed_buf += clean
        _pt_print(clean, end="")

    def _handle_end(self) -> None:
        """Called at the end of a full LLM turn."""
        with self._print_lock:
            if self._streaming:
                _pt_print("")  # newline after stream
                self._streaming = False
                self._streamed_buf = ""

        # Ensure any orphan spinner is joined before turn concludes
        if self._spinner.is_running():
            dur = (
                time.time() - self._tool_start_time
                if self._tool_start_time > 0
                else 0.0
            )
            self._spinner.stop(self._current_tool, dur, failed=False)
            self._current_tool = ""
            self._tool_start_time = 0.0

        # Context bar
        if self._ctx_max > 0:
            pct = min(self._ctx_current / self._ctx_max * 100.0, 100.0)
            bar = format_context_bar(self._ctx_current, self._ctx_max, pct)
            if bar:
                _pt_print(bar)
        self._turn_done.set()

    def _ensure_newline(self) -> None:
        """If we're mid-stream, print a newline first."""
        with self._print_lock:
            if self._streaming:
                _pt_print("")
                self._streaming = False

    def _flush_reply(self, reply: str) -> None:
        """
        Reconcile a non-streamed final reply against what was already
        streamed token-by-token.
        """
        if not reply:
            return
        if not self._streaming and not self._streamed_buf:
            # Nothing streamed — print the full reply
            _pt_print(f"\n{S.BOLD_CYAN}Assistant:{S.RST} {sanitize(reply)}")
            return
        # Already streamed the whole thing
        if reply == self._streamed_buf:
            return
        # Partial stream — print the tail
        if self._streamed_buf and reply.startswith(self._streamed_buf):
            tail = sanitize(reply[len(self._streamed_buf) :])
            _pt_print(tail, end="")
            return
        # Mismatch — print on new line
        _pt_print(f"\n{sanitize(reply)}")

    # ═══════════════════════════════════════════════════════════════════════
    # Tool preview helper
    # ═══════════════════════════════════════════════════════════════════════

    def _tool_preview(self, tool: str, args: dict, max_len: int = 60) -> str:
        PRIMARY = {
            "terminal": "command",
            "web_search": "query",
            "read_file": "path",
            "write_file": "path",
            "patch": "path",
            "browser_navigate": "url",
            "vision_analyze": "question",
        }
        key = PRIMARY.get(tool)
        if not key:
            for fallback in ("query", "text", "command", "path", "name", "prompt"):
                if fallback in args:
                    key = fallback
                    break
        if not key or key not in args:
            return ""
        val = str(args[key])
        val = " ".join(val.split())  # collapse whitespace
        if len(val) > max_len:
            val = val[: max_len - 3] + "…"
        return val

    # ═══════════════════════════════════════════════════════════════════════
    # Context meter (shown in prompt)
    # ═══════════════════════════════════════════════════════════════════════

    def _fmt_ctx_meter(self) -> str:
        if self._ctx_max <= 0:
            return ""
        return f" {_fmt_tokens(self._ctx_current)}/{_fmt_tokens(self._ctx_max)}"

    # ═══════════════════════════════════════════════════════════════════════
    # Terminal-specific client-side commands
    # ═══════════════════════════════════════════════════════════════════════

    def _register_terminal_commands(self) -> None:
        """Register client-side commands handled locally (not forwarded to handler)."""

        def _users(_args: str) -> None:
            """List all known super-users."""
            try:
                users = Gateway.list_users(self._handler_host, self._handler_port)
            except Exception as exc:
                _pt_print(f"{S.BOLD_RED}[Error]{S.RST} {exc}")
                return
            if not users:
                _pt_print("  (no users)")
                return
            _pt_print(f"\n{S.BOLD_CYAN}Super-users:{S.RST}")
            for u in users:
                tag = f"  {S.YELLOW}[active]{S.RST}" if u.get("active") else ""
                _pt_print(f"  {S.CYAN}{u['id']}{S.RST}{tag}")
            _pt_print("")

        def _list_sessions(args: str) -> None:
            """List sessions: /list_sessions [super_user]"""
            su = args.strip() or self._current_super_user()
            su = self._extract_super_user(su)
            if not su:
                _pt_print("  Usage: /list_sessions <super_user>")
                return
            try:
                sessions = Gateway.list_sessions(
                    su, self._handler_host, self._handler_port
                )
            except Exception as exc:
                _pt_print(f"{S.BOLD_RED}[Error]{S.RST} {exc}")
                return
            if not sessions:
                _pt_print(f"  (no sessions for {su})")
                return
            _pt_print(f"\n{S.BOLD_CYAN}Sessions for {su}:{S.RST}")
            for s in sessions:
                tag = f"  {S.YELLOW}[active]{S.RST}" if s.get("active") else ""
                title = f'  "{s["title"]}"' if s.get("title") else ""
                turns = f"  turns={s.get('turns', 0)}"
                _pt_print(
                    f"  {S.CYAN}{s['id']}{S.RST}"
                    f"{S.DIM}{title}{S.RST}"
                    f"  {S.DIM}{turns}{S.RST}"
                    f"{tag}"
                )
            _pt_print("")

        def _switch(args: str) -> None:
            """Switch to a different session: /switch <session_id>"""
            sid = args.strip()
            if not sid:
                _pt_print("  Usage: /switch <session_id>")
                return
            # Validate ownership
            su = self._current_super_user()
            if su and not sid.startswith(su + "_s"):
                _pt_print(
                    f"  {S.YELLOW}Warning:{S.RST} session '{sid}' "
                    f"may not belong to user '{su}'"
                )
            self.switch_session(sid)

        def _help(_args: str) -> None:
            """Show available commands."""
            lines = [
                "",
                f"{S.BOLD_CYAN}Client-side commands (handled locally):{S.RST}",
                f"  {S.CYAN}/help{S.RST}                    — this message",
                f"  {S.CYAN}/users{S.RST}                   — list all super-users",
                f"  {S.CYAN}/list_sessions [user]{S.RST}    — list sessions",
                f"  {S.CYAN}/switch <session_id>{S.RST}     — switch active session",
                f"  {S.CYAN}/approvemode [all|default]{S.RST} — runtime auto-approval mode",
                "",
                f"{S.BOLD_CYAN}Handler-side commands (sent to server):{S.RST}",
                f"  {S.CYAN}/new{S.RST}             — start a new session",
                f"  {S.CYAN}/compact{S.RST}         — compact current session",
                f"  {S.CYAN}/undo{S.RST}            — undo last turn",
                f"  {S.CYAN}/sessions{S.RST}        — list your sessions (with stats)",
                f"  {S.CYAN}/delete <sid>{S.RST}    — delete a session by id",
                f"  {S.CYAN}/destroy_user{S.RST}    — delete all sessions and user identity",
                f"  {S.CYAN}/title <text>{S.RST}    — set session title",
                f"  {S.CYAN}/session_info{S.RST}    — current session stats",
                f"  {S.CYAN}/model_info{S.RST}      — model & adapter config",
                f"  {S.CYAN}/scheduler_info{S.RST}  — scheduler queue depth",
                f"  {S.CYAN}/context{S.RST}         — context window usage",
                f"  {S.CYAN}/terminals{S.RST}       — list active persistent terminals",
                "",
            ]
            for line in lines:
                _pt_print(line)

        def _approvemode(args: str) -> None:
            wanted = args.strip().lower()
            if not wanted:
                _pt_print(f"  approvemode = {S.CYAN}{self._approve_mode}{S.RST}")
                return
            if wanted not in _VALID_UI_APPROVE_MODES:
                _pt_print(
                    f"  {S.BOLD_RED}Invalid mode{S.RST}: {sanitize(wanted)}. "
                    "Use one of: all | default"
                )
                return
            self._approve_mode = wanted
            _pt_print(f"  approvemode set to {S.CYAN}{wanted}{S.RST} (memory only)")

        self.register_command("/help", _help)
        self.register_command("/users", _users)
        self.register_command("/list_sessions", _list_sessions)
        self.register_command("/switch", _switch)
        self.register_command("/approvemode", _approvemode)
        # /exit and /quit handled by get_next_input()
