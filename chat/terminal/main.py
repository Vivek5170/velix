#!/usr/bin/env python3
"""
__main__.py — ``python -m terminal``

Usage
─────
  python -m terminal                        # interactive session picker
  python -m terminal --user-id vivek        # connect as super-user
  python -m terminal --user-id vivek_s2     # connect to specific session
  python -m terminal --new                  # force a brand-new session
  python -m terminal --host 10.0.0.5        # remote handler
  python -m terminal --tool-mode summary    # compact tool output
  python -m terminal --no-stream            # disable token streaming
"""

import argparse
import sys

from . import run_terminal


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="python -m terminal",
        description="Velix interactive terminal",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host",       default="127.0.0.1",
                        help="Handler host")
    parser.add_argument("--port",       type=int, default=6060,
                        help="Handler port")
    parser.add_argument("--user-id",    default=None,
                        help="super-user name or session_id; skips picker if given")
    parser.add_argument("--new",        action="store_true",
                        help="Force a brand-new session even if one exists")
    parser.add_argument(
        "--tool-mode",
        choices=["full", "summary", "silent"],
        default="full",
        help="Tool lifecycle event verbosity",
    )
    parser.add_argument("--no-stream",  action="store_true",
                        help="Disable token-by-token streaming")
    parser.add_argument("--no-banner",  action="store_true",
                        help="Skip the ASCII logo on startup")

    args = parser.parse_args()

    run_terminal(
        host             = args.host,
        port             = args.port,
        user_id          = args.user_id,
        tool_output_mode = args.tool_mode,
        stream_enabled   = not args.no_stream,
        force_new        = args.new,
        no_banner        = args.no_banner,
    )


if __name__ == "__main__":
    main()