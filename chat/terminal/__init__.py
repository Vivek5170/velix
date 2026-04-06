"""
terminal — Velix interactive terminal package.

Entry point::

    from terminal import run_terminal
    run_terminal()

Or run directly::

    python -m terminal [options]
"""

from .terminal_gateway import TerminalGateway
from .session_picker   import pick_identity

__all__ = ["TerminalGateway", "pick_identity", "run_terminal"]


def run_terminal(
    host: str                = "127.0.0.1",
    port: int                = 6060,
    user_id: str | None      = None,
    tool_output_mode: str    = "full",
    stream_enabled: bool     = True,
    force_new: bool          = False,
    no_banner: bool          = False,
) -> None:
    """
    Full Velix terminal session.

    Shows the session-picker UI (unless *user_id* is given), connects to
    the Handler, then enters the interactive read-eval loop.
    """
    from pathlib import Path
    store = Path.home() / ".velix_user"

    if not user_id and not force_new:
        user_id, force_new = pick_identity(
            handler_host=host,
            handler_port=port,
            requested_user_id=user_id,
            user_store=store,
            show_banner=not no_banner,
        )

    gw = TerminalGateway(
        handler_host=host,
        handler_port=port,
        user_id=user_id,
        tool_output_mode=tool_output_mode,
        stream_enabled=stream_enabled,
        user_store=store,
    )

    try:
        gw.connect(force_new=force_new)
    except RuntimeError as exc:
        import sys
        print(f"\n[Fatal] {exc}", file=sys.stderr)
        sys.exit(1)

    gw.run()