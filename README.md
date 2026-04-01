# Velix

Velix is a local-first, multi-process AI runtime that behaves like a small agent operating system.
It separates lifecycle control, LLM scheduling, tool execution, and IPC into dedicated services.

This README is the current operational reference for this repository.

## What Velix Runs

Core runtime services:
- Supervisor: lifecycle authority, process registration, heartbeats, limits, and authorization checks.
- Scheduler: LLM request queue, conversation hydration, fairness, and tool-call routing.
- Executioner: validated process launcher for skills and agents.
- Bus: PID-addressed IPC relay for results and lifecycle signals.
- Conversation Manager: user and process conversation storage.

SDKs:
- C++
- Python
- Node
- Go
- Rust

All SDKs use the same framed TCP JSON protocol:
- 4-byte big-endian length prefix
- UTF-8 JSON payload

## Repository Layout

- core/: supervisor and bus services.
- llm/: scheduler, conversation manager, and compacter.
- execution/: executioner and launch adapters.
- communication/: socket transport and framing helpers.
- runtime/sdk/: language SDK implementations.
- schema/: runtime message contracts.
- config/: ports and runtime tuning.
- skills/ and agents/: executable tools and orchestrators.
- tests/integration/: integration kernel and flow tests.

## Current Runtime Status

Implemented and actively used:
- Refactored Supervisor integrated as the canonical implementation in core/supervisor.cpp.
- ProcessRegistry and TerminationEngine wired into integration builds.
- Scheduler queue hardening and OpenAI-compatible base URL parsing.
- Cross-language SDK call_llm orchestration with iterative tool loop behavior.

Still intentionally minimal:
- core/handler.cpp is a placeholder in this repo.
- Integration flows use temporary handlers in tests/integration/ for runtime validation.

## Build and Integration

Integration build script:
- tests/integration/build_integration.sh

The script currently builds:
- build/tests/custom_verify_handler
- build/tests/integration_kernel

Integration kernel links Supervisor plus modular components:
- core/supervisor.cpp
- core/process_registry.cpp
- core/termination_engine.cpp
- core/bus.cpp
- execution/*
- llm/*

## Runtime Contract

Message types commonly exchanged:
- REGISTER_PID
- HEARTBEAT
- LLM_REQUEST
- EXEC_VELIX_PROCESS
- IPM_RELAY
- IPM_PUSH
- CHILD_TERMINATED

Execution context from environment:
- VELIX_PARENT_PID
- VELIX_TRACE_ID
- VELIX_PARAMS
- VELIX_PROCESS_NAME
- VELIX_TREE_ID

Default service ports (from config/ports.json unless overridden):
- SUPERVISOR: 5173
- LLM_SCHEDULER: 5171
- EXECUTIONER: 5172
- BUS: 5174

## SDK Behavior Parity

The SDK templates in runtime/sdk/ implement the same core lifecycle pattern:
- Register with Supervisor.
- Register on Bus and listen for PID-targeted replies.
- Send LLM requests through Scheduler.
- Execute tool calls through Executioner.
- Reinject tool outputs and continue the loop until final assistant response.

Current parity focus that has been implemented:
- Iterative call_llm loop with max-iteration guard.
- Support for both structured tool_calls and embedded EXEC ... END_EXEC blocks.
- Tool result reinjection as role tool messages with tool_call_id correlation.
- Consistent WAITING_LLM, RUNNING, and ERROR status transitions.

## Configuration Notes

Primary runtime configuration files:
- config/ports.json
- config/config.json
- config/model.json
- config/supervisor.json

Runtime components support fallback lookup paths used by test and build layouts.
