#!/usr/bin/env python3
import json
import os
import sqlite3
import sys
import time
from pathlib import Path
from runtime.sdk.python.velix_process import VelixProcess

# ── Constants ────────────────────────────────────────────────────────────────

_DEFAULT_LIMIT = 10
_MAX_LIMIT = 50
_SNIPPET_MAX_TOKENS = 64  # approximate word count for snippet
_SNIPPET_HIGHLIGHT_START = ">>>"
_SNIPPET_HIGHLIGHT_END = "<<<"


class SessionSearchTool(VelixProcess):
    def __init__(self) -> None:
        super().__init__("session_search", "tool")
        self.root = self._find_velix_root()
        self.storage_cfg = self._load_storage_config()
        self.storage_backend = self.storage_cfg.get("backend", "json")
        json_root = self.storage_cfg.get("json_root", "memory/sessions")
        self.sessions_root = self.root / json_root
        self.velix_sqlite_db = self.root / self.storage_cfg.get(
            "sqlite_path", ".velix/velix.db"
        )
        self.index_db = self.root / "memory" / ".session_search_index.db"
        self.manifest_json = self.root / "memory" / ".session_search_manifest.json"

    def _load_storage_config(self) -> dict:
        for rel in (
            "config/storage.json",
            "../config/storage.json",
            "build/config/storage.json",
        ):
            p = self.root / rel
            if p.exists():
                try:
                    with open(p, "r", encoding="utf-8") as f:
                        raw = json.load(f)
                    if isinstance(raw, dict):
                        return raw
                except Exception:
                    pass
        return {
            "backend": "json",
            "json_root": "memory/sessions",
            "sqlite_path": ".velix/velix.db",
        }

    @staticmethod
    def _extract_super_user(session_id: str) -> str:
        pos = session_id.rfind("_s")
        if pos != -1 and pos + 2 < len(session_id):
            suffix = session_id[pos + 2 :]
            if suffix.isdigit():
                return session_id[:pos]
        return session_id

    @classmethod
    def _is_session_id(cls, value: str) -> bool:
        if not value:
            return False
        return cls._extract_super_user(value) != value

    @staticmethod
    def _parse_proc_session(session_id: str) -> tuple[str, str] | None:
        if not session_id.startswith("proc_"):
            return None
        rest = session_id[5:]
        sep = rest.find("_")
        if sep == -1:
            return None
        pid = rest[:sep]
        if not pid.isdigit():
            return None
        return pid, session_id

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

    def _open_db(self) -> sqlite3.Connection:
        self.index_db.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(str(self.index_db))
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")

        ddl = """
        CREATE TABLE IF NOT EXISTS turns (
            convo_id   TEXT NOT NULL,
            turn_idx   INTEGER NOT NULL,
            role       TEXT NOT NULL,
            content    TEXT NOT NULL,
            ts_ms      INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (convo_id, turn_idx)
        );
        CREATE VIRTUAL TABLE IF NOT EXISTS turns_fts USING fts5(
            content,
            content=turns,
            content_rowid=rowid,
            tokenize='porter unicode61'
        );
        CREATE TABLE IF NOT EXISTS indexed_files (
            path      TEXT PRIMARY KEY,
            mtime_ns  INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS indexed_sources (
            source_key TEXT PRIMARY KEY,
            source_version TEXT NOT NULL
        );
        """
        trigger_ddl = """
        CREATE TRIGGER IF NOT EXISTS turns_ai AFTER INSERT ON turns BEGIN
            INSERT INTO turns_fts(rowid, content) VALUES (new.rowid, new.content);
        END;
        CREATE TRIGGER IF NOT EXISTS turns_ad AFTER DELETE ON turns BEGIN
            INSERT INTO turns_fts(turns_fts, rowid, content) VALUES ('delete', old.rowid, old.content);
        END;
        CREATE TRIGGER IF NOT EXISTS turns_au AFTER UPDATE ON turns BEGIN
            INSERT INTO turns_fts(turns_fts, rowid, content) VALUES ('delete', old.rowid, old.content);
            INSERT INTO turns_fts(rowid, content) VALUES (new.rowid, new.content);
        END;
        """
        conn.executescript(ddl)
        conn.executescript(trigger_ddl)
        conn.commit()
        return conn

    def _scan_convo_files(self, user_id: str, mode: str) -> list[Path]:
        """Return session JSON files scoped to mode and optional user/session identifier."""
        files: list[Path] = []
        if not self.sessions_root.exists():
            return files

        users_root = self.sessions_root / "users"
        procs_root = self.sessions_root / "procs"
        include_users = mode in ("all", "user")
        include_procs = mode in ("all", "proc")

        if not user_id:
            if include_users and users_root.exists():
                for super_dir in users_root.iterdir():
                    if not super_dir.is_dir():
                        continue
                    for session_dir in super_dir.iterdir():
                        if session_dir.is_dir():
                            files.extend(session_dir.glob("*.json"))
            if include_procs and procs_root.exists():
                for pid_dir in procs_root.iterdir():
                    if not pid_dir.is_dir():
                        continue
                    for session_dir in pid_dir.iterdir():
                        if session_dir.is_dir():
                            files.extend(session_dir.glob("*.json"))
            return files

        proc_parts = self._parse_proc_session(user_id)
        if proc_parts is not None:
            if not include_procs:
                return files
            pid, sid = proc_parts
            session_dir = procs_root / pid / sid
            if session_dir.exists() and session_dir.is_dir():
                files.extend(session_dir.glob("*.json"))
            return files

        if self._is_session_id(user_id):
            if not include_users:
                return files
            super_user = self._extract_super_user(user_id)
            session_dir = users_root / super_user / user_id
            if session_dir.exists() and session_dir.is_dir():
                files.extend(session_dir.glob("*.json"))
            return files

        # Fallback: treat user_id as super_user and scan all their sessions.
        if include_users:
            super_dir = users_root / user_id
            if super_dir.exists() and super_dir.is_dir():
                for session_dir in super_dir.iterdir():
                    if session_dir.is_dir():
                        files.extend(session_dir.glob("*.json"))
        return files

    def _index_messages(
        self, conn: sqlite3.Connection, convo_id: str, messages: list
    ) -> int:
        conn.execute("DELETE FROM turns WHERE convo_id = ?", (convo_id,))
        count = 0
        for idx, msg in enumerate(messages):
            if not isinstance(msg, dict):
                continue
            role = msg.get("role", "")
            content = msg.get("content", "")
            ts_ms = msg.get("timestamp_ms", 0)
            if not isinstance(content, str) or not content.strip():
                continue
            conn.execute(
                "INSERT OR REPLACE INTO turns (convo_id, turn_idx, role, content, ts_ms) "
                "VALUES (?, ?, ?, ?, ?)",
                (convo_id, idx, role, content, ts_ms),
            )
            count += 1
        return count

    def _index_file(self, conn: sqlite3.Connection, path: Path) -> int:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except Exception:
            return 0

        convo_id = data.get("convo_id", path.stem)
        messages = data.get("messages", [])
        if not isinstance(messages, list):
            return 0

        count = self._index_messages(conn, convo_id, messages)

        st = path.stat()
        source_key = "file:" + str(path)
        conn.execute(
            "INSERT OR REPLACE INTO indexed_sources (source_key, source_version) VALUES (?, ?)",
            (source_key, str(st.st_mtime_ns)),
        )
        return count

    def _list_sqlite_conversations(
        self, user_id: str, mode: str
    ) -> list[tuple[str, str, int, int]]:
        if not self.velix_sqlite_db.exists():
            return []
        rows: list[tuple[str, str, int, int]] = []
        try:
            src = sqlite3.connect(str(self.velix_sqlite_db))

            # Fetch current conversations
            cur = src.execute(
                "SELECT convo_id, messages_json, last_activity_ms, creator_pid, user_id FROM conversations"
            )
            for (
                convo_id,
                messages_json,
                last_activity_ms,
                creator_pid,
                convo_user_id,
            ) in cur.fetchall():
                if mode == "user" and int(creator_pid or -1) > 0:
                    continue
                if mode == "proc" and int(creator_pid or -1) <= 0:
                    continue

                if user_id:
                    proc_parts = self._parse_proc_session(user_id)
                    if proc_parts is not None:
                        if convo_id != user_id:
                            continue
                    elif self._is_session_id(user_id):
                        if convo_id != user_id:
                            continue
                    else:
                        if str(
                            convo_user_id or ""
                        ) != user_id and not convo_id.startswith(user_id + "_s"):
                            continue

                rows.append(
                    (
                        str(convo_id),
                        str(messages_json or "[]"),
                        int(last_activity_ms or 0),
                        int(creator_pid or -1),
                    )
                )

            # Also fetch historical snapshots (for post-compaction searchability)
            # Snapshots are stored as {session_id}_hN pseudo-convo_ids
            snap_cur = src.execute(
                "SELECT session_id, snapshot_n, messages_json, snapshot_ms FROM session_snapshots"
            )
            for (
                session_id,
                snapshot_n,
                messages_json,
                snapshot_ms,
            ) in snap_cur.fetchall():
                # Create pseudo-convo_id like the filesystem: session_id_hN
                snapshot_convo_id = f"{session_id}_h{snapshot_n}"

                # Apply same filtering as conversations
                if user_id:
                    proc_parts = self._parse_proc_session(user_id)
                    if proc_parts is not None:
                        if session_id != user_id:
                            continue
                    elif self._is_session_id(user_id):
                        if session_id != user_id:
                            continue
                    else:
                        if not session_id.startswith(user_id + "_s"):
                            continue

                rows.append(
                    (
                        str(snapshot_convo_id),
                        str(messages_json or "[]"),
                        int(snapshot_ms or 0),
                        -1,  # snapshots don't have creator_pid
                    )
                )

            src.close()
        except Exception:
            return []
        return rows

    def _update_index(self, conn: sqlite3.Connection, user_id: str, mode: str) -> None:
        current_keys: set[str] = set()

        if self.storage_backend == "sqlite":
            convos = self._list_sqlite_conversations(user_id, mode)
            for convo_id, messages_json, last_activity_ms, _creator_pid in convos:
                try:
                    messages = json.loads(messages_json)
                    if not isinstance(messages, list):
                        messages = []
                except Exception:
                    messages = []

                source_key = "convo:" + convo_id
                source_version = f"{last_activity_ms}:{len(messages)}"
                current_keys.add(source_key)

                row = conn.execute(
                    "SELECT source_version FROM indexed_sources WHERE source_key = ?",
                    (source_key,),
                ).fetchone()
                if row is None or str(row[0]) != source_version:
                    self._index_messages(conn, convo_id, messages)
                    conn.execute(
                        "INSERT OR REPLACE INTO indexed_sources (source_key, source_version) VALUES (?, ?)",
                        (source_key, source_version),
                    )

            existing = conn.execute(
                "SELECT source_key FROM indexed_sources WHERE source_key LIKE 'convo:%'"
            ).fetchall()
            for (source_key,) in existing:
                if source_key not in current_keys:
                    convo_id = source_key[len("convo:") :]
                    conn.execute("DELETE FROM turns WHERE convo_id = ?", (convo_id,))
                    conn.execute(
                        "DELETE FROM indexed_sources WHERE source_key = ?",
                        (source_key,),
                    )
            conn.commit()
            return

        files = self._scan_convo_files(user_id, mode)
        for f in files:
            source_key = "file:" + str(f)
            try:
                source_version = str(f.stat().st_mtime_ns)
            except OSError:
                continue
            current_keys.add(source_key)

            row = conn.execute(
                "SELECT source_version FROM indexed_sources WHERE source_key = ?",
                (source_key,),
            ).fetchone()
            if row is None or str(row[0]) != source_version:
                self._index_file(conn, f)

        existing = conn.execute(
            "SELECT source_key FROM indexed_sources WHERE source_key LIKE 'file:%'"
        ).fetchall()
        for (source_key,) in existing:
            if source_key not in current_keys:
                path = source_key[len("file:") :]
                convo_id = Path(path).stem
                conn.execute("DELETE FROM turns WHERE convo_id = ?", (convo_id,))
                conn.execute(
                    "DELETE FROM indexed_sources WHERE source_key = ?", (source_key,)
                )
        conn.commit()

    def run(self) -> None:
        query: str = self.params.get("query", "").strip()
        if not query:
            self._report_error("query is required")
            return

        limit: int = int(self.params.get("limit", _DEFAULT_LIMIT))
        limit = max(1, min(limit, _MAX_LIMIT))
        mode: str = self.params.get("mode", "all")
        user_id: str = self.params.get("user_id", "")
        since_hours: float = float(self.params.get("since_hours", 0))

        try:
            conn = self._open_db()
            self._update_index(conn, user_id, mode)

            since_ms = 0
            if since_hours > 0:
                since_ms = int((time.time() - since_hours * 3600) * 1000)

            sql = """
                SELECT
                    t.convo_id,
                    t.role,
                    t.ts_ms,
                    snippet(turns_fts, 0, ?, ?, '...', ?) AS snippet,
                    bm25(turns_fts) AS score
                FROM turns_fts
                JOIN turns AS t ON t.rowid = turns_fts.rowid
                WHERE turns_fts MATCH ?
            """
            sql_params: list = [
                _SNIPPET_HIGHLIGHT_START,
                _SNIPPET_HIGHLIGHT_END,
                _SNIPPET_MAX_TOKENS,
                query,
            ]

            if since_ms:
                sql += " AND t.ts_ms >= ?"
                sql_params.append(since_ms)

            if user_id:
                if self._is_session_id(user_id) or self._parse_proc_session(user_id):
                    sql += " AND (t.convo_id = ? OR t.convo_id GLOB ?)"
                    sql_params.append(user_id)
                    sql_params.append(user_id + "_h*")
                else:
                    sql += " AND t.convo_id GLOB ?"
                    sql_params.append(user_id + "_s*")

            sql += " ORDER BY score LIMIT ?"
            sql_params.append(limit)

            rows = conn.execute(sql, sql_params).fetchall()
            results = []
            for row in rows:
                convo_id, role, ts_ms, snippet_text, score = row
                results.append(
                    {
                        "convo_id": convo_id,
                        "role": role,
                        "ts_ms": ts_ms,
                        "snippet": snippet_text,
                        "score": round(score, 4),
                    }
                )

            conn.close()

            self.report_result(
                self.parent_pid,
                {
                    "status": "ok",
                    "query": query,
                    "mode": mode,
                    "result_count": len(results),
                    "results": results,
                },
                self.entry_trace_id,
            )

        except sqlite3.OperationalError as exc:
            self._report_error(
                f"FTS5 query error: {exc}. Hint: Wrap phrases in quotes."
            )
        except Exception as exc:
            self._report_error(f"Failed to search: {exc}")

    def _report_error(self, message: str) -> None:
        self.report_result(
            self.parent_pid, {"status": "error", "error": message}, self.entry_trace_id
        )


if __name__ == "__main__":
    SessionSearchTool().start()
