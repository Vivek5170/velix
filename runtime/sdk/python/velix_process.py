import json
import os
import platform
import signal
import socket
import struct
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Callable, Dict, List, Optional, Union


class VelixProcess:
    # Class-level singleton for signal handlers — mirrors static VelixProcess* instance_
    _instance: Optional["VelixProcess"] = None

    def __init__(self, name: str, role: str) -> None:
        self.process_name = name
        self.role = role
        self.os_pid = os.getpid()

        # Runtime identity — assigned by Supervisor on REGISTER_PID
        self.velix_pid: int = -1
        self.tree_id: str = ""
        self.is_root: bool = False
        self.is_handler: bool = False

        # Environment — injected by Executioner on spawn
        self.parent_pid: int = int(os.getenv("VELIX_PARENT_PID", "-1"))
        self.entry_trace_id: str = os.getenv("VELIX_TRACE_ID", "")
        self.user_id: str = os.getenv("VELIX_USER_ID", "")
        self.params: Dict[str, Any] = self._load_env_json("VELIX_PARAMS")

        # Global Supervisor Limits (synced from REGISTER_PID response)
        self.max_memory_mb: int = 0
        self.max_runtime_sec: int = 0

        # Lifecycle flags
        self.is_running: bool = False
        self.force_terminate: bool = False
        self.forced_by_signal: bool = False
        self.result_reported: bool = False

        # Async result waiting — mirrors queue_mutex / queue_cv / response_map / pending_response_traces
        self._response_map: Dict[str, Any] = {}
        self._response_cv: threading.Condition = threading.Condition()
        self._pending_response_traces: set = set()

        # Bus socket and threads
        self._bus_sock: Optional[socket.socket] = None
        self._bus_lock: threading.Lock = threading.Lock()
        self._bus_thread: Optional[threading.Thread] = None
        self._hb_thread: Optional[threading.Thread] = None

        # Process status — mirrors std::atomic<ProcessStatus> status
        self.__status: str = "STARTING"

        # Developer-assignable hooks — mirrors std::function members
        self.on_tool_start: Optional[Callable[[str, Dict[str, Any]], None]] = None
        self.on_tool_finish: Optional[Callable[[str, Dict[str, Any]], None]] = None
        self.on_bus_event: Optional[Callable[[Dict[str, Any]], None]] = None

    # -------------------------------------------------------------------------
    # Developer override points
    # -------------------------------------------------------------------------

    def run(self) -> None:
        """Main logic entrypoint. Must be overridden by every process subclass."""
        raise NotImplementedError()

    def on_shutdown(self) -> None:
        """
        Override to close custom resources before the runtime tears down sockets.
        Called during shutdown() before threads and sockets are released.
        """

    # -------------------------------------------------------------------------
    # Lifecycle
    # -------------------------------------------------------------------------

    def start(self, override_pid: int = -1, parent_tree_id: str = "") -> None:
        if self.is_running:
            return

        self.is_running = True
        self.force_terminate = False
        self.forced_by_signal = False

        # Register as global signal target
        VelixProcess._instance = self
        signal.signal(signal.SIGTERM, _posix_signal_handler)
        signal.signal(signal.SIGINT, _posix_signal_handler)

        # Resolve launch intent from environment
        launch_intent = os.getenv("VELIX_INTENT", "")
        launch_intent = _validate_launch_intent(self.role, self.parent_pid, launch_intent)
        self.is_root = (launch_intent == "NEW_TREE")

        sup_port = self._resolve_port("SUPERVISOR", 5173)
        retry_limit = _get_config("SDK_RETRY_LIMIT", 3)
        retry_delay = _get_config("SDK_RETRY_DELAY_MS", 500)

        # REGISTER_PID with Supervisor
        reg_msg: Dict[str, Any] = {
            "message_type": "REGISTER_PID",
            "payload": {
                "register_intent": launch_intent,
                "role": self.role,
                "os_pid": self.os_pid,
                "process_name": self.process_name,
                "trace_id": self.entry_trace_id,
                "status": "STARTING",
                "memory_mb": self._get_current_memory_usage_mb(),
            },
        }
        if launch_intent == "JOIN_PARENT_TREE" and self.parent_pid > 0:
            reg_msg["source_pid"] = self.parent_pid

        reply = self._request_with_retries("SUPERVISOR", sup_port, reg_msg, retry_limit, retry_delay)
        if reply.get("status") != "ok":
            raise RuntimeError(
                "VelixProcess supervisor registration failed: " + reply.get("error", "unknown")
            )

        process_obj = reply.get("process", {})
        self.velix_pid = int(process_obj.get("pid", -1))
        self.tree_id = str(process_obj.get("tree_id", "UNKNOWN"))
        if "is_root" in reply:
            self.is_root = bool(reply["is_root"])
        if "is_handler" in reply:
            self.is_handler = bool(reply["is_handler"])
        self.max_memory_mb = int(reply.get("max_memory_mb", 2048))
        self.max_runtime_sec = int(reply.get("max_runtime_sec", 300))

        # Connect to Bus
        bus_port = self._resolve_port("BUS", 5174)
        try:
            self._connect_bus(bus_port, retry_limit, retry_delay)
        except Exception as exc:
            _log_warn(f"Failed to connect to Velix Bus: {exc}")

        _log_info(
            f"Velix Node Started | PID: {self.os_pid} -> {self.velix_pid} | Role: {self.role}"
            + (" (ROOT)" if self.is_root else "")
            + f" | Parent PID: {self.parent_pid} | Launch Intent: {launch_intent}"
        )

        # Spawn heartbeat IO thread
        self._hb_thread = threading.Thread(target=self._run_kernel_io_loop, daemon=True)
        self._hb_thread.start()

        # Run developer logic with ResultGuard
        with _ResultGuard(self):
            try:
                self.run()
            except Exception as exc:
                _log_error(f"VelixProcess Sandbox Crashed: {exc}")

        self.shutdown()

    def shutdown(self) -> None:
        if not self.is_running:
            return

        self.is_running = False
        self.force_terminate = True

        try:
            self.on_shutdown()
        except Exception as exc:
            _log_warn(f"VelixProcess on_shutdown() failed: {exc}")

        # Wake any threads blocked on bus wait
        with self._response_cv:
            self._response_cv.notify_all()

        if self._bus_sock:
            try:
                self._bus_sock.close()
            except Exception:
                pass
            self._bus_sock = None

        if self._hb_thread and self._hb_thread.is_alive():
            self._hb_thread.join(timeout=10)
        if self._bus_thread and self._bus_thread.is_alive():
            self._bus_thread.join(timeout=5)

    def request_forced_shutdown(self) -> None:
        """Signal-safe entrypoint: mark forced termination then shutdown."""
        self.forced_by_signal = True
        self.force_terminate = True
        self.shutdown()

    # -------------------------------------------------------------------------
    # Public SDK API
    # -------------------------------------------------------------------------

    def call_llm(
        self,
        convo_id: str,
        user_message: str = "",
        system_message: str = "",
        user_id: str = "",
        mode: str = "",
    ) -> str:
        return self._call_llm_internal(
            convo_id=convo_id,
            user_message=user_message,
            system_message=system_message,
            user_id=user_id,
            mode=mode,
            stream_requested=False,
            on_token=None,
            intent_override=None,
        )

    def call_llm_stream(
        self,
        convo_id: str,
        user_message: str,
        on_token: Callable[[str], None],
        system_message: str = "",
        user_id: str = "",
        mode: str = "",
    ) -> str:
        return self._call_llm_internal(
            convo_id=convo_id,
            user_message=user_message,
            system_message=system_message,
            user_id=user_id,
            mode=mode,
            stream_requested=True,
            on_token=on_token,
            intent_override=None,
        )

    def execute_tool(self, instruction: str, args: Dict[str, Any]) -> Dict[str, Any]:
        return self._execute_tool_internal(
            instruction=instruction,
            args=args,
            user_id_override=None,
            intent_override=None,
        )

    def send_message(self, target_pid: int, purpose: str, payload: Dict[str, Any]) -> None:
        if not self._bus_sock:
            return
        relay: Dict[str, Any] = {
            "message_type": "IPM_RELAY",
            "target_pid": target_pid,
            "purpose": purpose,
            "user_id": self.user_id,
            "payload": payload,
        }
        with self._bus_lock:
            self._send_framed(self._bus_sock, relay)

    # -------------------------------------------------------------------------
    # Internal: LLM orchestration
    # -------------------------------------------------------------------------

    def call_llm_resume(
        self,
        convo_id: str,
        tool_result: Dict[str, Any],
        user_id: str = "",
        on_token: Optional[Callable[[str], None]] = None,
    ) -> str:
        """Resumes a conversation with an out-of-band tool result."""
        return self._call_llm_internal(
            convo_id, "", "", user_id, "", True, on_token, None, tool_result
        )

    def _call_llm_internal(
        self,
        convo_id: str,
        user_message: str,
        system_message: str = "",
        user_id: str = "",
        mode: str = "",
        stream_requested: bool = False,
        on_token: Optional[Callable[[str], None]] = None,
        intent_override: Optional[str] = None,
        tool_result_override: Optional[Union[Dict[str, Any], List[Dict[str, Any]]]] = None,
    ) -> str:
        """Core LLM reasoning loop. Handles tool calls internally."""
        self._status = "WAITING_LLM"

        # Resolve per-call user context without mutating shared process state
        effective_user_id = self.user_id if not user_id else user_id
        effective_intent_override: Optional[str] = None
        if (
            self.is_handler
            and intent_override is not None
            and _is_supported_intent_value(intent_override)
        ):
            effective_intent_override = intent_override

        effective_mode = _resolve_effective_mode(mode, convo_id, effective_user_id, self.is_handler)

        active_convo_id = convo_id
        base_payload: Dict[str, Any] = {
            "mode": effective_mode,
            "owner_pid": self.velix_pid,
            "stream": stream_requested,
        }

        if effective_mode != "simple":
            base_payload["user_id"] = effective_user_id

        pending_user_message = user_message
        pending_system_message = system_message
        pending_tool_messages: list = []
        if tool_result_override is not None:
            if isinstance(tool_result_override, list):
                pending_tool_messages = list(tool_result_override)
            else:
                pending_tool_messages = [tool_result_override]

        sched_port = self._resolve_port("SCHEDULER", 5171)
        retry_limit = _get_config("SDK_RETRY_LIMIT", 3)
        retry_delay = _get_config("SDK_RETRY_DELAY_MS", 500)
        llm_timeout_ms = _get_config("SDK_LLM_TIMEOUT_MS", 305000)
        max_iterations = _get_config("SDK_MAX_ITERATIONS", 100)

        loop_count = 0
        while self.is_running and loop_count < max_iterations:
            payload = dict(base_payload)
            payload["convo_id"] = active_convo_id
            payload["message_type"] = "LLM_REQUEST"
            payload["request_id"] = f"req_{self.velix_pid}_{uuid.uuid4().hex[:8]}"
            payload["trace_id"] = uuid.uuid4().hex
            payload["tree_id"] = self.tree_id
            payload["source_pid"] = self.velix_pid
            payload["priority"] = payload.get("priority", 1)

            if pending_system_message:
                payload["system_message"] = pending_system_message
            if pending_user_message:
                payload["user_message"] = pending_user_message

            # Honour stream_requested on tool-resume turns so the next assistant
            # reply still streams token-by-token. The old `stream=False` caused
            # the model's full response to be buffered silently after every tool call.
            if pending_tool_messages:
                payload["stream"] = stream_requested
                if len(pending_tool_messages) == 1:
                    payload["tool_message"] = pending_tool_messages[0]
                else:
                    payload["tool_messages"] = pending_tool_messages
            else:
                payload["stream"] = stream_requested

            pending_system_message = ""
            pending_user_message = ""
            pending_tool_messages = []

            request_streaming = bool(payload.get("stream", False))
            raw_reply = self._dispatch_llm_request(
                sched_port, payload, request_streaming, on_token,
                retry_limit, retry_delay, llm_timeout_ms,
            )
            reply: Dict[str, Any] = json.loads(raw_reply)

            if reply.get("status", "error") != "ok":
                self._status = "ERROR"
                raise RuntimeError("Scheduler rejected: " + reply.get("error", ""))

            # Correctly update convo_id for the next turn loop — mirrors C++ active_convo_id update
            returned_convo_id = reply.get("convo_id")
            if isinstance(returned_convo_id, str) and returned_convo_id:
                active_convo_id = returned_convo_id

            response_text: str = reply.get("response", "")

            # Normalize tool_calls — must be a list of objects with function.name / function.arguments / id
            raw_tool_calls = reply.get("tool_calls", [])
            normalized_tool_calls: list = (
                raw_tool_calls if isinstance(raw_tool_calls, list) else []
            )

            if not normalized_tool_calls:
                self._status = "RUNNING"
                return response_text

            # Protocol guard — mirrors C++ assistant_message check
            assistant_message = reply.get("assistant_message")
            if not isinstance(assistant_message, dict):
                self._status = "ERROR"
                raise RuntimeError(
                    "Scheduler protocol violation: missing assistant_message object"
                )

            self._status = "RUNNING"

            # Execute all tool calls in parallel — mirrors std::async launch::async
            tool_messages: list = []
            with ThreadPoolExecutor() as executor:
                futures = {
                    executor.submit(
                        self._execute_single_tool_call,
                        tool_call,
                        effective_user_id,
                        effective_intent_override,
                    ): tool_call
                    for tool_call in normalized_tool_calls
                }
                for future in as_completed(futures):
                    try:
                        tool_messages.append(future.result())
                    except RuntimeError:
                        self._status = "ERROR"
                        raise
                    except Exception as exc:
                        _log_warn(f"Failed to execute structured tool call: {exc}")

            if not tool_messages:
                self._status = "RUNNING"
                return response_text

            pending_tool_messages = tool_messages
            loop_count += 1
            self._status = "WAITING_LLM"

        self._status = "ERROR"
        return f"Failure: Agent state machine exceeded max {max_iterations} iterations."

    def _execute_single_tool_call(
        self,
        tool_call: Any,
        effective_user_id: str,
        effective_intent_override: Optional[str],
    ) -> Dict[str, Any]:
        """Mirrors the std::async lambda inside call_llm_internal."""
        if not isinstance(tool_call, dict):
            raise RuntimeError("Malformed tool_call: expected object")

        fn = tool_call.get("function", {})
        tool_name: str = fn.get("name", "") if isinstance(fn, dict) else ""
        if not tool_name:
            raise RuntimeError("Malformed tool_call: missing function.name")

        tool_args: Dict[str, Any] = fn.get("arguments", {}) if isinstance(fn, dict) else {}
        if not isinstance(tool_args, dict):
            tool_args = {}

        tool_call_id: str = tool_call.get("id", "")
        if not tool_call_id:
            raise RuntimeError("Malformed tool_call: missing id")

        try:
            tool_res = self._execute_tool_internal(
                instruction=tool_name,
                args=tool_args,
                user_id_override=effective_user_id if effective_user_id else None,
                intent_override=effective_intent_override,
            )
        except Exception as exc:
            tool_res = {
                "status": "error",
                "error": "tool_execution_failed",
                "message": str(exc),
                "tool": tool_name,
            }

        return {
            "role": "tool",
            "tool_call_id": tool_call_id,
            "content": json.dumps(tool_res),
        }

    def _dispatch_llm_request(
        self,
        sched_port: int,
        payload: Dict[str, Any],
        request_streaming: bool,
        on_token: Optional[Callable[[str], None]],
        retry_limit: int,
        retry_delay: int,
        llm_timeout_ms: int,
    ) -> str:
        """Open a fresh scheduler connection, send the envelope, receive the response."""
        try:
            # Connect timeout uses the full LLM deadline for the initial TCP handshake.
            sock = self._connect_with_retries("127.0.0.1", sched_port, llm_timeout_ms, retry_limit, retry_delay)
        except Exception:
            raise RuntimeError("VelixProcess SDK: Failed to connect to scheduler after retry attempts.")

        try:
            self._send_framed(sock, payload)
            # Use a short per-read poll timeout for streaming so a stalled chunk
            # is retried quickly. The stream_deadline inside
            # _receive_scheduler_response() still caps the overall wall-clock time.
            stream_poll_timeout_ms = _get_config("SDK_STREAM_POLL_TIMEOUT_MS", 30000)
            if request_streaming:
                sock.settimeout(stream_poll_timeout_ms / 1000.0)
            return self._receive_scheduler_response(sock, request_streaming, llm_timeout_ms, on_token)
        finally:
            sock.close()

    def _receive_scheduler_response(
        self,
        sock: socket.socket,
        request_streaming: bool,
        llm_timeout_ms: int,
        on_token: Optional[Callable[[str], None]],
    ) -> str:
        """Mirrors receive_scheduler_response in the C++ anonymous namespace."""
        if not request_streaming:
            return self._recv_framed_raw(sock)

        max_chunks = _get_config("SDK_STREAM_MAX_CHUNKS", 100000)
        chunk_count = 0
        deadline = time.monotonic() + (llm_timeout_ms + 10000) / 1000.0

        while chunk_count < max_chunks:
            chunk_count += 1

            if not self.is_running:
                raise RuntimeError("process shutdown during streaming")
            if time.monotonic() > deadline:
                raise RuntimeError("scheduler stream deadline exceeded")

            try:
                raw = self._recv_framed_raw(sock)
            except (TimeoutError, OSError) as exc:
                if _is_transient_socket_error(str(exc)):
                    continue
                raise

            try:
                message: Dict[str, Any] = json.loads(raw)
            except Exception:
                continue

            if message.get("message_type") == "LLM_STREAM_CHUNK":
                delta: str = message.get("delta", "")
                if delta and on_token is not None:
                    on_token(delta)
                continue

            return raw

        raise RuntimeError("scheduler stream chunk limit exceeded")

    # -------------------------------------------------------------------------
    # Internal: Tool execution
    # -------------------------------------------------------------------------

    def _execute_tool_internal(
        self,
        instruction: str,
        args: Dict[str, Any],
        user_id_override: Optional[str],
        intent_override: Optional[str],
    ) -> Dict[str, Any]:
        actual_trace: str = ""
        pending_trace_registered: bool = False

        def clear_pending_trace() -> None:
            nonlocal pending_trace_registered
            if not pending_trace_registered or not actual_trace:
                return
            with self._response_cv:
                self._pending_response_traces.discard(actual_trace)
            pending_trace_registered = False

        try:
            effective_user_id = self.user_id
            if self.is_handler and user_id_override:
                effective_user_id = user_id_override

            def call_on_tool_finish(tool_result: Dict[str, Any]) -> None:
                if self.on_tool_finish is None:
                    return
                try:
                    hook_result = dict(tool_result) if isinstance(tool_result, dict) else {}
                    if effective_user_id and "user_id" not in hook_result:
                        hook_result["user_id"] = effective_user_id
                    self.on_tool_finish(instruction, hook_result)
                except Exception as exc:
                    _log_warn(f"on_tool_finish hook failed: {exc}")

            if self.on_tool_start is not None:
                try:
                    hook_args = dict(args) if isinstance(args, dict) else {}
                    if effective_user_id and "user_id" not in hook_args:
                        hook_args["user_id"] = effective_user_id
                    self.on_tool_start(instruction, hook_args)
                except Exception as exc:
                    _log_warn(f"on_tool_start hook failed: {exc}")

            actual_trace = uuid.uuid4().hex

            with self._response_cv:
                self._pending_response_traces.add(actual_trace)
            pending_trace_registered = True

            effective_intent = "JOIN_PARENT_TREE"
            if (
                self.is_handler
                and intent_override is not None
                and _is_supported_intent_value(intent_override)
            ):
                effective_intent = intent_override

            launch_req: Dict[str, Any] = {
                "message_type": "EXEC_VELIX_PROCESS",
                "trace_id": actual_trace,
                "tree_id": self.tree_id,
                "source_pid": self.velix_pid,
                "is_handler": self.is_handler,
                "name": instruction,
                "params": args,
                "intent": effective_intent,
            }
            if effective_user_id:
                launch_req["user_id"] = effective_user_id

            exec_port = self._resolve_port("EXECUTIONER", 5172)
            exec_timeout_ms = _get_config("SDK_EXEC_TIMEOUT_MS", 120000)
            exec_retry_limit = _get_config("SDK_EXEC_RETRY_LIMIT", 3)
            exec_retry_delay = _get_config("SDK_EXEC_RETRY_DELAY_MS", 300)
            connect_retry_limit = _get_config("SDK_RETRY_LIMIT", 3)
            connect_retry_delay = _get_config("SDK_RETRY_DELAY_MS", 500)

            last_exec_error = "unknown"
            launch_acked = False

            for attempt in range(max(1, exec_retry_limit)):
                try:
                    exec_sock = self._connect_with_retries(
                        "127.0.0.1", exec_port, exec_timeout_ms,
                        connect_retry_limit, connect_retry_delay,
                    )
                    try:
                        self._send_framed(exec_sock, launch_req)
                        ack = self._receive_executioner_ack(exec_sock, exec_timeout_ms)
                    finally:
                        exec_sock.close()

                    if ack.get("status") == "ok":
                        launch_acked = True
                        break

                    launcher_error = ack.get("message", ack.get("error", "unknown rejection"))
                    last_exec_error = "Velix Launcher Failure: " + launcher_error

                    if "busy" in launcher_error and attempt + 1 < exec_retry_limit:
                        time.sleep(exec_retry_delay * (attempt + 1) / 1000.0)
                        continue

                    raise RuntimeError(last_exec_error)

                except Exception as exc:
                    last_exec_error = str(exc)
                    retryable = (
                        _is_transient_socket_error(last_exec_error)
                        or "executioner ack deadline exceeded" in last_exec_error
                        or "busy" in last_exec_error
                    )
                    if attempt + 1 < exec_retry_limit and retryable:
                        time.sleep(exec_retry_delay * (attempt + 1) / 1000.0)
                        continue
                    break

            if not launch_acked:
                if "Velix Launcher Failure" in last_exec_error:
                    raise RuntimeError(last_exec_error)
                raise RuntimeError("Velix Executioner Link Failed: " + last_exec_error)

            # Phase 2: Reactive wait on the Velix Bus for the actual tool output
            bus_wait_min = _get_config("SDK_BUS_WAIT_MIN", 60)
            with self._response_cv:
                success = self._response_cv.wait_for(
                    lambda: actual_trace in self._response_map,
                    timeout=bus_wait_min * 60,
                )
                if not success:
                    self._pending_response_traces.discard(actual_trace)
                    pending_trace_registered = False
                    raise TimeoutError(
                        f"Velix Package Timeout: {instruction} exceeded reactive limit of "
                        f"{bus_wait_min} mins"
                    )

                result: Dict[str, Any] = self._response_map.pop(actual_trace)
                self._pending_response_traces.discard(actual_trace)
                pending_trace_registered = False

                # Reactive fail-fast for terminal kernel events
                if result.get("status") == "error" and result.get("error") == "child_terminated":
                    reason = result.get("reason", "unknown_termination")
                    raise RuntimeError(
                        f"Velix Tool Crash: {instruction} terminated by Supervisor "
                        f"(Reason: {reason})"
                    )

            call_on_tool_finish(result)
            return result

        except Exception as exc:
            clear_pending_trace()
            error_payload: Dict[str, Any] = {"status": "error", "message": str(exc)}
            call_on_tool_finish(error_payload)
            raise

    def _receive_executioner_ack(
        self, sock: socket.socket, exec_timeout_ms: int
    ) -> Dict[str, Any]:
        """Mirrors receive_executioner_ack in the C++ anonymous namespace."""
        poll_timeout_s = max(250, min(exec_timeout_ms, 2000)) / 1000.0
        sock.settimeout(poll_timeout_s)
        deadline = time.monotonic() + (exec_timeout_ms + 2000) / 1000.0

        while time.monotonic() < deadline:
            if not self.is_running:
                raise RuntimeError("process shutdown during executioner ack wait")
            try:
                raw = self._recv_framed_raw(sock)
                return json.loads(raw)
            except (TimeoutError, OSError) as exc:
                if _is_transient_socket_error(str(exc)):
                    continue
                raise

        raise RuntimeError("executioner ack deadline exceeded")

    # -------------------------------------------------------------------------
    # Internal: Result reporting and messaging
    # -------------------------------------------------------------------------

    def report_result(
        self, target_pid: int, data: Dict[str, Any], trace_id: str = "", append: bool = True
    ) -> None:
        if not self._bus_sock:
            return
        relay: Dict[str, Any] = {
            "message_type": "IPM_RELAY",
            "target_pid": target_pid,
            "trace_id": trace_id,
            "payload": data,
        }
        with self._bus_lock:
            self._send_framed(self._bus_sock, relay)

        if append:
            # Normal path: wake the waiting dispatcher for synchronous tool calls.
            self.result_reported = True
        else:
            # Async path: clear the trace ID so dispatcher stops waiting.
            with self._queue_lock:
                if trace_id:
                    self._pending_response_traces.discard(trace_id)
                    self._response_map.pop(trace_id, None)

    # -------------------------------------------------------------------------
    # Internal: Heartbeat / Kernel IO loop
    # -------------------------------------------------------------------------

    def _run_kernel_io_loop(self) -> None:
        """Mirrors run_kernel_io_loop — periodic heartbeat with final death rattle."""
        hb_interval = _get_config("SDK_HEARTBEAT_SEC", 5)

        while self.is_running and not self.force_terminate:
            heartbeat: Dict[str, Any] = {
                "message_type": "HEARTBEAT",
                "pid": self.velix_pid,
                "payload": {
                    "status": self._status,
                    "memory_mb": self._get_current_memory_usage_mb(),
                },
            }
            try:
                sup_port = self._resolve_port("SUPERVISOR", 5173)
                sock = self._connect_with_retries("127.0.0.1", sup_port, 3000, 1, 0)
                try:
                    self._send_framed(sock, heartbeat)
                    self._recv_framed_raw(sock)  # consume {"status": "ok"}
                finally:
                    sock.close()
            except Exception:
                if self.is_running:
                    _log_warn("Lost supervisor connection during heartbeat. Engine terminating.")
                    self.is_running = False
                    import sys
                    sys.exit(1)

            # Interruptible sleep — mirrors sleep_cv.wait_for with force_terminate predicate
            deadline = time.monotonic() + hb_interval
            while time.monotonic() < deadline and not self.force_terminate:
                time.sleep(0.1)

        # Final death rattle — mirrors the post-loop block in C++
        if self.velix_pid > 0:
            try:
                final_status = "KILLED" if self.forced_by_signal else "FINISHED"
                final_hb: Dict[str, Any] = {
                    "message_type": "HEARTBEAT",
                    "pid": self.velix_pid,
                    "payload": {
                        "status": final_status,
                        "memory_mb": self._get_current_memory_usage_mb(),
                    },
                }
                sup_port = self._resolve_port("SUPERVISOR", 5173)
                sock = self._connect_with_retries("127.0.0.1", sup_port, 3000, 1, 0)
                try:
                    self._send_framed(sock, final_hb)
                finally:
                    sock.close()
            except Exception:
                pass

    # -------------------------------------------------------------------------
    # Internal: Bus connection and listener
    # -------------------------------------------------------------------------

    def _connect_bus(self, bus_port: int, retry_limit: int, retry_delay: int) -> None:
        sock = self._connect_with_retries("127.0.0.1", bus_port, 5000, retry_limit, retry_delay)
        sock.settimeout(1.0)

        bus_reg: Dict[str, Any] = {
            "message_type": "BUS_REGISTER",
            "pid": self.velix_pid,
            "tree_id": self.tree_id,
            "is_root": self.is_root,
        }
        self._send_framed(sock, bus_reg)
        self._recv_framed_raw(sock)  # OK ack

        self._bus_sock = sock
        self._bus_thread = threading.Thread(target=self._bus_listener_loop, daemon=True)
        self._bus_thread.start()

    def _bus_listener_loop(self) -> None:
        """Mirrors bus_listener_loop — routes IPM_PUSH to RPC waiters or on_bus_event."""
        if not self._bus_sock:
            return
        try:
            while self.is_running and self._bus_sock:
                try:
                    raw = self._recv_framed_raw(self._bus_sock)
                    msg: Dict[str, Any] = json.loads(raw)

                    msg_type = msg.get("message_type", "")
                    if msg_type in ("IPM_PUSH", "CHILD_TERMINATED"):
                        trace_id: str = msg.get("trace_id", "")
                        payload = msg.get("payload", {})
                        routed_to_rpc = False

                        if trace_id:
                            with self._response_cv:
                                if trace_id in self._pending_response_traces:
                                    self._response_map[trace_id] = payload
                                    self._response_cv.notify_all()
                                    routed_to_rpc = True

                        if not routed_to_rpc and self.on_bus_event is not None:
                            try:
                                self.on_bus_event(msg)
                            except Exception as exc:
                                _log_warn(f"on_bus_event hook failed: {exc}")

                except (TimeoutError, OSError) as exc:
                    if _is_transient_socket_error(str(exc)):
                        time.sleep(0.01)
                        continue
                    raise

        except Exception as exc:
            _log_warn(f"Velix Bus connection lost: {exc}")
            with self._response_cv:
                self.is_running = False
                self._response_cv.notify_all()

    # -------------------------------------------------------------------------
    # Internal: Socket helpers
    # -------------------------------------------------------------------------

    @property
    def _status(self) -> str:
        return self.__status

    @_status.setter
    def _status(self, value: str) -> None:
        self.__status = value

    def _connect_with_retries(
        self,
        host: str,
        port: int,
        timeout_ms: int,
        retry_limit: int,
        retry_delay_ms: int,
    ) -> socket.socket:
        last_exc: Optional[Exception] = None
        for i in range(max(1, retry_limit)):
            try:
                sock = socket.create_connection((host, port), timeout=timeout_ms / 1000.0)
                sock.settimeout(timeout_ms / 1000.0)
                return sock
            except Exception as exc:
                last_exc = exc
                if i == retry_limit - 1:
                    break
                time.sleep(retry_delay_ms * (i + 1) / 1000.0)
        raise RuntimeError(f"connect_with_retries failed: {last_exc}")

    def _request_with_retries(
        self,
        service_key: str,
        port: int,
        payload: Dict[str, Any],
        retry_limit: int,
        retry_delay_ms: int,
        timeout_ms: int = 5000,
    ) -> Dict[str, Any]:
        last_exc: Optional[Exception] = None
        for i in range(max(1, retry_limit)):
            try:
                sock = self._connect_with_retries("127.0.0.1", port, timeout_ms, 1, 0)
                try:
                    self._send_framed(sock, payload)
                    return json.loads(self._recv_framed_raw(sock))
                finally:
                    sock.close()
            except Exception as exc:
                last_exc = exc
                if i == retry_limit - 1:
                    break
                time.sleep(retry_delay_ms * (i + 1) / 1000.0)
        raise RuntimeError(f"Request to {service_key} failed after retries: {last_exc}")

    @staticmethod
    def _send_framed(sock: socket.socket, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        sock.sendall(struct.pack(">I", len(body)) + body)

    @staticmethod
    def _recv_framed_raw(sock: socket.socket) -> str:
        header = VelixProcess._recv_exact(sock, 4)
        size = struct.unpack(">I", header)[0]
        body = VelixProcess._recv_exact(sock, size)
        return body.decode("utf-8")

    @staticmethod
    def _recv_exact(sock: socket.socket, n: int) -> bytes:
        data = bytearray()
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                raise RuntimeError("socket closed")
            data.extend(chunk)
        return bytes(data)

    # -------------------------------------------------------------------------
    # Internal: Memory, config, ports
    # -------------------------------------------------------------------------

    @staticmethod
    def _get_current_memory_usage_mb() -> float:
        """Dependency-free memory query. Returns 0.0 on unsupported platforms."""
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

    @staticmethod
    def _resolve_port(service_name: str, fallback: int) -> int:
        """Mirrors resolve_port — SCHEDULER resolves LLM_SCHEDULER first."""
        return _get_port(service_name, fallback)

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


# =============================================================================
# Module-level helpers — mirror anonymous namespace in velix_process.cpp
# =============================================================================

def _is_supported_intent_value(intent: str) -> bool:
    return intent in ("JOIN_PARENT_TREE", "NEW_TREE")


def _validate_launch_intent(role: str, parent_pid: int, launch_intent: str) -> str:
    if launch_intent not in ("JOIN_PARENT_TREE", "NEW_TREE"):
        if role == "handler":
            return "JOIN_PARENT_TREE"
        return "JOIN_PARENT_TREE" if parent_pid > 0 else "NEW_TREE"
    return launch_intent


def _resolve_effective_mode(
    mode: str, convo_id: str, effective_user_id: str, is_handler: bool
) -> str:
    if mode:
        return mode
    if not convo_id and not effective_user_id:
        return "simple"
    if effective_user_id and is_handler:
        return "user_conversation"
    return "conversation"


def _is_transient_socket_error(err: str) -> bool:
    transient_markers = (
        "timed out", "timeout", "errno 11", "errno 35", "errno 110",
        "Resource temporarily unavailable",
    )
    return any(m in err for m in transient_markers)


def _get_config(key: str, default: int) -> int:
    try:
        return int(os.getenv(key, str(default)))
    except (ValueError, TypeError):
        return default


def _get_port(service_name: str, fallback: int) -> int:
    alias_map = {
        "SCHEDULER": ["LLM_SCHEDULER", "SCHEDULER"],
        "LLM_SCHEDULER": ["LLM_SCHEDULER", "SCHEDULER"],
    }
    lookup_keys = alias_map.get(service_name, [service_name])

    here = os.path.abspath(os.path.dirname(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    candidate_paths = [
        os.path.join(os.getcwd(), "config", "ports.json"),
        os.path.join(os.getcwd(), "..", "config", "ports.json"),
        os.path.join(os.getcwd(), "build", "config", "ports.json"),
        os.path.join(repo_root, "config", "ports.json"),
        os.path.join(repo_root, "build", "config", "ports.json"),
    ]
    for path in candidate_paths:
        try:
            with open(path, "r", encoding="utf-8") as f:
                ports = json.load(f)
            for key in lookup_keys:
                if key in ports:
                    return int(ports[key])
        except Exception:
            continue
    return fallback


def _log_info(msg: str) -> None:
    print(f"[INFO] {msg}", flush=True)


def _log_warn(msg: str) -> None:
    print(f"[WARN] {msg}", flush=True)


def _log_error(msg: str) -> None:
    print(f"[ERROR] {msg}", flush=True)


# =============================================================================
# Signal handling — mirrors posix_signal_handler
# =============================================================================

def _posix_signal_handler(signum: int, frame: Any) -> None:
    if VelixProcess._instance is not None:
        VelixProcess._instance.request_forced_shutdown()


# =============================================================================
# ResultGuard — mirrors the RAII struct in the C++ header
# =============================================================================

class _ResultGuard:
    """
    Context manager equivalent of the C++ ResultGuard RAII struct.
    Fires report_result on exit if the process has a parent and hasn't
    already reported a result.
    """

    def __init__(self, proc: VelixProcess) -> None:
        self._proc = proc

    def __enter__(self) -> "_ResultGuard":
        return self

    def __exit__(self, *_: Any) -> None:
        proc = self._proc
        if proc and not proc.result_reported and proc.parent_pid > 0:
            completion = {
                "status": "completed",
                "exit_reason": "normal",
                "pid": proc.velix_pid,
            }
            proc.report_result(proc.parent_pid, completion, proc.entry_trace_id)