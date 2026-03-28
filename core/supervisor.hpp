#pragma once

namespace velix::core {

/**
 * Start Supervisor service on the given port (default 5173).
 * This call blocks and should be run in a background thread/process.
 */
void start_supervisor(int port = 5173);

/**
 * Stop Supervisor service gracefully.
 * Closes listening socket and joins watchdog thread.
 */
void stop_supervisor();

} // namespace velix::core
