#!/usr/bin/env python3
import difflib
import json
import os
import sys
from pathlib import Path
from runtime.sdk.python.velix_process import VelixProcess

if sys.platform == "win32":
    import msvcrt

    def _lock_exclusive(fd: int) -> None:
        msvcrt.locking(fd, msvcrt.LK_LOCK, 1 << 31)

    def _unlock(fd: int) -> None:
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1 << 31)
else:
    import fcntl

    def _lock_exclusive(fd: int) -> None:
        fcntl.flock(fd, fcntl.LOCK_EX)

    def _unlock(fd: int) -> None:
        fcntl.flock(fd, fcntl.LOCK_UN)


class UpdateMemoryTool(VelixProcess):
    def __init__(self) -> None:
        super().__init__("update_memory", "tool")

    def _find_velix_root(self) -> Path:
        cur = Path.cwd()
        for _ in range(8):
            if (cur / "memory").exists():
                return cur
            if cur.parent == cur:
                break
            cur = cur.parent
        return Path.cwd()

    @staticmethod
    def _extract_super_user(user_id: str) -> str:
        pos = user_id.rfind("_s")
        if pos != -1 and pos + 2 < len(user_id):
            suffix = user_id[pos + 2 :]
            if suffix.isdigit():
                return user_id[:pos]
        return user_id

    @staticmethod
    def _is_session_id(user_id: str) -> bool:
        if not user_id:
            return False
        return UpdateMemoryTool._extract_super_user(user_id) != user_id

    @staticmethod
    def _read_lines(file_path: Path) -> list[str]:
        if not file_path.exists():
            return []
        with open(file_path, "r", encoding="utf-8") as f:
            return f.readlines()

    @staticmethod
    def _atomic_write(file_path: Path, content: str) -> None:
        tmp_path = file_path.with_suffix(file_path.suffix + ".tmp")
        with open(tmp_path, "w", encoding="utf-8") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, file_path)

    @staticmethod
    def _suggest_similar(lines: list[str], query: str, n: int = 2) -> list[str]:
        stripped = {l.strip() for l in lines if l.strip()}
        if not stripped:
            return []
        return difflib.get_close_matches(query.strip(), stripped, n=n, cutoff=0.0)

    def run(self) -> None:
        user_id = str(getattr(self, "user_id", "") or "").strip()
        target_file = self.params.get("target_file", "").strip()
        content = self.params.get("content", "")
        update_mode = self.params.get("update_mode", "append").strip().lower()
        replacement = self.params.get("replacement", "")

        if not user_id:
            self._report_error(
                "runtime user context missing (VELIX_USER_ID not injected)"
            )
            return

        if not self._is_session_id(user_id):
            self._report_error(
                "runtime user_id must be a full session_id like 'sameer_s1' or 'test_user_s5'"
            )
            return

        if target_file not in ("user.md", "soul.md"):
            self._report_error("target_file must be 'user.md' or 'soul.md'")
            return

        if not content:
            self._report_error("content is required")
            return

        if update_mode == "patch" and not replacement:
            self._report_error("replacement is required for patch mode")
            return

        if update_mode == "overwrite":
            self._report_error(
                "overwrite mode removed — use remove + append instead"
            )
            return

        super_user = self._extract_super_user(user_id)

        root = self._find_velix_root()
        agentfiles_dir = root / "memory" / "agentfiles" / super_user

        try:
            agentfiles_dir.mkdir(parents=True, exist_ok=True)
            file_path = agentfiles_dir / target_file
            lock_path = file_path.with_suffix(file_path.suffix + ".lck")

            lock_fd = os.open(str(lock_path), os.O_CREAT | os.O_WRONLY, 0o644)
            _lock_exclusive(lock_fd)
            try:
                if update_mode == "append":
                    existing = self._read_lines(file_path)
                    query = content.strip()
                    already = any(l.strip() == query for l in existing)
                    if already:
                        message = f"Skipped — line already exists in {target_file}"
                    else:
                        with open(file_path, "a", encoding="utf-8") as f:
                            if existing and not existing[-1].endswith("\n"):
                                f.write("\n")
                            f.write(content)
                            f.flush()
                            os.fsync(f.fileno())
                        message = f"Successfully updated {target_file} for user {super_user} (append)"

                elif update_mode == "remove":
                    query = content.strip()
                    lines = self._read_lines(file_path)
                    idx = -1
                    for i, l in enumerate(lines):
                        if l.strip() == query:
                            idx = i
                            break
                    if idx == -1:
                        suggestions = self._suggest_similar(lines, query, 2)
                        msg = f"No exact line match for '{query}'"
                        if suggestions:
                            msg += ". Closest matches:\n" + "\n".join(
                                f"  - {s}" for s in suggestions
                            )
                        self._report_error(msg)
                        return
                    lines.pop(idx)
                    self._atomic_write(file_path, "".join(lines))
                    message = f"Removed 1 line from {target_file}"

                elif update_mode == "patch":
                    query = content.strip()
                    lines = self._read_lines(file_path)
                    idx = -1
                    for i, l in enumerate(lines):
                        if l.strip() == query:
                            idx = i
                            break
                    if idx == -1:
                        suggestions = self._suggest_similar(lines, query, 2)
                        msg = f"No exact line match for '{query}'"
                        if suggestions:
                            msg += ". Closest matches:\n" + "\n".join(
                                f"  - {s}" for s in suggestions
                            )
                        self._report_error(msg)
                        return
                    lines[idx] = replacement + "\n" if not replacement.endswith("\n") else replacement
                    self._atomic_write(file_path, "".join(lines))
                    message = f"Patched {target_file}"

                else:
                    self._report_error(f"Unknown update_mode '{update_mode}'")
                    return
            finally:
                _unlock(lock_fd)
                os.close(lock_fd)
                try:
                    lock_path.unlink(missing_ok=True)
                except OSError:
                    pass

            self.report_result(
                self.parent_pid,
                {"status": "ok", "message": message},
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
