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

### 6. Hardware Efficiency
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
*   **Multi-Session Isolation**: Built-in support for `user_conversation` mode, enabling gateways to manage thousands of independent user sessions with zero cross-talk.
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
