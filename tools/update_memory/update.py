#!/usr/bin/env python3
import json
import os
import re
import sys
from pathlib import Path
from runtime.sdk.python.velix_process import VelixProcess


class UpdateMemoryTool(VelixProcess):
    def __init__(self) -> None:
        super().__init__("update_memory", "tool")

    def _find_velix_root(self) -> Path:
        """Walk up from CWD looking for the memory directory."""
        cur = Path.cwd()
        for _ in range(8):
            if (cur / "memory").exists():
                return cur
            if cur.parent == cur:
                break
            cur = cur.parent
        return Path.cwd()

    def _extract_super_user(self, user_id: str) -> str:
        """Accept either super_user or session_id in super_user_sN format."""
        match = re.fullmatch(r"(.+)_s([0-9]+)", user_id)
        if match:
            return match.group(1)
        return user_id

    def _validate_session_id(self, user_id: str) -> bool:
        return bool(re.fullmatch(r"[A-Za-z0-9\-]+_s[0-9]+", user_id))

    def run(self) -> None:
        user_id = str(getattr(self, "user_id", "") or "").strip()
        target_file = self.params.get("target_file", "").strip()
        content = self.params.get("content", "")
        update_mode = self.params.get("update_mode", "append").strip().lower()

        if not user_id:
            self._report_error(
                "runtime user context missing (VELIX_USER_ID not injected)"
            )
            return

        if not self._validate_session_id(user_id):
            self._report_error(
                "runtime user_id must be a full session_id like 'sameer_s1'"
            )
            return

        if target_file not in ("user.md", "soul.md"):
            self._report_error("target_file must be 'user.md' or 'soul.md'")
            return

        if not content and update_mode == "append":
            self._report_error("content is required for append")
            return

        super_user = self._extract_super_user(user_id)

        root = self._find_velix_root()
        agentfiles_dir = root / "memory" / "agentfiles" / super_user

        try:
            agentfiles_dir.mkdir(parents=True, exist_ok=True)
            file_path = agentfiles_dir / target_file
            mode = "a" if update_mode == "append" else "w"

            with open(file_path, mode, encoding="utf-8") as f:
                if mode == "a" and file_path.exists() and file_path.stat().st_size > 0:
                    f.write("\n")
                f.write(content)

            self.report_result(
                self.parent_pid,
                {
                    "status": "ok",
                    "message": f"Successfully updated {target_file} for user {super_user} ({update_mode})",
                },
                self.entry_trace_id,
            )

        except Exception as exc:
            self._report_error(f"Failed to update memory file: {exc}")

    def _report_error(self, message: str) -> None:
        self.report_result(
            self.parent_pid, {"status": "error", "error": message}, self.entry_trace_id
        )


if __name__ == "__main__":
    UpdateMemoryTool().start()
