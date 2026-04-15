# Velix: The Agentic Operating System

Velix is a high-performance, local-first agentic runtime designed to solve the structural failures of modern LLM frameworks. It treats agents and tools as **independent processes**, orchestrated by a robust C++ kernel.

## Why Velix?

Traditional agentic systems (often Python-heavy) fail when moved from cloud playgrounds to production local hardware. Velix was built to solve the **"Unmanaged Reasoning Loop"**:

*   **Concurrency Crisis**: Local LLMs have finite resources. Without a **Scheduler**, multiple agent turns crash the memory or cause massive latency spikes. Velix enforces queuing, fairness, and history compaction at the kernel level.
*   **Orchestration Latency**: Every millisecond spent in Python orchestration is a millisecond lost in user experience. Velix uses an **asynchronous C++ backbone** to reach near-zero orchestration overhead.
*   **Fragility**: A single tool crash in a single-threaded framework kills the entire agent. Velix uses **Process-Level Isolation**; if a tool fails, the OS cleans it up and the Supervisor reports it—preserving the system's state.
*   **Privacy Sovereignty**: Built for **air-gapped and data-sensitive environments**, Velix ensures that all reasoning, history, and tool traces stay on your local infrastructure.

---

## 🚀 Key Features

### 1. Developer-First SDK (Not "Prompt-First")
Most frameworks rely on loose prompts to guide agent behavior. Velix provides the `VelixProcess` SDK (C++, Python, Node, Go, Rust) that enforces **predictable behavior contracts**. Developers write code to handle logic, while the LLM handles reasoning—creating a clear separation of concerns.

### 2. High-Performance C++ Core
While other systems spend significant fractions of a second just orchestrating Python objects, Velix uses an **asynchronous C++ kernel**. This reduces orchestration latency by orders of magnitude, allowing for real-time, fluid agent interactions and token streaming across multiple concurrent users.

### 3. Distributed by Design
Every component in Velix communicates via a **standardized TCP/JSON protocol**. 
*   Your **Scheduler** can run on a machine with a powerful GPU.
*   Your **Agents** can run on a cluster of lightweight workers.
*   Your **Gateway** can run on a public-facing edge server.
Everything is connected via a simple, PID-addressed Bus.

### 4. Multi-Process Stability & Isolation
In Velix, **one crash does not kill the system**. Each tool and agent is a separate OS process. 
*   **Isolation**: Python dependency conflicts between two different tools are impossible.
*   **Security**: Tools run in their own process space, managed by the **Supervisor**.
*   **Lifecycle**: The Supervisor monitors heartbeats, memory usage, and execution limits, automatically cleaning up "zombie" reasoning loops.

### 5. Intelligent LLM Scheduling
Local LLMs are easily overwhelmed by concurrent requests. The **Velix Scheduler** manages the reasoning queue. It ensures fairness, handles conversation hydration (history management), and optimizes request batches, preventing the "one-at-a-time" bottleneck of simple API wrappers.

### 6. Infinite Context & Intelligent Compaction
LLMs have fixed context limits. Velix solves this through a kernel-level **Compaction Engine**. When a conversation reaches its limit, the system:
1.  **Summarizes**: Generates a high-fidelity summary of the current state.
2.  **Snapshots**: Saves the full history as an FTS5-indexed JSON snapshot.
3.  **Resets**: Clears the live context and re-seeds it with the summary and a "retrieval trigger."
This allows agents to maintain "infinite" context without losing the ability to search deep history using the **session_search** skill.

### 7. Persistent Multi-User Personas
Unlike simple session-based bots, Velix distinguishes between **Personas** (Super-Users) and **Sessions**.
*   **Persistent Memory**: Every persona (e.g., `velix`) has a dedicated `memory/agentfiles/` directory for long-term facts (`user.md`) and core instructions (`soul.md`).
*   **Multi-Session Branching**: A single persona can maintain hundreds of independent sessions (`velix_s1`, `velix_s2`), all of which share the same underlying identity and long-term memory.
*   **Identity Isolation**: The kernel enforces strict identity mapping, ensuring that tools and agents always access the correct user's vault.

### 8. Hardware Efficiency
While modern agent frameworks can consume gigabytes of RAM just for their orchestration layer, the Velix C++ core (Supervisor, Bus, Scheduler) has a **minimal resource footprint**. This leaves your hardware's memory where it belongs: with the LLM.

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
*   **Executioner**: The deployment engine. Safely launches and configures skill/agent processes.
*   **Bus**: The neural network. A high-speed IPC relay for PID-targeted messages and system events.

---

## ✨ Advanced Features

*   **Non-Blocking Background Tools**: Velix supports asynchronous tool results. An agent can start a long-running "Build" or "Search" task and acknowledge it immediately, allowing the system to remain responsive while work happens in the background.
*   **Persona-Scoped Vaults**: Built-in support for persistent identity management. Memory is organized into `agentfiles/` for long-term facts and `sessions/` for active discourse.
*   **Language Agnostic**: Use the best tool for the job. Write high-speed data tools in Rust, web crawlers in Node.js, and reasoning agents in Python—all living in the same Velix execution tree.
*   **End-to-End Tracing**: Every tool call and reasoning turn is linked via a `trace_id`, allowing for deep debugging of complex multi-agent workflows.

---

## 🛠️ Getting Started

Velix provides SDKs for all major languages. Each SDK implements the same core lifecycle and reasoning patterns.

*   **[C++ SDK Documentation](runtime/README.md)**
*   **[Python SDK Documentation](runtime/sdk/python/README.md)**
*   **[Node.js / Go / Rust SDKs](runtime/sdk/)**

---

