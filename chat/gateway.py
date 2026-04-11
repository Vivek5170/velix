"""
gateway.py — Velix Base Gateway
================================

Every transport gateway (terminal, Telegram, HTTP, …) inherits from
``Gateway``.  The base class owns:

  - framed 4-byte-length-prefixed TCP communication with the Handler
  - session registration handshake (user_id / session resolution)
  - sending messages and approval replies to the Handler
  - background receive thread that dispatches events to deliver()
  - clean connection lifecycle (connect / disconnect / run)
  - an extensible client-side slash-command registry
  - static helpers for listing users and sessions before connecting

Subclasses must implement:
  - ``get_next_input() -> str | None``   (blocking read from their transport)
  - ``deliver(event: dict) -> None``     (push an event to the user)

Optional hooks (override as needed):
  - ``on_connected()``        called once the registration ack is received
  - ``on_disconnected()``     called when the gateway tears down
  - ``on_token(text: str)``   called for each streaming token  (default: deliver)
  - ``on_end()``              called after each full turn
  - ``on_tool_start(tool, args)``
  - ``on_tool_finish(tool, result)``
  - ``on_context_usage(current, maximum, pct)``
  - ``on_approval_request(approval_trace, payload)``
  - ``on_notify(event)``      generic / unhandled bus events
  - ``on_error(message)``     error frames from the handler

Protocol (wire format)
-----------------------
Every frame:  [ 4-byte big-endian uint32 length ][ UTF-8 JSON body ]

Events emitted by the Handler:
  type == "token"         → streaming text delta
  type == "end"           → end of one full turn
  type == "tool_start"    → tool execution starting
  type == "tool_finish"   → tool execution finished
  type == "context_usage" → token budget status
  type == "approval_request" → human-in-the-loop gate
  type == "approval_ack"  → acknowledgement of our approval_reply
  type == "session_switched" → session_id changed in-band
  type == "notify"        → generic bus notification
  type == "error"         → handler-side error

Commands sent by the client:
  type == "register"         → initial handshake
  type == "message"          → user text → LLM
  type == "tool_message"     → resume after external tool result
  type == "resume_turn"      → resume after approval result
  type == "approval_reply"   → answer an approval request
  type == "switch_session"   → swap to a different session_id
  type == "list_users"       → pre-auth: enumerate super-users
  type == "list_sessions"    → pre-auth: enumerate sessions for a super-user
"""

from __future__ import annotations

import json
import socket
import struct
import threading
from abc import ABC, abstractmethod
from typing import Callable, Dict, List, Optional


# ---------------------------------------------------------------------------
# Wire helpers (module-level so they can be reused by static methods)
# ---------------------------------------------------------------------------

def _encode_frame(payload: dict) -> bytes:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    return struct.pack(">I", len(body)) + body


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("Handler socket closed unexpectedly")
        buf.extend(chunk)
    return bytes(buf)


def _recv_one(sock: socket.socket) -> dict:
    header = _recv_exact(sock, 4)
    size   = struct.unpack(">I", header)[0]
    body   = _recv_exact(sock, size)
    return json.loads(body.decode("utf-8"))


# ---------------------------------------------------------------------------
# Gateway base class
# ---------------------------------------------------------------------------

