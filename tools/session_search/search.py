#!/usr/bin/env python3
import json
import os
import re
import sqlite3
import time
from pathlib import Path
from typing import Optional
from runtime.sdk.python.velix_process import VelixProcess

# ── Constants ────────────────────────────────────────────────────────────────

_DEFAULT_LIMIT = 10
_MAX_LIMIT = 50
_SNIPPET_MAX_TOKENS = 64  # approximate word count for snippet
_SNIPPET_HIGHLIGHT_START = ">>>"
_SNIPPET_HIGHLIGHT_END = "<<<"


# ── Helpers ──────────────────────────────────────────────────────────────────


def _sanitize_filename(s: str) -> str:
    """Sanitize a string for safe use as a filename. Replace unsafe chars with underscore."""
    return re.sub(r"[^A-Za-z0-9._-]", "_", s)


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
        # Per-tenant index DB path is set in run() after validating session_id
        self.index_db: Optional[Path] = None
        self.index_search_dir = self.root / ".velix" / "session_search"

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
        if self.index_db is None:
            raise RuntimeError("index_db path not set")
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

    def _check_fts5_availability(self, conn: sqlite3.Connection) -> bool:
        """Verify FTS5 and bm25 are available in the SQLite build."""
        try:
            result = conn.execute(
                "SELECT sqlite_compileoption_used('ENABLE_FTS5')"
            ).fetchone()
            if result and result[0] == 1:
                return True
        except Exception:
            pass
        return False

    def _execute_with_retry(
        self, conn: sqlite3.Connection, sql: str, params: tuple, max_retries: int = 3
    ) -> None:
        """Execute SQL with retry on database locked."""
        for attempt in range(max_retries):
            try:
                conn.execute(sql, params)
                return
            except sqlite3.OperationalError as e:
                if "database is locked" in str(e) and attempt < max_retries - 1:
                    time.sleep(0.1 * (2**attempt))  # exponential backoff
                    continue
                raise

    def _scan_convo_files(
        self, session_id: str, mode: str, is_proc: bool, tenant_key: str
    ) -> list[Path]:
        """
        Return session JSON files scoped to tenant and mode.
        - session_id: the current session (superuser_sN or proc_...)
        - mode: 'session' or 'user'
        - is_proc: whether this is a proc session
        - tenant_key: extracted tenant key (super_user or full proc session id)
        """
        files: list[Path] = []
        if not self.sessions_root.exists():
            return files

        users_root = self.sessions_root / "users"
        procs_root = self.sessions_root / "procs"

        if is_proc:
            # Proc session: only scan memory/sessions/procs/<pid>/<session_id>/*.json
            pid, _sid = self._parse_proc_session(session_id)
            session_dir = procs_root / pid / session_id
            if session_dir.exists() and session_dir.is_dir():
                files.extend(session_dir.glob("*.json"))
            return files

        # Normal user session: scan under users_root / tenant_key (the super_user)
        super_user = tenant_key
        super_dir = users_root / super_user
        if not super_dir.exists():
            return files

        if mode == "session":
            # Session mode: only scan current session directory
            # memory/sessions/users/<super_user>/<session_id>/*.json
            session_dir = super_dir / session_id
            if session_dir.exists() and session_dir.is_dir():
                files.extend(session_dir.glob("*.json"))
        else:  # mode == "user"
            # User mode: scan all sessions for this super_user
            # memory/sessions/users/<super_user>/*/*.json
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
        self, session_id: str, mode: str, is_proc: bool, tenant_key: str
    ) -> list[tuple[str, str, int, int]]:
        """
        List conversations from Velix SQLite DB, scoped to tenant and mode.
        - session_id: the current session (superuser_sN or proc_...)
        - mode: 'session' or 'user'
        - is_proc: whether this is a proc session
        - tenant_key: extracted tenant key (super_user or full proc session id)
        """
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
                # Include row based on scope
                if is_proc:
                    # For proc sessions, only include if convo_id matches this session
                    if convo_id != session_id:
                        continue
                else:
                    # For user sessions, filter by mode
                    if mode == "session":
                        # Session mode: only include exact session match
                        if convo_id != session_id:
                            continue
                    else:  # mode == "user"
                        # User mode: include if belongs to this superuser
                        super_user = tenant_key
                        if not (
                            (str(convo_user_id or "") == super_user)
                            or convo_id.startswith(super_user + "_s")
                        ):
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
                snap_session_id,
                snapshot_n,
                messages_json,
                snapshot_ms,
            ) in snap_cur.fetchall():
                # Create pseudo-convo_id like the filesystem: session_id_hN
                snapshot_convo_id = f"{snap_session_id}_h{snapshot_n}"

                # Apply same filtering as conversations
                if is_proc:
                    # For proc sessions, include if parent session matches
                    if snap_session_id != session_id:
                        continue
                else:
                    # For user sessions, filter by mode
                    if mode == "session":
                        # Session mode: include only if snapshot belongs to this session
                        if snap_session_id != session_id:
                            continue
                    else:  # mode == "user"
                        # User mode: include if snapshot belongs to any session of this superuser
                        super_user = tenant_key
                        if not snap_session_id.startswith(super_user + "_s"):
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

    def _update_index(
        self,
        conn: sqlite3.Connection,
        session_id: str,
        mode: str,
        is_proc: bool,
        tenant_key: str,
    ) -> None:
        """Update index with tenant-scoped data. mode is 'session' or 'user'."""
        current_keys: set[str] = set()

        if self.storage_backend == "sqlite":
            convos = self._list_sqlite_conversations(
                session_id, mode, is_proc, tenant_key
            )
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

        files = self._scan_convo_files(session_id, mode, is_proc, tenant_key)
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
        """Main search entrypoint. Reads VELIX_USER_ID from runtime env."""
        # Validate query parameter
        query: str = self.params.get("query", "").strip()
        if not query:
            self._report_error("query is required")
            return

        # Read and validate session_id from runtime environment (VELIX_USER_ID)
        session_id = str(self.user_id or "").strip()
        if not session_id:
            self._report_error("VELIX_USER_ID not set in environment")
            return

        # Parse session type (proc or normal user session)
        proc_parts = self._parse_proc_session(session_id)
        is_proc = proc_parts is not None

        # Validate session_id format
        if not is_proc and not self._is_session_id(session_id):
            self._report_error(
                "VELIX_USER_ID must be a session id (superuser_sN) or proc session (proc_<pid>_...)"
            )
            return

        # Determine tenant key for per-tenant index DB
        if is_proc:
            tenant_key = session_id
        else:
            tenant_key = self._extract_super_user(session_id)

        # Set per-tenant index DB path with sanitization
        sanitized_tenant = _sanitize_filename(tenant_key)
        self.index_db = self.index_search_dir / f"{sanitized_tenant}.db"

        # Read and validate mode parameter
        mode: str = self.params.get("mode", "session").strip()
        if mode not in ("session", "user"):
            self._report_error("mode must be 'session' or 'user'")
            return

        # Procs can only use session mode
        if is_proc and mode == "user":
            self._report_error("proc sessions may only use mode='session'")
            return

        # Parse limit parameter
        limit: int = int(self.params.get("limit", _DEFAULT_LIMIT))
        limit = max(1, min(limit, _MAX_LIMIT))

        try:
            # Open per-tenant index DB
            conn = self._open_db()

            # Check FTS5 availability
            if not self._check_fts5_availability(conn):
                self._report_error(
                    "FTS5/bm25 not available in sqlite build; "
                    "ensure sqlite is compiled with SQLITE_ENABLE_FTS5"
                )
                conn.close()
                return

            # Update index with appropriate scope
            self._update_index(conn, session_id, mode, is_proc, tenant_key)

            # Build FTS5 query with mode-based filtering
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

            # Add convo filter based on mode and session type
            if is_proc or mode == "session":
                # Session mode: search only current session and its snapshots
                sql += " AND (t.convo_id = ? OR t.convo_id GLOB ?)"
                sql_params.append(session_id)
                sql_params.append(session_id + "_h*")
            else:
                # User mode: search all sessions for the superuser and their snapshots
                sql += " AND t.convo_id GLOB ?"
                sql_params.append(tenant_key + "_s*")

            # Order by relevance (bm25 score) and limit
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

            # Set file permissions on index DB
            try:
                os.chmod(str(self.index_db), 0o600)
            except Exception:
                pass  # Best effort; don't fail if permissions can't be set

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
