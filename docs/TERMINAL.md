# Terminal Commands Reference

The Velix terminal is an interactive chat interface for multi-turn reasoning with agents and tools. This guide documents all available commands and options.

---

## Starting the Terminal

### Basic Startup

```bash
cd chat
python -m terminal
```

### Common Options

| Option | Example | Description |
|--------|---------|-------------|
| `--user-id` | `--user-id alice` | Use a specific user/persona (default: `velix`) |
| `--new` | `--new` | Create a new session instead of resuming the last one |
| `--tool-mode` | `--tool-mode summary` | Set tool approval mode (see below) |
| `--no-stream` | `--no-stream` | Disable token streaming (collect full response before displaying) |
| `--help` | `--help` | Show all available options |

### Examples

```bash
# Start with user 'alice'
python -m terminal --user-id alice

# Start with user 'alice' and a new session
python -m terminal --user-id alice --new

# Multi-session branching: alice's second session
python -m terminal --user-id alice_s2

# Silent mode (minimal tool output)
python -m terminal --tool-mode silent

# Batch mode (no streaming)
python -m terminal --no-stream
```

---

## Interactive Commands

Commands are entered with a leading `/` in the terminal. They are not sent to the LLM.

### Session Management

#### `/help`
Display all available commands and their descriptions.

```
/help
```

Output includes command list and keyboard shortcuts.

---

#### `/session-info`
Show information about the current session.

```
/session-info
```

Output:
- Current persona/user ID
- Current session ID
- Session creation date
- Number of turns/messages
- Current token count
- Storage backend in use

Example output:
```
Session Information:
  User ID: alice
  Session ID: alice_s1
  Created: 2025-04-20 10:15:23
  Messages: 12
  Tokens (approx): 2,847
  Storage: JSON
```

---

#### `/new`
Create a new session for the current user.

```
/new
```

Behavior:
- Starts a fresh conversation session
- Keeps the same user/persona
- Returns new session ID
- All previous messages are in the old session (accessible via `/list`)

Example:
```
/new
> New session created: alice_s2
```

---

#### `/list`
List all sessions for the current user.

```
/list
```

Output includes:
- All session IDs
- Creation date of each session
- Number of messages in each session
- Whether session is currently active

Example output:
```
Sessions for user 'alice':
  ✓ alice_s1   [2025-04-20 10:15:23]  12 messages  (active)
    alice_s2   [2025-04-20 11:02:15]  3 messages
    alice_s3   [2025-04-20 12:45:00]  45 messages
```

---

#### `/delete <session_id>`
Delete a specific session.

```
/delete alice_s2
```

Behavior:
- Permanently removes the session from storage
- If deleting the active session, automatically creates a fallback session
- Refuses deletion if the target session is active on another terminal connection

Example:
```
/delete alice_s2
> Session alice_s2 deleted
```

Error cases:
```
/delete alice_s1
> Error: Cannot delete active session on another client
```

---

#### `/switch <session_id>`
Switch to a different session for the same user.

```
/switch alice_s3
```

Behavior:
- Loads the specified session
- Resumes conversation from where it left off
- Returns an error if session doesn't exist

Example:
```
/switch alice_s1
> Switched to alice_s1 (12 messages, last updated 2025-04-20 10:50:15)
```

---

### Conversation Management

#### `/undo`
Remove the last message exchange (your message + LLM response).

```
/undo
```

Behavior:
- Removes both your last prompt and the agent's response
- Cannot undo beyond the session start
- Invalidates cached context (next exchange will recompute)

Example:
```
/undo
> Removed last message exchange
  Session now has 10 messages (was 12)
```

---

#### `/clear`
Clear the visual display without affecting the conversation state.

```
/clear
```

Behavior:
- Clears the terminal screen
- Conversation history is preserved in memory and storage
- Useful for cluttered terminal output

---

### Tool Approval & Monitoring

#### `/approve <tool_id>`
Approve a specific tool for execution (when in approval mode).

```
/approve tool_001
```

Behavior:
- Marks tool for execution
- Only available during tool approval prompts
- See "Tool Approval Modes" below

---

#### `/deny <tool_id>`
Deny/reject a specific tool from execution (when in approval mode).

```
/deny tool_001
```

Behavior:
- Blocks tool execution
- Agent will be informed tool was rejected
- Agent may suggest alternative actions

---

### System Information

#### `/status`
Show system status and connection information.

```
/status
```

Output includes:
- Connection status to kernel
- Active scheduler status
- Memory usage summary
- Recent errors (if any)

Example output:
```
System Status:
  Kernel: Connected ✓
  Scheduler: Active ✓
  Memory (approx): 342 MB
  Recent errors: None
```

---

#### `/config`
Show current configuration.

```
/config
```

Output includes:
- LLM provider (e.g., "llama.cpp")
- Model name
- Storage backend
- Terminal mode settings

---

## Tool Approval Modes

Control how sensitive tool operations require user confirmation.

### Modes

