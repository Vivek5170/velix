# Storage Backends

Velix supports multiple storage backends for conversation history. Choose based on your use case:
- **JSON**: Easy inspection, manual editing, single-machine deployments
- **SQLite**: Concurrent access, better performance at scale, suitable for multi-user systems

---

## Configuration

Edit `config/storage.json` to select your backend:

```json
{
  "backend": "json",
  "json_root": "memory/sessions",
  "sqlite_path": ".velix/velix.db"
}
```

### Settings

| Key | Type | Description |
|-----|------|-------------|
| `backend` | string | `"json"` or `"sqlite"` |
| `json_root` | string | Directory path where JSON session files are stored (default: `memory/sessions`) |
| `sqlite_path` | string | Path to SQLite database file (default: `.velix/velix.db`) |

---

## JSON Backend (Default)

Conversations are stored as individual JSON files in `memory/sessions/`:

```
memory/
  sessions/
    user1_s1.json
    user1_s2.json
    user2_s1.json
```

### Advantages

- **Human-readable**: Easy to inspect and debug
- **Git-friendly**: Works well with version control
- **Manual editing**: Modify conversations directly in any text editor
- **No setup**: No database initialization required
- **Single-machine**: Ideal for development and single-user deployments

### Disadvantages

- **Concurrent writes**: Can cause conflicts if multiple processes write simultaneously
- **Search performance**: Full scan required for large conversation sets
- **Scalability**: Not suitable for thousands of concurrent sessions

### Use Cases

- Local development and testing
- Single-user deployments
- Air-gapped environments without database dependencies
- Projects requiring version control of conversation history

---

## SQLite Backend

Conversations are stored in a single SQLite database file with concurrent access support.

### Advantages

- **Concurrent access**: WAL mode enables safe multi-process reads/writes
- **Query performance**: Indexed queries for search and filtering
- **Scalability**: Supports thousands of sessions without performance degradation
- **Atomic transactions**: Session updates are ACID-compliant
- **Tenant isolation**: Built-in support for per-user/tenant data isolation

### Disadvantages

- **Less human-readable**: Requires SQLite tools to inspect
- **Database dependency**: SQLite must be installed (usually built-in)
- **Migration required**: Switching from JSON requires data migration (manual for now)

### Use Cases

- Multi-user production deployments
- High-concurrency environments (many simultaneous users)
- Systems requiring strong data consistency guarantees
- Scaling to 100+ sessions or more

---

## Switching Backends

### From JSON to SQLite

1. Stop Velix
2. Update `config/storage.json`:
   ```json
   {
     "backend": "sqlite",
     "sqlite_path": ".velix/velix.db"
   }
   ```
3. Restart Velix

**Note**: Existing JSON sessions in `memory/sessions/` will be preserved. To migrate them to SQLite, you would need a migration tool (not included in base distribution).

### From SQLite to JSON

1. Stop Velix
2. Update `config/storage.json`:
   ```json
   {
     "backend": "json",
     "json_root": "memory/sessions"
   }
   ```
3. Restart Velix

**Note**: Existing SQLite sessions remain in the database. To use them as JSON, you would need to export them (not included in base distribution).

---

## Architecture

Velix uses a **modular storage abstraction**:

### Storage Provider Interface

All backends implement `IStorageProvider` (defined in `llm/storage/istorage_provider.hpp`):

```cpp
class IStorageProvider {
public:
    virtual ~IStorageProvider() = default;
    
    // Save a conversation session
    virtual void SaveSession(const std::string& session_id, const SessionData& data) = 0;
    
    // Load a conversation session
    virtual SessionData LoadSession(const std::string& session_id) = 0;
    
    // Delete a conversation session
    virtual void DeleteSession(const std::string& session_id) = 0;
    
    // List all session IDs (optionally filtered)
    virtual std::vector<std::string> ListSessions(const std::string& user_id) = 0;
};
```

### Implementation Details

