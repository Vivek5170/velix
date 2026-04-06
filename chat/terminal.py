#!/usr/bin/env python3
"""
terminal.py — Velix Premium Terminal Gateway

OUTPUT ARCHITECTURE
===================
Three distinct output contexts. Wrong API → garbled output.

  [A] Before patch_stdout()
      → Rich Console freely. Proxy not active yet.

  [B] Main thread inside patch_stdout()
      → _pt_print() ONLY.
        Routes through proxy: erase prompt → write → redraw prompt.

  [C] Background thread inside patch_stdout()
      → sys.stdout.write() via the StdoutProxy that patch_stdout installed.
        The proxy buffers writes until it sees \n, then uses run_in_terminal.
        CRITICAL: \r does NOT work through StdoutProxy — it only flushes on
        \n. So in-place \r animation is impossible from a bg thread.
        The spinner therefore only writes complete lines ending with \n.

SPINNER DESIGN
==============
Because \r-based in-place animation is impossible through StdoutProxy,
the spinner works as a "progress line" model instead:
  - start()   prints one "⠋ [tool] starting..." line
  - Every N ms it prints a new progress line (each ends with \n)
  - stop()    clears by printing the final summary line via _pt_print [B]

This is less pretty than true in-place animation but 100% correct.
"""

import argparse
import json
import os
import re
import shutil
import sys
import threading
import time
import uuid
from datetime import datetime
from typing import Optional

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from prompt_toolkit import PromptSession, print_formatted_text
    from prompt_toolkit.patch_stdout import patch_stdout
    from prompt_toolkit.formatted_text import HTML, ANSI
except ImportError:
    print("[Error] Velix Terminal requires 'rich' and 'prompt_toolkit'.")
    print("Please install them: pip install rich prompt_toolkit")
    sys.exit(1)

from gateway import Gateway

# =========================================================================
# Branding
# =========================================================================

VELIX_LOGO = """[bold cyan]
\u2588\u2588\u2557   \u2588\u2588\u2557\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557\u2588\u2588\u2557     \u2588\u2588\u2557\u2588\u2588\u2557  \u2588\u2588\u2557
\u2588\u2588\u2551   \u2588\u2588\u2551\u2588\u2588\u2554\u2550\u2550\u2550\u2550\u255d\u2588\u2588\u2551     \u2588\u2588\u2551\u255a\u2588\u2588\u2557\u2588\u2588\u2554\u255d
\u2588\u2588\u2551   \u2588\u2588\u2551\u2588\u2588\u2588\u2588\u2588\u2557  \u2588\u2588\u2551     \u2588\u2588\u2551 \u255a\u2588\u2588\u2588\u2554\u255d 
\u255a\u2588\u2588\u2557 \u2588\u2588\u2554\u255d\u2588\u2588\u2554\u2550\u2550\u255d  \u2588\u2588\u2551     \u2588\u2588\u2551 \u2588\u2588\u2554\u2588\u2588\u2557 
 \u255a\u2588\u2588\u2588\u2588\u2554\u255d \u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557\u2588\u2588\u2551\u2588\u2588\u2554\u255d \u2588\u2588\u2557
  \u255a\u2550\u2550\u2550\u255d  \u255a\u2550\u2550\u2550\u2550\u2550\u2550\u255d\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u255d\u255a\u2550\u255d\u255a\u2550\u255d  \u255a\u2550\u255d[/]
[dim cyan]The Agentic Operating System[/]"""

# =========================================================================
# Constants
# =========================================================================

CONSOLE = Console()  # context [A] ONLY

A_RESET      = "\033[0m"
A_BOLD_CYAN  = "\033[1;36m"
A_DIM_CYAN   = "\033[2;36m"
A_DIM        = "\033[2m"
A_WHITE      = "\033[0;37m"
A_BOLD_RED   = "\033[1;31m"
A_BOLD_BLUE  = "\033[1;34m"
A_YELLOW     = "\033[0;33m"

KAWAII_FACES = [
    "(｡◕‿◕｡)", "(◕‿◕✿)", "٩(◕‿◕｡)۶", "(✿◠‿◠)", "( ˘▽˘)っ",
    "♪(´ε` )", "(◕ᴗ◕✿)", "ヾ(＾∇＾)", "(≧◡≦)", "(★ω★)",
]

