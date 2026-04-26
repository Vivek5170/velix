<span style="color:#0099ff">

```
██╗   ██╗███████╗██╗     ██╗██╗  ██╗
██║   ██║██╔════╝██║     ██║╚██╗██╔╝
██║   ██║█████╗  ██║     ██║ ╚███╔╝ 
╚██╗ ██╔╝██╔══╝  ██║     ██║ ██╔██╗ 
 ╚████╔╝ ███████╗███████╗██║██╔╝ ██╗
  ╚═══╝  ╚══════╝╚══════╝╚═╝╚═╝  ╚═╝
```

**The Agentic Operating System**

</span>

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Velix is a high-performance, local-first agentic runtime designed to solve the structural failures of modern LLM frameworks. It treats agents and tools as **independent processes**, orchestrated by a robust C++ kernel.

## Why Velix?

Traditional agentic systems (often Python-heavy) fail when moved from cloud playgrounds to production local hardware. Velix was built to solve the **"Unmanaged Reasoning Loop"**:

*   **Concurrency Crisis**: Local LLMs have finite resources. Without a **Scheduler**, multiple agent turns crash the memory or cause massive latency spikes. Velix enforces queuing, fairness, and history compaction at the kernel level.
*   **Orchestration Latency**: Every millisecond spent in Python orchestration is a millisecond lost in user experience. Velix uses an **asynchronous C++ backbone** to reach near-zero orchestration overhead.
*   **Fragility**: A single tool crash in a single-threaded framework kills the entire agent. Velix uses **Process-Level Isolation**; if a tool fails, the OS cleans it up and the Supervisor reports it—preserving the system's state.
*   **Privacy Sovereignty**: Built for **air-gapped and data-sensitive environments**, Velix ensures that all reasoning, history, and tool traces stay on your local infrastructure.

---

## ⚡ Quick Start (5 Minutes)

### 1. Build the kernel

From the project root:

```bash
cmake -S . -B build
cmake --build build -j
```

### 2. Configure your LLM provider

Edit `config/model.json` and ensure your adapter has **both** `base_url` and `chat_completions_path` set correctly (they combine to form the complete endpoint):

```json
{
  "active_adapter": "llama.cpp",
  "adapters": {
    "llama.cpp": {
      "api_style": "openai-chat-completions",
      "base_url": "http://127.0.0.1:8033/v1",
      "host": "127.0.0.1",
      "port": 8033,
      "use_https": false,
      "base_path": "/v1",
      "chat_completions_path": "/chat/completions",
      "model": "your-model-name",
      "enable_tools": true,
      "enable_streaming": true,
      "api_key": "",
      "api_key_env": "",
      "stop_tokens": []
    }
  }
}
```

**Key point**: `base_url` (`http://127.0.0.1:8033/v1`) + `chat_completions_path` (`/chat/completions`) = complete endpoint (`http://127.0.0.1:8033/v1/chat/completions`)

**📖 See [docs/SETUP_LLM.md](docs/SETUP_LLM.md) for detailed provider setup (llama.cpp, Ollama, OpenAI, Anthropic, etc.)**

### 3. Start the kernel and terminal

Terminal 1:
```bash
./build/integration_kernel
```

Terminal 2:
```bash
cd chat
python -m terminal
```

### 4. (Optional) Verify Python tools

Python-based tools use `uv` for isolation:

```bash
uv --version
```

**That's it!** You now have a running Velix instance. Type `/help` in the terminal for commands.

---

## 🧠 Core Concepts

### Skills
**Declarative configuration files** (`.md` markdown files) that describe tool capabilities without executing code. Skills define what a tool does, its inputs, outputs, and behavior contract.

**Example**: [`tools/session_search/skill.md`](tools/session_search/skill.md)

### Agents & Tools
**Independent OS processes** that extend the `VelixProcess` interface. They can be written in any language (Python, Go, Rust, Node.js, C++) and run isolated from one another.