**JSON Provider** (`llm/storage/json_storage_provider.cpp`):
- Serializes `SessionData` to JSON
- Writes to `{json_root}/{session_id}.json`
- Reads JSON files on demand

**SQLite Provider** (`llm/storage/sqlite_storage_provider.cpp`):
- Creates tables for sessions, metadata, and indices
- Uses WAL (Write-Ahead Logging) for concurrent access
- Enforces busy timeout for transaction conflicts
- Indexes on session_id, user_id for fast queries

### Components Using Storage

- **Scheduler** (`llm/scheduler.cpp`): Stores conversation state and turn history
- **Supervisor** (`llm/supervisor.cpp`): Persists session metadata and cleanup tracking
- **SessionIO**: Orchestrates load/save operations via `IStorageProvider`

### Provider Factory

`llm/storage/provider_factory.cpp` creates the appropriate provider based on `config/storage.json`:

```cpp
auto provider = CreateStorageProvider(config);  // Returns JSON or SQLite provider
```

---

## Long-Term Storage (Persona Memory)

**Important**: `memory/agentfiles/*` (including `soul.md` and `user.md`) remains **file-based** regardless of backend choice.

These files store:
- `soul.md`: Core system prompt and identity
- `user.md`: Long-term facts about the user
- Custom agent files

These are intentionally kept as files because they are:
- Edited by users manually
- Shared across multiple sessions
- Part of persona identity, not session history

---

## Performance Tuning

### JSON Backend

- Store `json_root` on fast SSD for best performance
- Use symlinks if you need to move the sessions directory
- Periodically archive old sessions to separate directories to keep active directory size manageable

### SQLite Backend

- Database file can be on the same drive as the executable
- SQLite automatically handles WAL files (`.velix/velix.db-wal`)
- Checkpoint/vacuum occurs automatically; no manual maintenance required
- For high-concurrency (100+ simultaneous users), consider PRAGMA settings:
  ```sql
  PRAGMA journal_mode=WAL;
  PRAGMA busy_timeout=5000;
  PRAGMA synchronous=NORMAL;
  ```

---

## Troubleshooting

### JSON Backend

**Symptom**: "Permission denied" when saving sessions
- Verify `memory/sessions/` directory exists and is writable
- Check file permissions on parent directories

**Symptom**: Sessions not persisting
- Verify Velix has write permission to `json_root` directory
- Check disk space availability

### SQLite Backend

**Symptom**: "database is locked" errors
- Increase `busy_timeout` in configuration or code
- Verify only one Velix instance is running
- Check for zombie processes holding connections

**Symptom**: Slow session saves with high concurrency
- SQLite can handle most workloads, but consider:
  - Archiving old sessions to a separate database
  - Using read replicas for query-heavy workloads (advanced)

**Symptom**: Database file growing too large
- Sessions are never automatically pruned; delete old sessions manually or implement a cleanup tool
- Use `VACUUM` command to reclaim space after mass deletions

---

## Migration & Backup

### Backing Up Sessions

**JSON Backend:**
```bash
tar -czf session-backup-$(date +%s).tar.gz memory/sessions/
```

**SQLite Backend:**
```bash
sqlite3 .velix/velix.db ".backup '.velix/velix-backup-$(date +%s).db'"
```

### Exporting Sessions

To export a session from SQLite to JSON for inspection or migration:

```bash
sqlite3 .velix/velix.db "SELECT * FROM sessions WHERE session_id='user1_s1';" --json
```

---

## Recommendations by Use Case

| Use Case | Recommended | Reasoning |
|----------|------------|-----------|
| Local development | **JSON** | Easy debugging, no setup |
| Single-user production | **JSON** | Simple, sufficient for one user |
| Multi-user local network | **SQLite** | Supports concurrent access safely |
| Cloud deployment | **SQLite** | Better scalability and performance |
| Air-gapped environment | **JSON** | No external database dependency |
| High-concurrency (100+) | **SQLite** | Essential for managing many simultaneous sessions |
