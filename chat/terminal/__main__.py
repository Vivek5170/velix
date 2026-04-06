#!/usr/bin/env python3
"""Module entrypoint for `python -m terminal`.

This is used when Python resolves `terminal` as a package (for example,
when running from the `chat` directory).
"""

from .main import main


if __name__ == "__main__":
    main()