- **Tools** are single-purpose workers (e.g., web search, database query)
- **Agents** are reasoning-capable processes that coordinate multiple tools

### Supervisor
The **lifecycle authority** that monitors process health, enforces timeouts, manages the execution tree, and **propagates errors gracefully** back to parent processes. One crashed tool does not kill the system.

### Bus
The **neural network** of Velix: a high-speed IPC relay that routes PID-targeted messages and system events between all processes.

---

## 🚀 Key Features

1. **Skills-Based Design** - Declarative markdown configs for tools; no code required to define tool behavior
2. **C++ Core** - Asynchronous C++ kernel reduces orchestration latency to near-zero
3. **Distributed** - Every component communicates via TCP/JSON; scale horizontally
4. **Multi-Process Stability** - One crash does not kill the system; OS-level isolation via processes
5. **Intelligent Scheduling** - Fair queue management for concurrent LLM requests with history compaction
6. **Infinite Context** - Kernel-level compaction engine + FTS5-indexed snapshots = search deep history
7. **Privacy-Preserving Search** - Tenant-isolated session search; data stays on your infrastructure
8. **Persistent Personas** - Multi-session branching with shared long-term memory per user
9. **Hardware Efficiency** - Minimal C++ core footprint leaves RAM for what matters: your LLM

---

## 🧩 Broad Model Support

Velix is built to be model-agnostic. Use high-performance local providers or enterprise cloud APIs:

*   **Local Performance**: Native support for `llama.cpp` and `Ollama`.
*   **OpenAI-Compatible**: Seamless integration with `vLLM`, `LM Studio`, and `TGI`.
*   **Enterprise Grade**: Built-in adapters for Anthropic and OpenAI.

---

## 🏗️ Architecture

Velix is composed of specialized service nodes:

*   **Supervisor**: The lifecycle authority. Monitors process health, enforces timeouts, and manages the execution tree.
*   **Scheduler**: The reasoning engine. Manages LLM queues and maintains conversation state across multi-turn reasoning loops.
*   **Executioner**: The deployment engine. Safely launches and configures tool/agent processes.
*   **Bus**: The neural network. A high-speed IPC relay for PID-targeted messages and system events.

---

## ✨ Advanced Features

*   **Non-Blocking Background Tools**: Velix supports asynchronous tool results. An agent can start a long-running "Build" or "Search" task and acknowledge it immediately, allowing the system to remain responsive while work happens in the background.
*   **Persona-Scoped Vaults**: Built-in support for persistent identity management. Memory is organized into `agentfiles/` for long-term facts and `sessions/` for active discourse.
*   **Language Agnostic**: Use the best tool for the job. Write high-speed data tools in Rust, web crawlers in Node.js, and reasoning agents in Python—all living in the same Velix execution tree.
*   **End-to-End Tracing**: Every tool call and reasoning turn is linked via a `trace_id`, allowing for deep debugging of complex multi-agent workflows.

---

## 📚 Documentation

### Setup & Configuration
- **[LLM Provider Setup](docs/SETUP_LLM.md)** - Configure llama.cpp, Ollama, OpenAI, Anthropic, or other providers
- **[Storage Backends](docs/STORAGE.md)** - Choose between JSON or SQLite for conversation storage
 - **Interactive Ask-User Flow** (see docs/TERMINAL.md) - ASK_USER_REQUEST / ASK_USER_REPLY protocol for user approvals and prompts

### Usage
- **[Terminal Commands](docs/TERMINAL.md)** - Full reference for `/help`, `/session_info`, `/new`, `/sessions`, `/delete`, `/undo`, and more
- **[C++ SDK Documentation](runtime/README.md)**
- **[Python SDK Documentation](runtime/sdk/python/README.md)**
- **[Node.js / Go / Rust SDKs](runtime/sdk/)**

---

## 📜 License

Velix is built for the future of agentic computing. See [LICENSE](LICENSE) for details.
