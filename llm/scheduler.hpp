#pragma once

#include <string>

namespace velix::llm {

/**
 * Start the scheduler server on the given port.
 *
 * Responsibilities:
 * - Accept LLM_REQUEST payloads.
 * - Queue by tree with per-tree FCFS.
 * - Allocate concurrent LLM execution keys (worker slots).
 * - Call provider exactly once per request.
 * - Inject default tool schemas when caller omits tools.
 * - Normalize provider tool calls into OpenAI-compatible tool_calls payloads.
 * - Return generalized assistant_message/tool_calls output for portable SDKs.
 * - Persist conversation assistant turns and enforce scheduler fairness rules.
 *
 * Non-responsibilities:
 * - Tool execution.
 * - Internal agent/tool iteration loops.
 *
 * Cross-SDK protocol (JSON-only):
 * - Input message_type: "LLM_REQUEST"
 * - Optional stream chunk message_type: "LLM_STREAM_CHUNK" with field "delta"
 * - Final message_type: "LLM_RESPONSE"
 *
 * Canonical objects in LLM_RESPONSE:
 * - ToolCall:
 *   {
 *     "id": string,
 *     "type": "function",
 *     "function": {"name": string, "arguments": object}
 *   }
 * - AssistantMessage:
 *   {
 *     "role": "assistant",
 *     "content": string,
 *     "tool_calls": ToolCall[]
 *   }
 * - ToolResultMessage (constructed by SDK):
 *   {
 *     "role": "tool",
 *     "tool_call_id": string,
 *     "content": string
 *   }
 *
 * This function blocks and runs the scheduler service loop.
 */
void start_scheduler(int port = 5171);

} // namespace velix::llm
