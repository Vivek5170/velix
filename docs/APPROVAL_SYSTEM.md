# Tool Approval System

Velix includes a permission system to control how tools are presented and executed. This document describes the actual implementation.

---

## Overview

The tool approval system consists of two main components:

1. **Tool Output Mode** (`--tool-mode` flag): Controls how much detail is shown about tool execution
2. **Approval Mode** (`/approvemode` command): Controls whether tools require user confirmation before execution

These work together to provide different levels of oversight for tool execution.

---

## Tool Output Mode

Controls the verbosity of tool execution information displayed to the user.

### Modes

| Mode | Behavior |
|------|----------|
| `full` | Show all tool details, parameters, and complete output |
| `summary` | Show tool name and brief summary of output (default) |
| `silent` | Minimize tool output; show only essential results |

### Setting Tool Mode at Startup

```bash
# Show full tool details
python -m terminal --tool-mode full

# Summary mode (default)
python -m terminal --tool-mode summary

# Minimal output
python -m terminal --tool-mode silent
```

---

## Approval Mode (Interactive)

Controls whether tools automatically execute or require user confirmation.

### Modes

| Mode | Behavior | When to Use |
|------|----------|------------|
| `default` | Prompt user before each tool execution | Standard interactive use |
| `all` | Auto-approve all tools, no prompts | Trusted environments, batch processing |

### Setting Approval Mode During Session

Use the `/approvemode` command:

```
/approvemode all
> Approval mode changed to: all (auto-approve all tools)
```

Or:

```
/approvemode default
> Approval mode changed to: default (prompt before execution)
```

### Approving Individual Tools

When in `default` mode, you will see a tool approval prompt:

```
Agent: I'll search for information about Velix...

[Tool Request]
Tool: web_search
Description: Search the web

Approve tool execution? [y/n/always]:
```

Respond with:
- `y` or `yes`: Approve this tool execution
- `n` or `no`: Reject this tool execution
- `always`: Approve this tool and all future requests from this agent

---

## Tool Execution Flow

### With Default Approval Mode

1. Agent decides a tool is needed
2. Terminal displays tool approval prompt
3. User confirms or rejects
4. Tool executes (if approved) or error is returned to agent
5. Output is displayed according to `--tool-mode` setting

### With All Approval Mode

1. Agent decides a tool is needed
2. Tool executes immediately (no confirmation)
3. Output is displayed according to `--tool-mode` setting

---

## Command Reference

### `/approvemode <mode>`

Change the approval mode during a session.

```
/approvemode all
> Approval mode changed to: all (auto-approve all tools)

/approvemode default
> Approval mode changed to: default (prompt before execution)
```

**Available modes:** `all`, `default`

---

## Combination Examples

### Maximum Oversight (Development/Testing)

```bash
python -m terminal --tool-mode full
```

Then in terminal:
```
/approvemode default
```

This shows:
- All tool details before execution
- Prompts for confirmation on every tool
- Full output of tool results

---

### Production Automation

```bash
python -m terminal --tool-mode silent
```

Then in terminal:
```
/approvemode all
```

This shows:
- Minimal tool output (only results)
- Auto-executes all tools without prompting
- Good for batch processing or automated workflows

---

### Balanced Interactive Use

```bash
python -m terminal --tool-mode summary
```

Then in terminal:
```
/approvemode default
```

This shows (default behavior):
- Summary of tool execution
- Prompts for confirmation before each tool
- Clean, readable output

---

## Scope-Based Authorization

Tools can declare scopes that represent what permissions they need. The system supports transmitting scope information but does not currently enforce scope-based access control.

**Scopes are sent** from tools to terminal but **not currently enforced**.

---

## Implementation Notes

### Current Limitations

The current implementation:
- Does NOT have audit logging (no `memory/audit/tool_approvals.log`)
- Does NOT classify tools by risk level (no safe/warning/dangerous)
- Does NOT support per-tool approval policies
- Does NOT have whitelist/blacklist functionality
- Only supports prompt-based approval at terminal level

### How Approval Works

1. **Terminal sends request**: When agent requests tool execution, handler validates it
2. **User interaction**: Terminal prompts user (if in `default` mode)
3. **Tool execution**: If approved, tool runs as separate OS process
4. **Result handling**: Output returned to agent and displayed to user

### Architecture

- **Terminal** (`chat/terminal/terminal_gateway.py`): Handles user interaction and approval prompts
- **Handler** (`chat/handler.cpp`): Routes tool requests to terminal for approval
- **Bus**: Transmits approval decisions back to agent process

---

## Best Practices

### For Development

```bash
# Start in full mode to understand tool behavior
python -m terminal --tool-mode full

# Manually approve each tool to see what it does
# Keep /approvemode default
```

### For Batch Processing

```bash
# Auto-approve tools, show minimal output
python -m terminal --tool-mode silent

# Set approval mode to auto
/approvemode all
```

### For Security-Sensitive Operations

```bash
# Show full details and require manual approval
python -m terminal --tool-mode full
/approvemode default

# Manually review and approve each tool request
```

---

## Troubleshooting

### Tool Stuck Waiting for Approval

**Problem**: Terminal shows approval prompt but doesn't accept input

**Solution:**
- Ensure terminal is in focus and responsive
- Press `Enter` to submit response
- Check `/status` to verify system is responsive

---

### Approval Mode Not Changing

**Problem**: `/approvemode` command doesn't work

**Solution:**
- Verify syntax: `/approvemode all` or `/approvemode default`
- Check terminal is connected: `/status`
- Valid modes are only `all` or `default`

---

### Tools Auto-Executing When They Shouldn't

**Problem**: Tools run without prompts even though you wanted to review them

**Solution:**
- Check current approval mode: Look at terminal startup message
- Reset to manual approval: `/approvemode default`
- Verify `--tool-mode` flag at startup (controls output, not approval)

---

## See Also

- **[Terminal Commands Reference](TERMINAL.md)** - Complete command reference
- **[LLM Provider Setup](SETUP_LLM.md)** - Configure your LLM provider
- **[Storage Backends](STORAGE.md)** - Session storage options
