import json
import os
import platform
import socket
import struct
import threading
import time
import uuid
from typing import Any, Dict, Optional


class VelixProcess:
    def __init__(self, name: str, role: str):
        self.process_name = name
        self.role = role
        self.os_pid = os.getpid()
        self.velix_pid = -1
        self.tree_id = ""
        self.parent_pid = int(os.getenv("VELIX_PARENT_PID", "-1"))
        self.entry_trace_id = os.getenv("VELIX_TRACE_ID", "")
        self.params = self._load_env_json("VELIX_PARAMS")
        self.is_running = False
        self.response_map: Dict[str, Any] = {}
        self.response_cv = threading.Condition()
        self._bus_sock: Optional[socket.socket] = None
        self._bus_thread: Optional[threading.Thread] = None
        self._hb_thread: Optional[threading.Thread] = None
        self.status = "STARTING"

    def run(self) -> None:
        raise NotImplementedError()

    def start(self) -> None:
        if self.is_running:
            return
        self.is_running = True

        reg = {
            "message_type": "REGISTER_PID",
            "payload": {
                "register_intent": "NEW_TREE" if self.parent_pid <= 0 else "JOIN_PARENT_TREE",
                "role": self.role,
                "os_pid": self.os_pid,
                "process_name": self.process_name,
                "trace_id": self.entry_trace_id,
                "status": "STARTING",
                "memory_mb": self._get_current_memory_usage_mb(),
            },
        }
        if self.parent_pid > 0:
            reg["source_pid"] = self.parent_pid

        reply = self._request("SUPERVISOR", reg, timeout_ms=5000)
        if reply.get("status") != "ok":
            raise RuntimeError(f"registration failed: {reply}")

        process_obj = reply.get("process", {})
        self.velix_pid = int(process_obj.get("pid", -1))
        self.tree_id = process_obj.get("tree_id", "")

        self._connect_bus()

        self._hb_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self._hb_thread.start()

        try:
            self.run()
        finally:
            self.shutdown()

    def shutdown(self) -> None:
        if not self.is_running:
            return
        self.is_running = False
        if self._bus_sock:
            try:
                self._bus_sock.close()
            except Exception:
                pass
            self._bus_sock = None

    def call_llm(self, convo_id: str, user_message: str = "", system_message: str = "") -> str:
        base_payload: Dict[str, Any] = {
            "message_type": "LLM_REQUEST",
            "mode": "conversation",
            "tree_id": self.tree_id,
            "source_pid": self.velix_pid,
            "priority": 1,
            "convo_id": convo_id,
            "owner_pid": self.velix_pid,
        }

        next_messages = []
        if system_message:
            next_messages.append({"role": "system", "content": system_message})
        if user_message:
            next_messages.append({"role": "user", "content": user_message})

        max_iterations = 10
        for _ in range(max_iterations):
            self.status = "WAITING_LLM"

            payload = dict(base_payload)
            payload["request_id"] = f"req_{self.velix_pid}_{uuid.uuid4().hex[:8]}"
            payload["trace_id"] = uuid.uuid4().hex
            if next_messages:
                payload["messages"] = list(next_messages)

            resp = self._request("LLM_SCHEDULER", payload, timeout_ms=120000)
            if resp.get("status") != "ok":
                self.status = "ERROR"
                raise RuntimeError(resp.get("error", "llm request failed"))

            if not bool(resp.get("exec_required", False)):
                self.status = "RUNNING"
                return str(resp.get("response", ""))

            trace_id = str(resp.get("trace_id", ""))
            tool_messages = []
            tool_executed = False

            for tool_call in self._extract_tool_calls(resp):
                name = str(tool_call.get("name", "")).strip()
                if not name:
                    continue
                args = tool_call.get("arguments", {})
                if not isinstance(args, dict):
                    args = {}

                self.status = "RUNNING"
                result = self.execute_tool(name, args)
                tool_executed = True
                tool_messages.append(
                    {
                        "role": "tool",
                        "content": json.dumps(result),
                        "tool_call_id": trace_id,
                    }
                )

            if not tool_executed:
                self.status = "RUNNING"
                return str(resp.get("response", ""))

            # Scheduler owns historical context; only send newly generated
            # tool messages on next pass.
            next_messages = tool_messages

        self.status = "ERROR"
        return "Failure: Agent state machine exceeded max iterations."

    def _extract_tool_calls(self, scheduler_reply: Dict[str, Any]) -> list:
        tool_calls = []

        structured = scheduler_reply.get("tool_calls", [])
        if isinstance(structured, list):
            for call in structured:
                if isinstance(call, dict):
                    tool_calls.append(call)

        response_text = str(scheduler_reply.get("response", ""))
        search_at = 0
        while True:
            start = response_text.find("EXEC", search_at)
            if start == -1:
                break
            end = response_text.find("END_EXEC", start)
            if end == -1:
                break
            raw = response_text[start + 4:end].strip()
            if raw:
                try:
                    parsed = json.loads(raw)
                    if isinstance(parsed, dict):
                        tool_calls.append(parsed)
                except Exception:
                    pass
            search_at = end + len("END_EXEC")

        return tool_calls

    def execute_tool(self, name: str, params: Dict[str, Any]) -> Dict[str, Any]:
        trace_id = uuid.uuid4().hex
        req = {
            "message_type": "EXEC_VELIX_PROCESS",
            "trace_id": trace_id,
            "tree_id": self.tree_id,
            "source_pid": self.velix_pid,
            "name": name,
            "params": params,
        }
        ack = self._request("EXECUTIONER", req, timeout_ms=5000)
        if ack.get("status") != "ok":
            raise RuntimeError(ack.get("error", "executioner rejected"))

        with self.response_cv:
            ok = self.response_cv.wait_for(lambda: trace_id in self.response_map, timeout=60 * 60)
            if not ok:
                raise TimeoutError("tool result timeout")
            payload = self.response_map.pop(trace_id)
            if isinstance(payload, dict) and payload.get("error") == "child_terminated":
                raise RuntimeError(f"child terminated: {payload}")
            return payload if isinstance(payload, dict) else {"status": "ok", "payload": payload}

    def report_result(self, target_pid: int, data: Dict[str, Any], trace_id: str = "") -> None:
        if not self._bus_sock:
            return
        msg = {
            "message_type": "IPM_RELAY",
            "target_pid": target_pid,
            "trace_id": trace_id,
            "payload": data,
        }
        self._send_framed(self._bus_sock, msg)

    def _connect_bus(self) -> None:
        bus_port = self._get_port("BUS", 5174)
        sock = self._connect("127.0.0.1", bus_port, timeout_ms=5000)
        self._send_framed(sock, {"message_type": "BUS_REGISTER", "pid": self.velix_pid})
        ack = self._recv_framed(sock)
        if ack.get("status") != "ok":
            sock.close()
            raise RuntimeError(f"bus register failed: {ack}")
        self._bus_sock = sock
        self._bus_thread = threading.Thread(target=self._bus_listener_loop, daemon=True)
        self._bus_thread.start()

    def _bus_listener_loop(self) -> None:
        if not self._bus_sock:
            return
        while self.is_running:
            try:
                msg = self._recv_framed(self._bus_sock)
                if msg.get("message_type") in ("IPM_PUSH", "CHILD_TERMINATED"):
                    trace_id = msg.get("trace_id", "")
                    if trace_id:
                        with self.response_cv:
                            self.response_map[trace_id] = msg.get("payload", {})
                            self.response_cv.notify_all()
            except Exception:
                break

    def _heartbeat_loop(self) -> None:
        while self.is_running:
            try:
                hb = {
                    "message_type": "HEARTBEAT",
                    "pid": self.velix_pid,
                    "payload": {
                        "status": "RUNNING",
                        "memory_mb": self._get_current_memory_usage_mb(),
                    },
                }
                _ = self._request("SUPERVISOR", hb, timeout_ms=3000)
            except Exception:
                pass
            time.sleep(5)

    @staticmethod
    def _get_current_memory_usage_mb() -> float:
        # Keep this dependency-free; return 0.0 on unsupported platforms.
        try:
            if platform.system() == "Linux":
                with open("/proc/self/statm", "r", encoding="utf-8") as f:
                    parts = f.read().strip().split()
                if len(parts) >= 2:
                    rss_pages = int(parts[1])
                    page_size = os.sysconf("SC_PAGE_SIZE")
                    return float(rss_pages * page_size) / (1024.0 * 1024.0)
        except Exception:
            pass
        return 0.0

    def _request(self, service_key: str, payload: Dict[str, Any], timeout_ms: int) -> Dict[str, Any]:
        port = self._get_port(service_key, 0)
        if port <= 0:
            raise RuntimeError(f"invalid port for {service_key}")
        sock = self._connect("127.0.0.1", port, timeout_ms)
        try:
            self._send_framed(sock, payload)
            return self._recv_framed(sock)
        finally:
            sock.close()

    @staticmethod
    def _connect(host: str, port: int, timeout_ms: int) -> socket.socket:
        sock = socket.create_connection((host, port), timeout=timeout_ms / 1000.0)
        sock.settimeout(timeout_ms / 1000.0)
        return sock

    @staticmethod
    def _send_framed(sock: socket.socket, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        sock.sendall(struct.pack(">I", len(body)) + body)

    @staticmethod
    def _recv_framed(sock: socket.socket) -> Dict[str, Any]:
        header = VelixProcess._recv_exact(sock, 4)
        size = struct.unpack(">I", header)[0]
        body = VelixProcess._recv_exact(sock, size)
        return json.loads(body.decode("utf-8"))

    @staticmethod
    def _recv_exact(sock: socket.socket, n: int) -> bytes:
        data = bytearray()
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                raise RuntimeError("socket closed")
            data.extend(chunk)
        return bytes(data)

    @staticmethod
    def _load_env_json(key: str) -> Dict[str, Any]:
        raw = os.getenv(key, "")
        if not raw:
            return {}
        try:
            value = json.loads(raw)
            return value if isinstance(value, dict) else {}
        except Exception:
            return {}

    @staticmethod
    def _get_port(service_name: str, fallback: int) -> int:
        for path in ("config/ports.json", "../config/ports.json", "build/config/ports.json"):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    ports = json.load(f)
                    return int(ports.get(service_name, fallback))
            except Exception:
                continue
        return fallback