TOOL_EMOJIS = {
    "terminal": "💻", "web_search": "🔍", "read_file": "📖",
    "write_file": "✍️",  "patch": "🔧", "browser_navigate": "🌐",
    "skill_view": "📚",  "image_generate": "🎨", "cronjob": "⏰",
}

_ANSI_CSI_RE      = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
_ANSI_OSC_RE      = re.compile(r"\x1B\][^\x07\x1B]*(?:\x07|\x1B\\)")
_CONTROL_CHARS_RE = re.compile(r"[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]")


def _sanitize(text: object) -> str:
    if text is None:
        return ""
    s = str(text).replace("\r\n", "\n").replace("\r", "\n")
    s = _ANSI_CSI_RE.sub("", s)
    s = _ANSI_OSC_RE.sub("", s)
    return _CONTROL_CHARS_RE.sub("", s)


def _pt_print(text: str, end: str = "\n") -> None:
    """Context [B]: main thread inside patch_stdout()."""
    print_formatted_text(ANSI(text + end), end="")


# =========================================================================
# KawaiiSpinner
# =========================================================================

class KawaiiSpinner:
    """
    Tool execution feedback spinner.

    Because StdoutProxy (installed by patch_stdout) only flushes on \\n and
    does not support \\r-based in-place rewriting, animation works as a
    "heartbeat" model: each tick prints a fresh line (always ending \\n).
    The final summary line is printed by stop() from the main thread via
    _pt_print() [context B].

    The bg thread writes via sys.stdout.write (the StdoutProxy) which is
    safe because StdoutProxy uses run_in_terminal for thread safety.
    """

    # Braille spinner chars — used as a subtle animated prefix
    _CHARS = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]
    # How often (seconds) to emit a heartbeat line from the bg thread
    _TICK  = 3.0

    def __init__(self) -> None:
        self.message     = ""
        self.running     = False
        self.start_time  = 0.0
        self._frame      = 0
        self._thread: Optional[threading.Thread] = None

    def _bg_tick(self) -> None:
        """
        Background thread — writes via sys.stdout (StdoutProxy).
        Each write MUST end with \\n so StdoutProxy flushes it immediately.
        No \\r, no ANSI cursor movement — those don't work through the proxy.
        """
        tick = 0
        while self.running:
            time.sleep(self._TICK)
            if not self.running:
                break
            elapsed = time.time() - self.start_time
            char = self._CHARS[tick % len(self._CHARS)]
            face = KAWAII_FACES[tick % len(KAWAII_FACES)]
            # Must end with \n — StdoutProxy only flushes on newline
            line = (
                f"  {A_BOLD_CYAN}{char}{A_RESET} "
                f"{A_DIM_CYAN}{face}{A_RESET} "
                f"{A_WHITE}{self.message}{A_RESET} "
                f"{A_DIM}({elapsed:.0f}s)...{A_RESET}\n"
            )
            sys.stdout.write(line)
            sys.stdout.flush()
            tick += 1

    def start(self, message: str = "") -> None:
        if self.running:
            return
        self.message    = message
        self.running    = True
        self.start_time = time.time()
        self._frame     = 0
        # Print the initial "starting" line immediately from main thread [B]
        emoji = "⚙️"
        _pt_print(
            f"  {A_DIM_CYAN}┊{A_RESET} {emoji} "
            f"{A_WHITE}{message}{A_RESET} "
            f"{A_DIM}starting...{A_RESET}"
        )
        self._thread = threading.Thread(target=self._bg_tick, daemon=True)
        self._thread.start()

    def stop(self, final_line: str = "") -> None:
        """Always called from main thread — uses _pt_print() [context B]."""
        was_running = self.running
        self.running = False
        if self._thread:
            self._thread.join()
        # Only print final_line from main thread via proxy
        if final_line:
            _pt_print(final_line)


# =========================================================================
# TerminalGateway
# =========================================================================

