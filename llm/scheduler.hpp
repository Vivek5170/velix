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
 * - Detect EXEC blocks and return async handoff metadata to caller.
 * - Enforce EXEC availability only for conversation mode requests.
 *
 * This function blocks and runs the scheduler service loop.
 */
void start_scheduler(int port = 5171);

} // namespace velix::llm
