#ifndef VELIX_BUS_HPP
#define VELIX_BUS_HPP

namespace velix::core {

/**
 * The Velix Bus is a high-speed, protocol-aware message router.
 * It provides sequential, guaranteed delivery of IPC messages between
 * PID-identified processes.
 */
void start_bus(int port = 5174);
void stop_bus();

} // namespace velix::core

#endif // VELIX_BUS_HPP