class TerminalGateway(Gateway):
    def __init__(
        self,
        handler_host: str = "127.0.0.1",
        handler_port: int = 6060,
        user_id: Optional[str] = None,
        user_store_path: Optional[str] = None,
    ) -> None:
        super().__init__(
            handler_host=handler_host,
            handler_port=handler_port,
            user_id=user_id,
        )
        self._user_store_path      = user_store_path
        self._response_in_progress = False
        self._response_lock        = threading.Lock()
        self._response_done        = threading.Event()
        self._response_done.set()

        self.spinner               = KawaiiSpinner()
        self._current_tool: Optional[str] = None
        self._tool_start_time: float      = 0.0
        self._session              = PromptSession()
        self._current_context_tokens: int = 0
        self._max_context_tokens: int = 0

    # ------------------------------------------------------------------
    # Lifecycle — context [A]
    # ------------------------------------------------------------------

    def on_connected(self) -> None:
        if self._user_store_path and self.user_id:
            try:
                os.makedirs(os.path.dirname(self._user_store_path), exist_ok=True)
                with open(self._user_store_path, "w", encoding="utf-8") as f:
                    f.write(self.user_id.strip() + "\n")
            except Exception as exc:
                CONSOLE.print(f"[dim red]Failed to persist user_id: {exc}[/]")
        self._print_welcome_banner()

    def _print_welcome_banner(self) -> None:
        CONSOLE.print()
        if shutil.get_terminal_size().columns >= 80:
            CONSOLE.print(VELIX_LOGO, justify="center")
        tbl = Table.grid(padding=(0, 2))
        tbl.add_column("Key",   style="bold cyan")
        tbl.add_column("Value", style="white")
        tbl.add_row("User Session", self.user_id)
        tbl.add_row("Handler",      f"{self._handler_host}:{self._handler_port}")
        tbl.add_row("Local Time",   datetime.now().strftime("%H:%M:%S"))
        CONSOLE.print(Panel(tbl, title="[bold white]Connection Status[/]",
                            border_style="cyan", expand=False, padding=(1, 2)))
        CONSOLE.print(
            "[dim cyan]Type your message and press Enter. "
            "Commands: [bold]/exit[/bold], [bold]/quit[/bold]. "
            "Shortcuts: [bold]Ctrl-C[/bold], [bold]Ctrl-D[/bold][/]\n"
        )

    def on_disconnected(self) -> None:
        _pt_print(f"\n{A_BOLD_RED}Disconnected from Handler.{A_RESET}")

    # ------------------------------------------------------------------
    # Event dispatcher — context [B], _pt_print() everywhere
    # ------------------------------------------------------------------

    def deliver(self, event: dict) -> None:
        etype = event.get("type", "")

        if etype == "token":
            # Only call stop() on the first token — it joins the bg thread.
            # On every subsequent token the spinner is already stopped so this
            # was redundantly calling thread.join() in a tight streaming loop.
            if self.spinner.running:
                self.spinner.stop()
            # deliver() is invoked solely from the single _recv_loop bg thread,
            # so _response_in_progress is only ever written here — no lock needed.
            if not self._response_in_progress:
                _pt_print(f"\n{A_BOLD_CYAN}Assistant:{A_RESET}", end=" ")
                self._response_in_progress = True
                self._response_done.clear()
            _pt_print(_sanitize(event.get("data", "")), end="")

        elif etype == "end":
            with self._response_lock:
                if self._response_in_progress:
                    _pt_print("")
                    self._response_in_progress = False
            self._response_done.set()

        elif etype == "tool_start":
            tool = event.get("tool", "unknown")
            self._current_tool    = tool
            self._tool_start_time = time.time()
            preview = self._tool_preview(tool, event.get("args", {}))
            self.spinner.start(f"[{tool}] {preview}")

        elif etype == "tool_finish":
            duration = time.time() - self._tool_start_time if self._tool_start_time > 0 else 0.0
            tool     = event.get("tool", "unknown")
            self.spinner.stop(
                self._tool_finish_line(tool, duration, event.get("result", {}))
            )
            self._current_tool = None
            self._tool_start_time = 0.0

        elif etype == "notify":
            self._handle_notify(event)

        elif etype == "approval_ack":
            trace = event.get("approval_trace", "")
            _pt_print(f"{A_DIM_CYAN}Approval granted for trace {trace[:8]}...{A_RESET}")

        elif etype == "error":
            self.spinner.stop()
            _pt_print(f"\n{A_BOLD_RED}Error:{A_RESET} {_sanitize(event.get('message', event))}")

        elif etype == "context_usage":
            current = int(event.get("current_tokens", 0) or 0)
            max_tokens = int(event.get("max_tokens", 0) or 0)
            self._current_context_tokens = max(0, current)
            self._max_context_tokens = max(0, max_tokens)

    # ------------------------------------------------------------------
    # Notify — context [B], NO Rich Console
    # ------------------------------------------------------------------

    def _handle_notify(self, event: dict) -> None:
        ntype   = event.get("notify_type", "")
        payload = event.get("payload", {})
        purpose = event.get("purpose", "")

        if ntype == "TOOL_RESULT":
            self.spinner.stop()
            res_str = json.dumps(payload, indent=2)
            if len(res_str) > 500:
                res_str = res_str[:497] + "..."
            sep = f"{A_DIM_CYAN}{'─' * 52}{A_RESET}"
            _pt_print(f"\n{sep}")
            _pt_print(f"  {A_BOLD_CYAN}Async Tool Result{A_RESET}")
            _pt_print(sep)
            for line in res_str.splitlines():
                _pt_print(f"  {line}")
            _pt_print(f"{sep}\n")

        elif ntype == "INDEPENDENT":
            content = _sanitize(payload.get("content", str(payload)))
            _pt_print(f"\n{A_BOLD_BLUE}📩 Message:{A_RESET} {content}")

        elif ntype == "SYSTEM_EVENT":
            _pt_print(f"{A_DIM_CYAN}⚙️  {payload}{A_RESET}")

        elif purpose == "APPROVAL_REQUEST":
            self.spinner.stop()
            self._handle_approval(payload)

    def _handle_approval(self, payload: dict) -> None:
        command = payload.get("command", "")
        desc    = payload.get("description", "")
        trace   = payload.get("approval_trace", "")
        sep     = f"{A_YELLOW}{'─' * 52}{A_RESET}"
        _pt_print(f"\n{sep}")
        _pt_print(f"  {A_YELLOW}⚠  Action Required{A_RESET}")
        _pt_print(sep)
        _pt_print(f"  Command : {A_WHITE}{command}{A_RESET}")
        _pt_print(f"  Reason  : {desc}")
        _pt_print(sep)
        try:
            answer = input("  Confirm action? [y/N]: ").strip().lower()
        except EOFError:
            answer = "n"
        self.send_approval_reply(trace, "once" if answer in ("y", "yes") else "deny")

    # ------------------------------------------------------------------
    # Tool helpers
    # ------------------------------------------------------------------

    def _tool_preview(self, tool: str, args: dict) -> str:
        if not args:
            return ""
        if tool == "terminal":
            return args.get("command", "")[:60]
        for key in ("path", "query", "url"):
            if key in args:
                return str(args[key])[:60]
        return str(args)[:60]

    def _tool_finish_line(self, tool: str, duration: float, result: dict) -> str:
        emoji  = TOOL_EMOJIS.get(tool, "⚡")
        suffix = ""
        if isinstance(result, dict):
            if result.get("status") == "error" or result.get("error"):
                suffix = f" {A_BOLD_RED}[failed]{A_RESET}"
            elif tool == "terminal":
                ec = result.get("exit_code")
                if ec is not None and ec != 0:
                    suffix = f" {A_BOLD_RED}[exit {ec}]{A_RESET}"
        return (
            f"  {A_DIM_CYAN}┊{A_RESET} {emoji} "
            f"{A_WHITE}{tool:<12}{A_RESET}"
            f"{A_DIM}{duration:.1f}s{A_RESET}"
            f"{suffix}"
        )

    # ------------------------------------------------------------------
    # Input
    # ------------------------------------------------------------------

    def get_next_input(self) -> Optional[str]:
        """Block until response done, THEN show prompt."""
        self._response_done.wait()
        meter = self._format_context_meter()
        try:
            line = self._session.prompt(
                HTML(f"<b><skyblue>You{meter}:</skyblue></b> ")
            ).strip()
        except (EOFError, KeyboardInterrupt):
            return None
        if line.lower() in ("/exit", "/quit", "/bye", "exit"):
            return None
        return line

    def _format_context_meter(self) -> str:
        if self._max_context_tokens <= 0:
            return ""
        def fmt(v: int) -> str:
            if v >= 1000:
                return f"{v / 1000:.1f}k"
            return str(v)
        return f" [{fmt(self._current_context_tokens)}/{fmt(self._max_context_tokens)}]"