class Gateway(ABC):
    """
    Abstract base gateway.  Subclass it, implement get_next_input() and
    deliver(), then call connect() followed by run().
    """

    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    def __init__(
        self,
        handler_host: str          = "127.0.0.1",
        handler_port: int          = 6060,
        user_id: Optional[str]     = None,
        connect_timeout_s: float   = 5.0,
        recv_timeout_s: float      = 0.05,
    ) -> None:
        self._handler_host       = handler_host
        self._handler_port       = handler_port
        self._requested_user_id  = user_id
        self._user_id: Optional[str] = None
        self._super_user: Optional[str] = None
        self._connect_timeout_s  = connect_timeout_s
        self._recv_timeout_s     = recv_timeout_s

        self._sock: Optional[socket.socket] = None
        self._sock_lock = threading.Lock()

        self._running = False
        self._recv_thread: Optional[threading.Thread] = None

        # Slash-command registry: "/cmd" → callable(args: str) -> None
        self._commands: Dict[str, Callable[[str], None]] = {}
        self._register_builtin_commands()

    # ------------------------------------------------------------------ #
    # Public lifecycle                                                     #
    # ------------------------------------------------------------------ #

    def connect(self, force_new: bool = False) -> None:
        """
        Connect to the Handler and complete the registration handshake.
        Starts the background receive thread.
        Raises RuntimeError on rejection.
        """
        sock = socket.create_connection(
            (self._handler_host, self._handler_port),
            timeout=self._connect_timeout_s,
        )
        sock.settimeout(self._recv_timeout_s)
        self._sock = sock

        # Registration frame.
        reg: dict = {"type": "register", "force_new": force_new}
        if self._requested_user_id:
            reg["user_id"] = self._requested_user_id
        self._send_raw(reg)

        # Wait for ack (use connect_timeout here; recv_timeout is too short).
        self._sock.settimeout(self._connect_timeout_s)
        ack = _recv_one(self._sock)
        self._sock.settimeout(self._recv_timeout_s)

        if ack.get("type") == "error":
            self._sock.close()
            self._sock = None
            raise RuntimeError(
                f"Handler rejected registration: {ack.get('message', 'unknown')}"
            )
        if ack.get("type") != "registered":
            self._sock.close()
            self._sock = None
            raise RuntimeError(f"Unexpected registration response: {ack}")

        self._user_id   = ack["user_id"]
        self._super_user = ack.get("super_user", self._user_id)
        self._running   = True

        self._recv_thread = threading.Thread(
            target=self._recv_loop,
            daemon=True,
            name=f"gw-recv-{self._user_id}",
        )
        self._recv_thread.start()

        self.on_connected()

    def disconnect(self) -> None:
        """Cleanly shut down the gateway."""
        self._running = False
        with self._sock_lock:
            if self._sock:
                try:
                    self._sock.close()
                except Exception:
                    pass
                self._sock = None
        if self._recv_thread and self._recv_thread.is_alive():
            self._recv_thread.join(timeout=3)
        self.on_disconnected()

    def run(self) -> None:
        """
        Main input loop.  Reads from get_next_input(), dispatches client-side
        slash commands, then forwards everything else to the Handler.
        Blocks until disconnected.  Call after connect().
        """
        while self._running:
            try:
                text = self.get_next_input()
                if text is None:
                    break
                text = text.strip()
                if not text:
                    continue

                # Slash-command dispatch (client-side).
                if text.startswith("/"):
                    cmd, _, args = text.partition(" ")
                    handler_fn = self._commands.get(cmd)
                    if handler_fn is not None:
                        try:
                            handler_fn(args.strip())
                        except Exception as exc:
                            self.deliver({"type": "token",
                                          "data": f"[Command error: {exc}]\n"})
                        continue
                    # Unknown /command → forward to LLM so the user can ask
                    # about it by name (e.g. "what does /compact do?").

                self.send_message(text)

            except (KeyboardInterrupt, EOFError):
                break
            except Exception as exc:
                print(f"[Gateway] Input loop error: {exc}")
                break

        self.disconnect()

    # ------------------------------------------------------------------ #
    # Sending to Handler                                                   #
    # ------------------------------------------------------------------ #

    def send_message(self, text: str) -> None:
        """Send a user text message to the Handler for LLM processing."""
        self._send_raw({
            "type":    "message",
            "message": text,
            "user_id": self._user_id or "",
        })

    def send_tool_message(self, tool_message: dict) -> None:
        """Resume an LLM turn with an external tool result."""
        self._send_raw({"type": "tool_message", "tool_message": tool_message})

    def send_resume_turn(self, result: dict) -> None:
        """Resume a turn with a structured result payload."""
        self._send_raw({"type": "resume_turn", "payload": {"result": result}})

    def send_approval_reply(self, approval_trace: str, scope: str) -> None:
        """Answer an approval request from the Handler."""
        self._send_raw({
            "type":           "approval_reply",
            "approval_trace": approval_trace,
            "scope":          scope,
        })

    def switch_session(self, session_id: str) -> None:
        """Request the Handler to switch this connection to a different session."""
        self._send_raw({"type": "switch_session", "session_id": session_id})

    def send_raw(self, payload: dict) -> None:
        """Send an arbitrary frame to the Handler."""
        self._send_raw(payload)

    # ------------------------------------------------------------------ #
    # Static pre-auth queries (no live session required)                   #
    # ------------------------------------------------------------------ #

    @staticmethod
    def list_users(
        handler_host: str = "127.0.0.1",
        handler_port: int = 6060,
        timeout_s: float  = 5.0,
    ) -> List[dict]:
        """
        Return all known super-users.
        Each entry: {"id": str, "active": bool}
        """
        sock = socket.create_connection(
            (handler_host, handler_port), timeout=timeout_s
        )
        sock.settimeout(timeout_s)
        try:
            sock.sendall(_encode_frame({"type": "list_users"}))
            reply = _recv_one(sock)
        finally:
            try:
                sock.close()
            except Exception:
                pass

        if reply.get("type") != "user_list":
            raise RuntimeError(f"Unexpected list_users response: {reply}")

        out: List[dict] = []
        for u in reply.get("users", []):
            if isinstance(u, dict):
                out.append({"id": u.get("id", ""), "active": bool(u.get("active"))})
            elif isinstance(u, str):
                out.append({"id": u, "active": False})
        return out

    @staticmethod
    def list_sessions(
        super_user: str,
        handler_host: str = "127.0.0.1",
        handler_port: int = 6060,
        timeout_s: float  = 5.0,
    ) -> List[dict]:
        """
        Return all sessions for a super-user.
        Each entry: {"id": str, "title": str, "turns": int, "active": bool}
        """
        sock = socket.create_connection(
            (handler_host, handler_port), timeout=timeout_s
        )
        sock.settimeout(timeout_s)
        try:
            sock.sendall(_encode_frame({
                "type":       "list_sessions",
                "super_user": super_user,
            }))
            reply = _recv_one(sock)
        finally:
            try:
                sock.close()
            except Exception:
                pass

        if reply.get("type") != "session_list":
            raise RuntimeError(f"Unexpected list_sessions response: {reply}")

        out: List[dict] = []
        for s in reply.get("sessions", []):
            if isinstance(s, dict):
                out.append({
                    "id":     s.get("id", ""),
                    "title":  s.get("title", ""),
                    "turns":  s.get("turns", 0),
                    "active": bool(s.get("active")),
                })
            elif isinstance(s, str):
                out.append({"id": s, "title": "", "turns": 0, "active": False})
        return out

    # ------------------------------------------------------------------ #
    # Command registry                                                     #
    # ------------------------------------------------------------------ #

    def register_command(self, name: str, fn: Callable[[str], None]) -> None:
        """
        Register a *client-side* slash command.

        name  — must start with '/', e.g. "/debug"
        fn    — callable(args: str) -> None
                args is everything after the command name, stripped.

        Client-side commands are NOT forwarded to the Handler.
        Call send_message() inside fn if you also need an LLM response.
        """
        if not name.startswith("/"):
            raise ValueError(f"Command name must start with '/': {name!r}")
        self._commands[name] = fn

    def _register_builtin_commands(self) -> None:
        """
        Built-in client-side commands.

        Note: /new, /compact, /undo, /sessions, /title, /session_info,
        /model_info, /scheduler_info, /context are implemented on the C++
        handler side — we forward them as plain messages so the handler can
        update session state and reply.  Only /help is intercepted here to
        avoid a network round-trip.
        """
        def _help(_args: str) -> None:
            lines = [
                "Client-side commands (handled locally):",
                "  /help            — this message",
                "",
                "Handler-side commands (forwarded to server):",
                "  /new             — start a new session",
                "  /compact         — compact current session",
                "  /undo            — undo last turn",
                "  /sessions        — list your sessions",
                "  /terminals       — list active persistent terminals",
                "  /title <text>    — set session title",
                "  /session_info    — current session stats",
                "  /model_info      — model & adapter config",
                "  /scheduler_info  — scheduler queue depth",
                "  /context         — context window usage",
                "",
                "Extra client commands registered by this gateway:",
            ]
            for cmd in sorted(self._commands):
                if cmd != "/help":
                    lines.append(f"  {cmd}")
            self.deliver({"type": "token", "data": "\n".join(lines) + "\n"})

        self.register_command("/help", _help)

    # ------------------------------------------------------------------ #
    # Abstract interface                                                   #
    # ------------------------------------------------------------------ #

    @abstractmethod
    def get_next_input(self) -> Optional[str]:
        """
        Block until the next user input arrives.
        Return the raw string, or None to signal clean shutdown.
        """

    @abstractmethod
    def deliver(self, event: dict) -> None:
        """
        Deliver a Handler event to the user.
        event["type"] is one of:
          "token", "end", "tool_start", "tool_finish", "context_usage",
          "approval_request", "approval_ack", "session_switched",
          "notify", "error"
        """

    # ------------------------------------------------------------------ #
    # Optional hooks — override any or all                                #
    # ------------------------------------------------------------------ #

    def on_connected(self) -> None:
        """Called once the registration handshake succeeds."""

    def on_disconnected(self) -> None:
        """Called when the gateway tears down."""

    def on_token(self, text: str) -> None:
        """Called for every streaming token delta. Default: calls deliver()."""
        self.deliver({"type": "token", "data": text})

    def on_end(self) -> None:
        """Called at the end of each complete LLM turn."""
        self.deliver({"type": "end"})

    def on_tool_start(self, tool: str, args: dict) -> None:
        """Called when a tool execution begins."""
        self.deliver({"type": "tool_start", "tool": tool, "args": args})

    def on_tool_finish(self, tool: str, result: dict) -> None:
        """Called when a tool execution completes."""
        self.deliver({"type": "tool_finish", "tool": tool, "result": result})

    def on_context_usage(
        self,
        current: int,
        maximum: int,
        pct: float,
        *,
        session_tokens: int = 0,
        system_prompt_tokens: int = 0,
        tool_schema_tokens: int = 0,
        request_tokens: int = 0,
        total_context_tokens: int = 0,
    ) -> None:
        """Called whenever the handler reports context window usage."""
        self.deliver({
            "type":           "context_usage",
            "current_tokens": current,
            "max_tokens":     maximum,
            "pct":            pct,
            "session_tokens": session_tokens,
            "system_prompt_tokens": system_prompt_tokens,
            "tool_schema_tokens": tool_schema_tokens,
            "request_tokens": request_tokens,
            "total_context_tokens": total_context_tokens,
        })

    def on_approval_request(self, approval_trace: str, payload: dict) -> None:
        """
        Called when an agent needs human approval.
        Default: delivers the event.  Override to show interactive UI.
        """
        self.deliver({
            "type":           "approval_request",
            "approval_trace": approval_trace,
            "payload":        payload,
        })

    def on_session_switched(self, session_id: str) -> None:
        """Called when the handler confirms a session switch."""
        self._user_id = session_id
        self.deliver({"type": "session_switched", "session_id": session_id})

    def on_notify(self, event: dict) -> None:
        """Called for generic bus notifications not handled by another hook."""
        self.deliver(event)

    def on_error(self, message: str) -> None:
        """Called for error frames from the handler."""
        self.deliver({"type": "error", "message": message})

    # ------------------------------------------------------------------ #
    # Properties                                                           #
    # ------------------------------------------------------------------ #

    @property
    def user_id(self) -> Optional[str]:
        """The resolved session_id confirmed by the Handler."""
        return self._user_id

    @property
    def super_user(self) -> Optional[str]:
        """The super-user identity (without _sN suffix)."""
        return self._super_user

    @property
    def is_connected(self) -> bool:
        return self._running and self._sock is not None

    # ------------------------------------------------------------------ #
    # Internal: receive loop                                               #
    # ------------------------------------------------------------------ #

    def _recv_loop(self) -> None:
        """Background thread: reads events and dispatches them via hooks."""
        while self._running:
            try:
                event = _recv_one(self._sock)  # type: ignore[arg-type]
                self._dispatch(event)
            except (TimeoutError, OSError):
                # Transient timeout — keep looping.
                continue
            except Exception as exc:
                if self._running:
                    print(f"[Gateway:{self._user_id}] Receive error: {exc}")
                break
        self._running = False

    def _dispatch(self, event: dict) -> None:
        """Route an incoming event to the correct hook or deliver()."""
        t = event.get("type", "")

        if t == "token":
            self.on_token(event.get("data", ""))

        elif t == "end":
            self.on_end()

        elif t == "tool_start":
            self.on_tool_start(
                event.get("tool", ""),
                event.get("args", event.get("summary", {})),
            )

        elif t == "tool_finish":
            self.on_tool_finish(
                event.get("tool", ""),
                event.get("result", event.get("summary", {})),
            )

        elif t == "context_usage":
            self.on_context_usage(
                event.get("current_tokens", 0),
                event.get("max_tokens", 0),
                event.get("pct", 0.0),
                session_tokens=event.get("session_tokens", 0),
                system_prompt_tokens=event.get("system_prompt_tokens", 0),
                tool_schema_tokens=event.get("tool_schema_tokens", 0),
                request_tokens=event.get("request_tokens", 0),
                total_context_tokens=event.get("total_context_tokens", 0),
            )

        elif t == "approval_request":
            payload = event.get("payload", {})
            trace = event.get("approval_trace", "")
            if not trace and isinstance(payload, dict):
                trace = str(payload.get("approval_trace", ""))
            self.on_approval_request(
                trace,
                payload,
            )

        elif t == "approval_ack":
            # Deliver raw for subclasses that want to react.
            self.deliver(event)

        elif t == "session_switched":
            self.on_session_switched(event.get("session_id", ""))

        elif t == "error":
            self.on_error(event.get("message", "unknown error"))

        elif t in ("notify", "tool_message", "independent_msg"):
            self.on_notify(event)

        else:
            # Unknown event type — hand it to deliver() unchanged.
            self.deliver(event)

    # ------------------------------------------------------------------ #
    # Internal: framed send                                                #
    # ------------------------------------------------------------------ #

    def _send_raw(self, payload: dict) -> None:
        frame = _encode_frame(payload)
        with self._sock_lock:
            if self._sock is None:
                raise RuntimeError("Gateway is not connected")
            self._sock.sendall(frame)