## 📜 License

Velix is built for the future of agentic computing. See [LICENSE](LICENSE) for details.

---

## How To Build And Use

### 1) Build

From the project root:

```bash
cmake -S . -B build
cmake --build build -j
```

This produces:

- `build/integration_kernel`
- `build/chat_handler`

### 2) Start the kernel and chat handler

Open separate terminals from the project root.

Terminal 1:

```bash
./build/integration_kernel
```

### 3) Python requirements for terminal client

The interactive terminal client requires Python 3.10+ and these packages:

```bash
python -m pip install --upgrade pip
python -m pip install prompt_toolkit rich
```

If you use Conda/Miniforge, install into the same environment where you run
`python -m terminal`.

### 4) Start the terminal gateway (Python client)

Use the terminal client from the `chat` directory (recommended):

```bash
cd chat
python -m terminal
```

Common options:

```bash
python -m terminal --user-id user1
python -m terminal --user-id user1_s2
python -m terminal --new
python -m terminal --tool-mode summary
python -m terminal --tool-mode silent
python -m terminal --no-stream
python -m terminal --help
```

### 5) Python skills require `uv`

Python-runtime skills (for example `skills/web_search`) are configured to run with `uv` (`uv run ...` in the skill manifest).

Install `uv` once on your machine, then run Velix normally:

```bash
uv --version
```

If `uv` is not installed, Python skills will fail to launch.

### 6) LLM provider config (`config/model.json`)

Velix reads the active provider from `active_adapter`.

#### Environment variables (file name + format)

- File name: `.env`
- File location: project root (same level as `README.md`)
- Format: one `KEY=VALUE` per line
- No JSON/YAML in `.env`; plain dotenv format only

Example:

```dotenv
OPENAI_API_KEY=sk-xxxxxxxx
ANTHROPIC_API_KEY=sk-ant-xxxxxxxx
OLLAMA_API_KEY=optional-if-your-ollama-endpoint-requires-it
```

If you set `api_key_env` for an adapter in `config/model.json`, Velix reads that env var name first. Fallback order is:

1. `api_key` value inside `config/model.json`
2. env var referenced by `api_key_env`
3. `OPENAI_API_KEY`
4. `OLLAMA_API_KEY`

#### Option A: `llama.cpp`

1. Start a llama.cpp server exposing OpenAI-compatible chat completions on port `8033`:

```bash
./llama-server -m /absolute/path/to/your-model.gguf --host 127.0.0.1 --port 8033
```

2. Keep this adapter active in `config/model.json`:

```json
{
	"active_adapter": "llama.cpp",
	"adapters": {
		"llama.cpp": {
			"base_url": "http://127.0.0.1:8033/v1",
			"chat_completions_path": "/chat/completions",
			"model": "your-model-name",
			"enable_tools": true,
			"enable_streaming": true
		}
	}
}
```

#### Option B: `ollama`

1. Start Ollama locally and pull a model:

```bash
ollama serve
ollama pull llama3
```

2. Switch `active_adapter` and set the model in `config/model.json`:

```json
{
	"active_adapter": "ollama",
	"adapters": {
		"ollama": {
			"base_url": "http://127.0.0.1:11434",
			"chat_completions_path": "/api/chat",
			"model": "llama3",
			"enable_tools": true,
			"enable_streaming": true
		}
	}
}
```

### 6) Other providers

If you are not using `llama.cpp` or `ollama`, you can keep default values and only change:

- `active_adapter`
- adapter `base_url` / endpoint path fields
- `model`
- `api_key_env` (recommended) or `api_key`

Recommended pattern for non-default providers:

1. Put the secret in `.env` as `YOUR_PROVIDER_KEY=...`
2. Set `api_key_env` for that adapter to `YOUR_PROVIDER_KEY`
3. Keep `api_key` empty in `config/model.json`

### 7) Conversation storage backends (JSON / SQLite)

Velix now uses a modular storage-provider architecture for conversations.

- Interface: `llm/storage/istorage_provider.hpp`
- Providers:
  - JSON provider: `llm/storage/json_storage_provider.cpp`
  - SQLite provider: `llm/storage/sqlite_storage_provider.cpp`
- Factory + DI wiring:
  - `llm/storage/provider_factory.cpp`
  - Injected into Scheduler and Supervisor `SessionIO` instances.

#### Current backend support

1. `json` backend (default)
   - Stores conversations under `memory/sessions/...` as JSON files.
   - Good for easy inspection and manual editing.

2. `sqlite` backend
   - Stores conversations in a SQLite DB file.
   - Default DB path in config: `.velix/velix.db`
   - Enabled with WAL and busy timeout for concurrent read/write behavior.

#### How to switch backend

Edit `config/storage.json`:

```json
{
  "backend": "json",
  "json_root": "memory/sessions",
  "sqlite_path": ".velix/velix.db"
}
```

Set `"backend": "sqlite"` to use SQLite.

#### Important modularity notes

- Business logic is now decoupled from storage writes/reads via
  `IStorageProvider` (repository-style boundary).
- `SessionIO` orchestrates behavior and delegates persistence to provider.
- Supervisor process-conversation cleanup path remains safe and now delegates
  to the same provider abstraction.
- `memory/agentfiles/*` (`soul.md`, `user.md`) intentionally remain file-based.

### 8) Session deletion command (`/delete`)

The chat handler now supports deleting a session by ID:

```text
/delete <session_id>
```

Example:

```text
/delete user1_s3
```

Behavior:

- Deletes the target session from session index and storage.
- If deleting the current active session, the handler automatically switches to
  a newly created fallback session for the same super-user.
- Refuses deletion when the target session is active on another socket.