# =========================================================================
# Entry Point
# =========================================================================

def main() -> None:
    parser = argparse.ArgumentParser(description="Velix Terminal Gateway")
    parser.add_argument("--host",        default="127.0.0.1")
    parser.add_argument("--port",        type=int, default=6060)
    parser.add_argument("--user-id",     default=None)
    parser.add_argument("--user-store",
        default=os.path.join(os.path.dirname(__file__), ".terminal_user_id"))
    parser.add_argument("--forget-user", action="store_true")
    args = parser.parse_args()

    def load_saved(path: str) -> Optional[str]:
        if os.path.exists(path):
            v = open(path).read().strip()
            return v or None
        return None

    def extract_super_user(value: str) -> str:
        m = re.fullmatch(r"(.+)_s([0-9]+)", value)
        if m:
            return m.group(1)
        return value

    def pick_persona_and_session(host: str, port: int, store: str) -> tuple[Optional[str], bool]:
        try:
            users = Gateway.list_users(host, port)
        except Exception as e:
            CONSOLE.print(f"[dim red]Note: identity listing unavailable: {e}[/]")
            users = []
        saved_raw = load_saved(store)
        saved = extract_super_user(saved_raw) if saved_raw else None
        super_user: Optional[str] = None

        if saved and all(str(u.get("id", "")).strip() != saved for u in users):
            users = users + [{"id": saved, "active": False}]

        CONSOLE.print("\n[bold cyan]Available Super Users:[/]")
        if users:
            for i, u in enumerate(users, 1):
                uid = str(u.get("id", "")).strip()
                if not uid:
                    continue
                status = "[bold yellow]Active[/]" if u.get("active") else "[bold green]Free[/]"
                CONSOLE.print(f"  {i}. {uid:24} {status}")
        else:
            CONSOLE.print("  [dim]No known super users yet.[/]")

        if saved:
            CONSOLE.print(f"  [bold cyan]l[/]. Use last saved user ([bold white]{saved}[/])")
        CONSOLE.print("  [bold cyan]n[/]. Enter new super user")

        choice = input("\nSelect super user (number / l / n): ").strip().lower()

        if choice.isdigit() and users:
            idx = int(choice) - 1
            if 0 <= idx < len(users):
                selected = str(users[idx].get("id", "")).strip()
                if selected:
                    super_user = selected
        elif choice == "l" and saved:
            super_user = saved
        elif choice == "n" or not choice:
            custom = input("Enter super user name: ").strip()
            if custom:
                super_user = custom

        if not super_user:
            CONSOLE.print("[bold red]Error:[/] invalid super user selection.")
            return pick_persona_and_session(host, port, store)

        # Now select session
        try:
            sessions = Gateway.list_sessions(super_user, host, port)
        except Exception:
            sessions = []

        if sessions:
            CONSOLE.print(f"\n[bold cyan]Sessions for {super_user}:[/]")
            for i, s_dict in enumerate(sessions, 1):
                s_id = str(s_dict.get("id", "Unknown"))
                is_active = bool(s_dict.get("active", False))
                status = "[bold yellow](Active)[/]" if is_active else "[bold green](Free)[/]"
                CONSOLE.print(f"  {i}. {s_id} {status}")
            CONSOLE.print("  [bold cyan]n[/]. Create New Session")
            choice = input("\nSelect session (leave blank for latest, 'n' for new): ").strip().lower()
            if choice == "n":
                return super_user, True
            if choice.isdigit():
                idx = int(choice) - 1
                if 0 <= idx < len(sessions):
                    return str(sessions[idx].get("id", "")), False
            latest_dict = sessions[-1]
            return str(latest_dict.get("id", "")), False

        # No sessions exist yet
        CONSOLE.print("\n[dim cyan]No sessions found; creating first session on connect.[/]")
        return super_user, False

    user_id = args.user_id
    force_new = False
    if not user_id:
        user_id, force_new = pick_persona_and_session(args.host, args.port, args.user_store)

    gw = TerminalGateway(
        handler_host=args.host,
        handler_port=args.port,
        user_id=user_id,
        user_store_path=args.user_store,
    )
    try:
        gw.connect(force_new=force_new)
    except Exception as e:
        CONSOLE.print(f"[bold red]Connection Failed:[/] {e}")
        sys.exit(1)

    with patch_stdout():
        gw.run()


if __name__ == "__main__":
    main()