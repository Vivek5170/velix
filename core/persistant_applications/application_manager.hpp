#pragma once

/**
 * ApplicationManager — Velix External Process Service
 *
 * Owns all long-lived external process state that would bloat the Supervisor:
 *   • Terminal sessions (LocalPTY today, SSH/Docker/K8s later)
 *   • Job tracking for async command execution
 *   • Watchdog for idle/dead session cleanup
 *
 * Skills talk to this service, NOT to the Supervisor, for anything
 * involving file descriptors, PTYs, or sessions that outlive a single call.
 *
 * Wire protocol (TCP, same framing as Supervisor):
 *   Client sends one JSON request, receives one JSON response.
 *   For EXECUTE the response is immediate (job_id), output is retrieved
 *   by polling POLL until status != "running".
 *
 * Message types (request field "message_type"):
 *   EXECUTE       — lazy-create named session + submit command → { job_id }
 *   POLL          — fetch job output/status                   → { status, output, ... }
 *   KILL_SESSION  — terminate session(s)                      → { ok }
 *   LIST_SESSIONS — snapshot of all sessions or user sessions → { sessions: [...] }
 */

namespace velix::app_manager {

void start_application_manager(int port = 0);
void stop_application_manager();

} // namespace velix::app_manager