#!/bin/bash
echo "Cleaning up Velix processes on macOS..."

# Find and kill processes holding ports 5170 to 5174
for port in {5170..5174}; do
    pid=$(lsof -ti tcp:$port)
    if [ ! -z "$pid" ]; then
        echo "Killing process on port $port (PID: $pid)"
        kill -9 $pid 2>/dev/null
    fi
done

# Kill integration_kernel, chat_handler, and any python processes running velix scripts
echo "Stopping velix-related binaries..."
pkill -9 -f "integration_kernel" 2>/dev/null
pkill -9 -f "chat_handler" 2>/dev/null
pkill -9 -f "velix" 2>/dev/null

echo "Cleanup complete."
