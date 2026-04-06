"""
gateway.py — Base Gateway

Every gateway (terminal, Telegram, HTTP, etc.) inherits from Gateway.
The base class handles:

  - framed TCP communication with the Handler
  - session registration handshake (user_id negotiation)
  - sending messages/events to the Handler
  - receiving and dispatching notifications from the Handler
  - clean connection lifecycle

Subclasses must implement:
  - get_next_input() -> str | None   (blocking read from their transport)
  - deliver(event: dict)             (push an event to the user)
  - on_connected()                   (optional — called after registration ack)
  - on_disconnected()                (optional — called on teardown)
"""

import json
import socket
import struct
import threading
import time
from abc import ABC, abstractmethod
from typing import Callable, Dict, List, Optional


class Gateway(ABC):
    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    def __init__(
        self,
        handler_host: str = "127.0.0.1",
        handler_port: int = 6060,
        user_id: Optional[str] = None,
        connect_timeout_s: float = 5.0,
        recv_timeout_s: float = 0.05,
    ) -> None:
        self._handler_host = handler_host
        self._handler_port = handler_port
        self._requested_user_id: Optional[str] = user_id
        self._user_id: Optional[str] = None
        self._connect_timeout_s = connect_timeout_s
        self._recv_timeout_s = recv_timeout_s

        self._sock: Optional[socket.socket] = None
        self._sock_lock = threading.Lock()

        self._running = False
        self._recv_thread: Optional[threading.Thread] = None

        # ── Command registry ─────────────────────────────────────────────────────────
        # Maps "/command" -> callable(args: str) -> None
        # Populated by _register_builtin_commands() and subclass register_command().
        # run() dispatches lines starting with '/' here before forwarding to handler.
        self._command_registry: Dict[str, Callable[[str], None]] = {}
        self._register_builtin_commands()

    # ------------------------------------------------------------------ #
    # Public lifecycle                                                     #
    # ------------------------------------------------------------------ #

    def connect(self, force_new: bool = False) -> None:
        """
        Connect to the Handler, perform the registration handshake,
        and start the background receive thread.
        Raises RuntimeError if the handler rejects the connection.
        """
        sock = socket.create_connection(
            (self._handler_host, self._handler_port),
            timeout=self._connect_timeout_s,
        )
        sock.settimeout(self._recv_timeout_s)
        self._sock = sock

        # Send registration message.
        reg: dict = {"type": "register", "force_new": force_new}
        if self._requested_user_id:
            reg["user_id"] = self._requested_user_id
        self._send(reg)

        # Wait for the handler's ack (blocking, uses connect_timeout).
        self._sock.settimeout(self._connect_timeout_s)
        ack = self._recv_one()
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

        self._user_id = ack["user_id"]
        self._running = True

        # Start background thread that reads events from the Handler and
        # dispatches them to deliver() / subclass handlers.
        self._recv_thread = threading.Thread(
            target=self._recv_loop, daemon=True, name=f"gw-recv-{self._user_id}"
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
        Main loop: read input from the transport and forward it to the Handler.
        Lines starting with '/' are checked against the command registry first.
        Blocks until the gateway is stopped. Subclasses should call this after connect().
        """
        while self._running:
            try:
                text = self.get_next_input()
                if text is None:
                    break
                text = text.strip()
                if not text:
                    continue
                # ── Command dispatch ──────────────────────────────────────────────
                # Split on first space to support: /command optional_args
                # Unknown slash-prefixed words are forwarded to the handler
                # so the LLM can respond to questions like "what is /compact?".
                if text.startswith("/"):
                    cmd, _, args = text.partition(" ")
                    handler_fn = self._command_registry.get(cmd)
                    if handler_fn is not None:
                        try:
                            handler_fn(args)
                        except Exception as exc:
                            self.deliver({"type": "token", "data": f"[Command error: {exc}]\n"})
                        continue  # Don't forward handled commands to the LLM.
                # Forward as a normal message to the handler.
                self.send_user_message(text)
            except (KeyboardInterrupt, EOFError):
                break
            except Exception as exc:
                print(f"[Gateway] Input error: {exc}")
                break
        self.disconnect()

    # ------------------------------------------------------------------ #
    # Command registry                                                     #
    # ------------------------------------------------------------------ #

    def register_command(
        self, name: str, handler: Callable[[str], None]
    ) -> None:
        """
        Register a client-side slash command.

        name    -- e.g. "/debug" (must start with '/')
        handler -- callable(args: str) -> None
                   args is everything after the command name, stripped.

        Client-side commands run entirely in Python and are NOT forwarded
        to the C++ handler. Use send_user_message() inside the handler if
        you need to also send something to the LLM after local processing.

        Example::
            gw.register_command("/debug", lambda args: print(f"debug: {args}"))
        """
        if not name.startswith("/"):
            raise ValueError(f"Command name must start with '/': {name!r}")
        self._command_registry[name] = handler

    def _register_builtin_commands(self) -> None:
        """Register commands that the gateway handles locally (client-side)."""

        def _help(_args: str) -> None:
            lines = ["Available commands:"] + sorted(self._command_registry)
            self.deliver({"type": "token", "data": "\n".join(lines) + "\n"})

        self.register_command("/help", _help)
        # Session commands are forwarded to the handler as plain messages so
        # the C++ side can update session state. They are NOT intercepted here.
        # (The C++ handler's own command table handles /new, /compact, etc.)

    # ------------------------------------------------------------------ #
    # Sending to Handler                                                   #
    # ------------------------------------------------------------------ #

    def send_user_message(self, message: str, user_id: Optional[str] = None) -> None:
        """Forward a user text message to the Handler for LLM processing."""
        self._send({
            "type": "message",
            "message": message,
            "user_id": user_id or self._user_id or "",
        })

    def send_approval_reply(self, approval_trace: str, scope: str) -> None:
        """Forward a user approval decision to the Handler."""
        self._send({
            "type": "approval_reply",
            "approval_trace": approval_trace,
            "scope": scope,
        })

    def send_raw(self, payload: dict) -> None:
        """Send an arbitrary payload to the Handler."""
        self._send(payload)

    @classmethod
    def list_sessions(cls, super_user: str, host: str="127.0.0.1", port: int=6060) -> list:
        sock = socket.create_connection((host, port), timeout=5.0)
        sock.settimeout(5.0)
        try:
            Gateway._send_framed(sock, {"type": "list_sessions", "super_user": super_user})
            resp = Gateway._recv_one_from(sock)
            if resp.get("type") == "session_list":
                sessions = resp.get("sessions", [])
                if not isinstance(sessions, list):
                    return []

                normalized = []
                for s in sessions:
                    if isinstance(s, dict):
                        sid = s.get("id")
                        if isinstance(sid, str) and sid:
                            normalized.append({"id": sid, "active": bool(s.get("active", False))})
                return normalized
            return []
        except Exception:
            return []
        finally:
            sock.close()

    @staticmethod
    def list_users(
        handler_host: str = "127.0.0.1",
        handler_port: int = 6060,
        timeout_s: float = 5.0,
    ) -> List[dict]:
        """Query handler for known persistent user identities."""
        sock = socket.create_connection((handler_host, handler_port), timeout=timeout_s)
        sock.settimeout(timeout_s)
        try:
            Gateway._send_framed(sock, {"type": "list_users"})
            reply = Gateway._recv_one_from(sock)
        finally:
            try:
                sock.close()
            except Exception:
                pass

        if reply.get("type") != "user_list":
            raise RuntimeError(f"Unexpected list_users response: {reply}")

        users = reply.get("users", [])
        if not isinstance(users, list):
            return []
        
        # New format: list of dicts {id, active}
        processed = []
        for u in users:
            if isinstance(u, dict):
                processed.append(u)
            elif isinstance(u, str):
                # Backwards compat: assume not active if it's just a string
                processed.append({"id": u, "active": False})
        return processed

    # ------------------------------------------------------------------ #
    # Abstract interface — subclasses must implement                       #
    # ------------------------------------------------------------------ #

    @abstractmethod
    def get_next_input(self) -> Optional[str]:
        """
        Block until the next piece of input arrives from the user transport.
        Return the input string, or None to signal clean shutdown.
        """

    @abstractmethod
    def deliver(self, event: dict) -> None:
        """
        Deliver an event that arrived from the Handler to the user.
        event["type"] is one of: "token", "end", "notify", "approval_ack", "error"
        """

    # ------------------------------------------------------------------ #
    # Optional hooks                                                       #
    # ------------------------------------------------------------------ #

    def on_connected(self) -> None:
        """Called once registration is confirmed. Override to customise."""

    def on_disconnected(self) -> None:
        """Called when the gateway tears down. Override to clean up."""

    # ------------------------------------------------------------------ #
    # Properties                                                           #
    # ------------------------------------------------------------------ #

    @property
    def user_id(self) -> Optional[str]:
        """The user_id confirmed by the Handler after registration."""
        return self._user_id

    @property
    def is_connected(self) -> bool:
        return self._running and self._sock is not None

    # ------------------------------------------------------------------ #
    # Internal: receive loop                                               #
    # ------------------------------------------------------------------ #

    def _recv_loop(self) -> None:
        """
        Background thread: reads events from the Handler and dispatches them.
        Exits when the socket closes or _running is False.
        """
        while self._running:
            try:
                event = self._recv_one()
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
        """Route an incoming Handler event to the appropriate handler."""
        event_type = event.get("type", "")

        # Delegate everything to deliver() — the subclass decides presentation.
        try:
            self.deliver(event)
        except Exception as exc:
            print(f"[Gateway:{self._user_id}] deliver() error: {exc}")

    # ------------------------------------------------------------------ #
    # Internal: framed socket IO                                           #
    # ------------------------------------------------------------------ #

    def _send(self, payload: dict) -> None:
        frame = self._encode_frame(payload)
        with self._sock_lock:
            if self._sock is None:
                raise RuntimeError("Gateway is not connected")
            self._sock.sendall(frame)

    def _recv_one(self) -> dict:
        """Read one framed message from the socket. May raise on timeout."""
        header = self._recv_exact(4)
        size = struct.unpack(">I", header)[0]
        body = self._recv_exact(size)
        return json.loads(body.decode("utf-8"))

    @staticmethod
    def _encode_frame(payload: dict) -> bytes:
        body = json.dumps(payload).encode("utf-8")
        return struct.pack(">I", len(body)) + body

    @staticmethod
    def _send_framed(sock: socket.socket, payload: dict) -> None:
        sock.sendall(Gateway._encode_frame(payload))

    @staticmethod
    def _recv_exact_from(sock: socket.socket, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("Handler socket closed")
            buf.extend(chunk)
        return bytes(buf)

    @staticmethod
    def _recv_one_from(sock: socket.socket) -> dict:
        header = Gateway._recv_exact_from(sock, 4)
        size = struct.unpack(">I", header)[0]
        body = Gateway._recv_exact_from(sock, size)
        return json.loads(body.decode("utf-8"))

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            # Access sock directly (called from recv thread or connect, no lock needed).
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("Handler socket closed")
            buf.extend(chunk)
        return bytes(buf)