| Mode | Behavior | Use Case |
|------|----------|----------|
| `summary` | Prompt before each tool execution | Default; balanced |
| `silent` | Execute tools without prompting | Trusted environments, batch processing |
| `strict` | Require explicit approval for each tool | Air-gapped/high-security |
| `debug` | Show all tool details before execution | Troubleshooting |

### Setting Tool Mode at Startup

```bash
python -m terminal --tool-mode silent
python -m terminal --tool-mode strict
python -m terminal --tool-mode debug
```

### Changing Tool Mode During Session

Use the `/set-tool-mode` command:

```
/set-tool-mode strict
> Tool mode changed to: strict
```

---

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+C` | Exit terminal gracefully |
| `Ctrl+D` | Exit terminal (EOF) |
| `Ctrl+L` | Clear screen (equivalent to `/clear`) |
| `Ctrl+R` | Search command history (prompt_toolkit feature) |
| `Ctrl+A` | Move to line start |
| `Ctrl+E` | Move to line end |
| `Up Arrow` | Previous message in history |
| `Down Arrow` | Next message in history |
| `Tab` | Auto-complete (if enabled) |

---

## Message Format & Special Syntax

### Sending Regular Messages

Simply type your message and press Enter:

```
> What's the weather in New York?
```

The message is sent to the LLM for processing.

### Multi-Line Messages

Use `Shift+Enter` to create line breaks within a single message:

```
> This is a longer question that spans
  multiple lines for clarity
```

Press `Enter` (alone) to send when complete.

### Code Blocks

You can include code in messages using standard Markdown:

```
> Write a Python function to calculate Fibonacci numbers:

def fibonacci(n):
    if n <= 1:
        return n
    return fibonacci(n-1) + fibonacci(n-2)
```

The agent will process code blocks correctly when included in messages.

---

## Agent & Tool Execution

### How Tools Are Invoked

When an agent decides to use a tool:

1. **Tool approval phase**: Terminal shows tool name and description
2. **Approval mode handling**:
   - `summary`: Auto-approves after brief confirmation
   - `silent`: Auto-executes immediately
   - `strict`: Waits for manual `/approve` or `/deny`
3. **Execution**: Tool runs in its own process
4. **Result reporting**: Output is displayed and sent back to agent

### Example Tool Execution Flow

```
> Find information about Velix

Agent: I'll search for Velix information...

[Tool Approval]
Tool: web_search
Query: "Velix operating system"
Description: Search the web for relevant pages

> /approve tool_001

[Executing web_search...]
Results:
  - Velix: The Agentic Operating System (official docs)
  - GitHub repository with source code
  - Technical architecture blog post

Agent: I found several resources about Velix...
```

---

## Session Persistence

### Automatic Saving

- Every message exchange is automatically saved to storage
- Undo operations trigger cache invalidation (next response will recompute context)
- Session metadata is updated continuously

### Manual Backup

Create a backup of your current session:

```bash
# JSON backend
cp memory/sessions/alice_s1.json memory/sessions/alice_s1.backup.json

# SQLite backend
sqlite3 .velix/velix.db ".backup 'velix-backup.db'"
```

---

## Troubleshooting

### Terminal Won't Start

**Problem**: "Connection refused"
```
Error: Could not connect to kernel on localhost:8080
```

**Solution**:
- Ensure `./build/integration_kernel` is running in another terminal
- Check kernel started successfully (no error messages)

---

### Messages Not Persisting

**Problem**: Session history disappears after restart

**Solution**:
- Verify `config/storage.json` backend setting
- Check `memory/sessions/` directory exists (JSON backend)
- Verify `.velix/velix.db` exists (SQLite backend)
- Check file permissions

---

### Tool Execution Fails

**Problem**: Tool returns an error or times out

**Solution**:
- Check tool is installed and configured (see tool manifest)
- Verify tool dependencies (e.g., `uv` for Python tools)
- Try again; transient errors may self-resolve
- Check `/status` for system issues

---

### Slow Response Times

**Problem**: Agent takes long time to respond

**Causes & Solutions**:
- **LLM slow**: Check LLM server is responding (test with curl)
- **High context**: Use `/undo` to remove messages; context is too large
- **Many concurrent sessions**: Scheduler may be queuing your requests
- **Disk I/O**: Storage backend may be bottlenecked

---

### "Out of Context" Errors

**Problem**: Agent says context limit reached

**Solution**:
- This should not happen in normal operation; compaction should trigger automatically
- If you see this, contact support with session ID
- Temporary workaround: Use `/new` to start a fresh session

---

## Best Practices

1. **Use specific user IDs**: Helps organize memories across multiple users
   ```bash
   python -m terminal --user-id alice
   ```

2. **Create sessions for different topics**: Keeps context manageable
   ```
   /new
   ```

3. **Approve tools carefully**: Understand what each tool does
   ```
   /approve tool_001
   ```

4. **Monitor token usage**: Check `/session-info` periodically
   ```
   /session-info
   ```

5. **Back up important sessions**: Export before major operations
   ```bash
   cp memory/sessions/session_id.json session_id.backup.json
   ```

6. **Use `/undo` to correct mistakes**: Cheaper than retyping long contexts
   ```
   /undo
   ```
