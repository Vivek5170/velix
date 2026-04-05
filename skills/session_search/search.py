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

class SessionSearchSkill(VelixProcess):
    def __init__(self) -> None:
        super().__init__("session_search", "skill")
        self.root = self._find_velix_root()
        self.sessions_root = self.root / "memory" / "sessions"
        self.index_db = self.root / "memory" / ".session_search_index.db"
        self.manifest_json = self.root / "memory" / ".session_search_manifest.json"

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

    def _scan_convo_files(self, user_id: str) -> list[Path]:
        """Return all session snapshot JSON files for the given super_user."""
        files: list[Path] = []
        if not self.sessions_root.exists():
            return files

        super_user = user_id.split('_')[0] if user_id else ""

        for session_dir in self.sessions_root.iterdir():
            if session_dir.is_dir():
                if super_user and not session_dir.name.startswith(super_user + "_"):
                    continue
                files.extend(session_dir.glob("*.json"))
        return files

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

        st = path.stat()
        conn.execute(
            "INSERT OR REPLACE INTO indexed_files (path, mtime_ns) VALUES (?, ?)",
            (str(path), st.st_mtime_ns),
        )
        return count

    def _update_index(self, conn: sqlite3.Connection, user_id: str) -> None:
        files = self._scan_convo_files(user_id)
        stale = []
        for f in files:
            try:
                mtime_ns = f.stat().st_mtime_ns
            except OSError:
                continue
            row = conn.execute(
                "SELECT mtime_ns FROM indexed_files WHERE path = ?", (str(f),)
            ).fetchone()
            if row is None or row[0] != mtime_ns:
                stale.append(f)
        
        if not stale:
            return
        for f in stale:
            self._index_file(conn, f)
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
            self._update_index(conn, "") # Index everything for complete search
            
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
                super_user = user_id.split('_')[0]
                sql += " AND t.convo_id LIKE ?"
                sql_params.append(super_user + "_%")

            sql += " ORDER BY score LIMIT ?"
            sql_params.append(limit)

            rows = conn.execute(sql, sql_params).fetchall()
            results = []
            for row in rows:
                convo_id, role, ts_ms, snippet_text, score = row
                results.append({
                    "convo_id": convo_id,
                    "role": role,
                    "ts_ms": ts_ms,
                    "snippet": snippet_text,
                    "score": round(score, 4),
                })
            
            conn.close()
            
            self.report_result(self.parent_pid, {
                "status": "ok",
                "query": query,
                "mode": mode,
                "result_count": len(results),
                "results": results,
            }, self.entry_trace_id)

        except sqlite3.OperationalError as exc:
            self._report_error(f"FTS5 query error: {exc}. Hint: Wrap phrases in quotes.")
        except Exception as exc:
            self._report_error(f"Failed to search: {exc}")

    def _report_error(self, message: str) -> None:
        self.report_result(self.parent_pid, {
            "status": "error",
            "error": message
        }, self.entry_trace_id)

if __name__ == "__main__":
    SessionSearchSkill().start()
