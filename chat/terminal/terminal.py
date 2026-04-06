#!/usr/bin/env python3
"""Standalone `python -m terminal` entrypoint when cwd is `chat/terminal`.

This module avoids package-relative imports so it can run when the current
working directory is this folder.
"""

import argparse

from terminal_gateway import TerminalGateway
from session_picker import pick_identity


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="python -m terminal",
        description="Velix interactive terminal",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default="127.0.0.1", help="Handler host")
    parser.add_argument("--port", type=int, default=6060, help="Handler port")
    parser.add_argument(
        "--user-id",
        default=None,
        help="super-user name or session_id; skips picker if given",
    )
    parser.add_argument(
        "--new",
        action="store_true",
        help="Force a brand-new session even if one exists",
    )
    parser.add_argument(
        "--tool-mode",
        choices=["full", "summary", "silent"],
        default="full",
        help="Tool lifecycle event verbosity",
    )
    parser.add_argument(
        "--no-stream",
        action="store_true",
        help="Disable token-by-token streaming",
    )
    parser.add_argument(
        "--no-banner",
        action="store_true",
        help="Skip the ASCII logo on startup",
    )

    args = parser.parse_args()

    from pathlib import Path

    store = Path.home() / ".velix_user"
    user_id = args.user_id
    force_new = args.new

    if not user_id and not force_new:
        user_id, force_new = pick_identity(
            handler_host=args.host,
            handler_port=args.port,
            requested_user_id=user_id,
            user_store=store,
            show_banner=not args.no_banner,
        )

    gw = TerminalGateway(
        handler_host=args.host,
        handler_port=args.port,
        user_id=user_id,
        tool_output_mode=args.tool_mode,
        stream_enabled=not args.no_stream,
        user_store=store,
    )

    try:
        gw.connect(force_new=force_new)
    except RuntimeError as exc:
        import sys

        print(f"\n[Fatal] {exc}", file=sys.stderr)
        raise SystemExit(1)

    gw.run()


if __name__ == "__main__":
    main()
