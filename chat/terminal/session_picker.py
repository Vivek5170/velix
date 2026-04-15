"""
session_picker.py — Pre-connect interactive session picker.

Runs entirely in context [A] — before patch_stdout() is ever active.
Uses plain print() and Rich Console freely.
Communicates with the Handler via Gateway static helpers.

Flow
────
1. List existing super-users (from handler / disk).
2. Show them; let user pick one or create a new one.
3. List sessions for chosen super-user.
4. Let user pick a session, start a new one, or force a fresh session.
5. Return (user_id_or_session_id, force_new: bool).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Optional

from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from . import style as S

CONSOLE = Console()

# ─────────────────────────────────────────────────────────────────────────────
# Saved-user persistence (~/.velix_user)
# ─────────────────────────────────────────────────────────────────────────────

_DEFAULT_STORE = Path.home() / ".velix_user"


def _save_user(user_id: str, path: Path = _DEFAULT_STORE) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(user_id.strip() + "\n", encoding="utf-8")
    except OSError:
        pass


def _load_saved_user(path: Path = _DEFAULT_STORE) -> Optional[str]:
    try:
        v = path.read_text(encoding="utf-8").strip()
        return v or None
    except OSError:
        return None


def _extract_super_user(value: str) -> str:
    """Strip _sN suffix if present: user1_s3 -> user1."""
    m = re.fullmatch(r"(.+)_s(\d+)", value)
    return m.group(1) if m else value


def _is_session_id(value: str) -> bool:
    return bool(re.fullmatch(r".+_s\d+", value))


def _is_valid_user_id(uid: str) -> bool:
    if not uid or len(uid) > 128:
        return False
    return bool(re.fullmatch(r"[a-zA-Z0-9_\-]+", uid))


# ─────────────────────────────────────────────────────────────────────────────
# Banner (context [A])
# ─────────────────────────────────────────────────────────────────────────────


def print_banner(host: str, port: int) -> None:
    """Print the Velix welcome banner in context [A]."""
    CONSOLE.print()

    term_w = CONSOLE.width or 80
    if term_w >= 50:
        for line in S.VELIX_LOGO_TEXT.splitlines():
            CONSOLE.print(line, style="bold cyan")
    else:
        CONSOLE.print("⬡ VELIX  Agentic OS", style="bold cyan")

    CONSOLE.print(S.VELIX_TAGLINE_TEXT, style="dim cyan")
    CONSOLE.print()

    info = Table.grid(padding=(0, 2))
    info.add_column(style="bold cyan")
    info.add_column(style="dim")
    info.add_row("Handler", f"{host}:{port}")
    CONSOLE.print(Panel(info, border_style="cyan", padding=(0, 2)))
    CONSOLE.print()


# ─────────────────────────────────────────────────────────────────────────────
# Super-user picker
# ─────────────────────────────────────────────────────────────────────────────


def _pick_super_user(
    users: list[dict],  # [{"id": str, "active": bool}]
    saved: Optional[str],
) -> str:
    """
    Interactive super-user selection.
    Returns the chosen super-user name (never a session id).
    """
    CONSOLE.print("[bold cyan]── Select User ──────────────────────────────[/]")

    if not users and not saved:
        CONSOLE.print("  [dim]No existing users found.[/]")
    else:
        for i, u in enumerate(users, 1):
            uid = u.get("id", "")
            active = bool(u.get("active"))
            tag = "  [bold yellow][active][/]" if active else ""
            CONSOLE.print(f"  [bold]{i}.[/]  [cyan]{uid}[/]{tag}")

    if saved:
        CONSOLE.print(f"\n  [bold cyan]l[/]  Use last: [bold white]{saved}[/]")
    CONSOLE.print("  [bold cyan]n[/]  Create new user")
    CONSOLE.print()

    while True:
        try:
            raw = input("Select (number / l / n): ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            sys.exit(0)

        if raw == "l" and saved:
            return _extract_super_user(saved)

        if raw == "n" or raw == "":
            while True:
                try:
                    name = input("New super-user name: ").strip()
                except (EOFError, KeyboardInterrupt):
                    sys.exit(0)
                if _is_valid_user_id(name):
                    return name
                CONSOLE.print("  [red]Invalid name.[/] Use letters, digits, _ or -.")

        if raw.isdigit():
            idx = int(raw) - 1
            if 0 <= idx < len(users):
                return str(users[idx].get("id", "")).strip()

        CONSOLE.print("  [red]Invalid choice.[/]")


# ─────────────────────────────────────────────────────────────────────────────
# Session picker
# ─────────────────────────────────────────────────────────────────────────────


def _pick_session(
    super_user: str,
    sessions: list[dict],  # [{"id", "title", "turns", "active"}]
) -> tuple[str, bool]:
    """
    Interactive session selection for a known super-user.
    Returns (user_id_or_session_id, force_new).
    """
    CONSOLE.print()
    CONSOLE.print(
        f"[bold cyan]── Sessions for [white]{super_user}[/] ──────────────────[/]"
    )

    if not sessions:
        CONSOLE.print("  [dim]No sessions yet — a new one will be created.[/]")
        return super_user, False

    for i, s in enumerate(sessions, 1):
        sid = s.get("id", "")
        title = s.get("title", "")
        turns = s.get("turns", 0)
        active = bool(s.get("active"))

        title_str = f'  [dim]"{title}"[/]' if title else ""
        turns_str = f"[dim]turns={turns}[/]"
        active_tag = "  [bold yellow][active][/]" if active else ""
        current_tag = "  [green]← current[/]" if active else ""

        CONSOLE.print(
            f"  [bold]{i}.[/]  [cyan]{sid}[/]"
            f"{title_str}  {turns_str}{active_tag}{current_tag}"
        )

    CONSOLE.print()
    CONSOLE.print("  [bold cyan]n[/]  Start a fresh new session")
    CONSOLE.print("  [dim](blank = resume latest)[/]")
    CONSOLE.print()

    while True:
        try:
            raw = (
                input("Select session (number / n / Enter for latest): ")
                .strip()
                .lower()
            )
        except (EOFError, KeyboardInterrupt):
            sys.exit(0)

        if raw == "n":
            return super_user, True  # force_new

        if raw == "" or raw == "l":
            # Resume the last (highest-numbered) session
            latest = sessions[-1].get("id", super_user)
            return latest, False

        if raw.isdigit():
            idx = int(raw) - 1
            if 0 <= idx < len(sessions):
                sid = sessions[idx].get("id", "")
                return sid, False

        CONSOLE.print("  [red]Invalid choice.[/]")


# ─────────────────────────────────────────────────────────────────────────────
# Main entry-point called by TerminalGateway before connect()
# ─────────────────────────────────────────────────────────────────────────────


def pick_identity(
    handler_host: str = "127.0.0.1",
    handler_port: int = 6060,
    requested_user_id: Optional[str] = None,
    user_store: Path = _DEFAULT_STORE,
    show_banner: bool = True,
) -> tuple[str, bool]:
    """
    Full pre-connect interactive flow.

    Returns ``(user_id_or_session_id, force_new)`` ready to pass to
    ``Gateway.connect()``.

    If *requested_user_id* is provided (CLI ``--user-id`` flag) the
    picker is skipped and we return it directly.
    """
    # Lazy import Gateway to avoid circular deps
    from gateway import Gateway  # type: ignore

    if show_banner:
        print_banner(handler_host, handler_port)

    # Fast path: --user-id flag was given
    if requested_user_id:
        _save_user(requested_user_id, user_store)
        return requested_user_id, False

    # Load saved user
    saved_raw = _load_saved_user(user_store)
    saved_su = _extract_super_user(saved_raw) if saved_raw else None

    # Fetch super-users from handler
    users_fetch_ok = False
    try:
        users = Gateway.list_users(handler_host, handler_port)
        users_fetch_ok = True
    except Exception as exc:
        CONSOLE.print(f"[dim red]Note: could not list users ({exc})[/]")
        users = []

    # If user listing failed, keep saved user as local hint.
    # If listing succeeded, trust server truth and avoid resurrecting deleted users.
    if (
        (not users_fetch_ok)
        and saved_su
        and not any(str(u.get("id", "")) == saved_su for u in users)
    ):
        users = users + [{"id": saved_su, "active": False}]

    # Super-user selection
    super_user = _pick_super_user(users, saved_su)
    _save_user(super_user, user_store)

    # Fetch sessions for this super-user
    try:
        sessions = Gateway.list_sessions(super_user, handler_host, handler_port)
    except Exception:
        sessions = []

    user_id, force_new = _pick_session(super_user, sessions)
    CONSOLE.print()
    return user_id, force_new
