use std::collections::HashMap;
use std::env;
use std::io::{Read, Write};
use std::net::Shutdown;
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::{json, Map, Value};

pub struct VelixProcess {
    pub process_name: String,
    pub role: String,
    pub os_pid: i32,
    pub velix_pid: i32,
    pub tree_id: String,
    pub parent_pid: i32,
    pub entry_trace_id: String,
    pub user_id: String,
    pub params: Value,
    status: Arc<Mutex<String>>,
    is_running: Arc<AtomicBool>,
    bus_stream: Option<TcpStream>,
    responses: Arc<(Mutex<HashMap<String, Value>>, Condvar)>,
}

impl VelixProcess {
    pub fn new(name: &str, role: &str) -> Self {
        Self {
            process_name: name.to_string(),
            role: role.to_string(),
            os_pid: std::process::id() as i32,
            velix_pid: -1,
            tree_id: String::new(),
            parent_pid: env::var("VELIX_PARENT_PID").ok().and_then(|v| v.parse().ok()).unwrap_or(-1),
            entry_trace_id: env::var("VELIX_TRACE_ID").unwrap_or_default(),
            user_id: env::var("VELIX_USER_ID").unwrap_or_default(),
            params: parse_env_json("VELIX_PARAMS"),
            status: Arc::new(Mutex::new("STARTING".to_string())),
            is_running: Arc::new(AtomicBool::new(false)),
            bus_stream: None,
            responses: Arc::new((Mutex::new(HashMap::new()), Condvar::new())),
        }
    }

    pub fn start<F>(&mut self, mut run: F) -> Result<(), String>
    where
        F: FnMut(&mut VelixProcess) -> Result<(), String>,
    {
        if self.is_running.load(Ordering::SeqCst) {
            return Ok(());
        }
        self.is_running.store(true, Ordering::SeqCst);

        let mut reg = json!({
            "message_type": "REGISTER_PID",
            "payload": {
                "register_intent": if self.parent_pid <= 0 { "NEW_TREE" } else { "JOIN_PARENT_TREE" },
                "role": self.role,
                "os_pid": self.os_pid,
                "process_name": self.process_name,
                "trace_id": self.entry_trace_id,
                "status": "STARTING",
                "memory_mb": 0
            }
        });
        if self.parent_pid > 0 {
            reg["source_pid"] = json!(self.parent_pid);
        }

        let reply = self.request("SUPERVISOR", &reg, Duration::from_secs(5))?;
        if reply["status"] != "ok" {
            return Err(format!("registration failed: {}", reply));
        }
        self.velix_pid = reply["process"]["pid"].as_i64().unwrap_or(-1) as i32;
        self.tree_id = reply["process"]["tree_id"].as_str().unwrap_or("").to_string();

        self.connect_bus()?;
        self.start_heartbeat();

        let result = run(self);
        self.shutdown();
        result
    }

    pub fn shutdown(&mut self) {
        self.is_running.store(false, Ordering::SeqCst);
        if let Some(stream) = self.bus_stream.as_mut() {
            let _ = stream.shutdown(Shutdown::Both);
        }
        self.bus_stream = None;
    }

    pub fn call_llm(&self, convo_id: &str, user_message: &str, system_message: &str, user_id: &str, mode: &str) -> Result<String, String> {
        self.call_llm_internal(convo_id, user_message, system_message, user_id, mode, None)
    }

    pub fn call_llm_resume(&self, convo_id: &str, tool_result: Value, user_id: &str) -> Result<String, String> {
        self.call_llm_internal(convo_id, "", "", user_id, "user_conversation", Some(tool_result))
    }

    fn call_llm_internal(&self, convo_id: &str, user_message: &str, system_message: &str, user_id: &str, mode: &str, tool_result_override: Option<Value>) -> Result<String, String> {
        let m = if !mode.is_empty() {
            mode.to_string()
        } else if convo_id.is_empty() && user_id.is_empty() {
            "simple".to_string()
        } else if !user_id.is_empty() {
            "user_conversation".to_string()
        } else {
            "conversation".to_string()
        };

        let base_payload = json!({
            "message_type": "LLM_REQUEST",
            "mode": m,
            "tree_id": self.tree_id,
            "source_pid": self.velix_pid,
            "priority": 1,
            "convo_id": convo_id,
            "user_id": user_id,
            "owner_pid": self.velix_pid
        });

        let mut next_messages: Vec<Value> = Vec::new();
        if !system_message.is_empty() {
            next_messages.push(json!({"role": "system", "content": system_message}));
        }
        if !user_message.is_empty() {
            next_messages.push(json!({"role": "user", "content": user_message}));
        }

        const MAX_ITERATIONS: usize = 15;
        for i in 0..MAX_ITERATIONS {
            self.set_status("WAITING_LLM");

            let mut payload = base_payload.clone();
            payload["request_id"] = Value::String(format!("req_{}_{}", self.velix_pid, rand_suffix()));
            payload["trace_id"] = Value::String(format!("trace_{}", now_nanos()));

            if i == 0 {
                if let Some(tr) = &tool_result_override {
                    payload["tool_message"] = tr.clone();
                }
            }

            if !next_messages.is_empty() {
                payload["messages"] = Value::Array(next_messages.clone());
            }

            let resp = self.request("LLM_SCHEDULER", &payload, Duration::from_secs(120))?;
            if resp["status"] != "ok" {
                self.set_status("ERROR");
                return Err(resp["error"].as_str().unwrap_or("llm request failed").to_string());
            }

            let tool_calls = resp.get("tool_calls").and_then(|v| v.as_array());
            if tool_calls.is_none() || tool_calls.unwrap().is_empty() {
                self.set_status("RUNNING");
                return Ok(resp["response"].as_str().unwrap_or("").to_string());
            }

            let tool_calls = tool_calls.unwrap();
            let mut tool_messages: Vec<Value> = Vec::new();
            let mut tool_executed = false;

            for call in tool_calls {
                let name = call.get("function").and_then(|f| f.get("name")).and_then(|v| v.as_str())
                    .or_else(|| call.get("name").and_then(|v| v.as_str()))
                    .unwrap_or("")
                    .trim()
                    .to_string();
                if name.is_empty() {
                    continue;
                }

                let args = call.get("function").and_then(|f| f.get("arguments")).cloned()
                    .or_else(|| call.get("arguments").cloned())
                    .filter(|v| v.is_object())
                    .unwrap_or_else(|| Value::Object(Map::new()));

                self.set_status("RUNNING");
                let result = self.execute_tool(&name, args)?;
                tool_executed = true;

                let call_id = call.get("id").and_then(|v| v.as_str())
                    .or_else(|| call.get("trace_id").and_then(|v| v.as_str()))
                    .unwrap_or("");

                tool_messages.push(json!({
                    "role": "tool",
                    "content": result.to_string(),
                    "tool_call_id": call_id
                }));
            }

            if !tool_executed {
                self.set_status("RUNNING");
                return Ok(resp["response"].as_str().unwrap_or("").to_string());
            }

            next_messages = tool_messages;
        }

        self.set_status("ERROR");
        Ok("Failure: Agent state machine exceeded max iterations.".to_string())
    }

    pub fn execute_tool(&self, name: &str, params: Value) -> Result<Value, String> {
        let trace_id = format!("trace_{}", now_nanos());
        let req = json!({
            "message_type": "EXEC_VELIX_PROCESS",
            "trace_id": trace_id,
            "tree_id": self.tree_id,
            "source_pid": self.velix_pid,
            "name": name,
            "params": params
        });
        let mut req = req;
        if !self.user_id.is_empty() {
            req["user_id"] = Value::String(self.user_id.clone());
        }
        let ack = self.request("EXECUTIONER", &req, Duration::from_secs(5))?;
        if ack["status"] != "ok" {
            return Err(format!("executioner rejected: {}", ack));
        }

        let (lock, cvar) = &*self.responses;
        let mut map = lock.lock().map_err(|_| "response lock poisoned".to_string())?;
        let timeout_at = std::time::Instant::now() + Duration::from_secs(60 * 60);
        loop {
            if let Some(payload) = map.remove(&trace_id) {
                if payload["error"] == "child_terminated" {
                    return Err(format!("child terminated: {}", payload));
                }
                return Ok(payload);
            }
            if std::time::Instant::now() > timeout_at {
                return Err("tool result timeout".to_string());
            }
            let wait = cvar.wait_timeout(map, Duration::from_millis(100)).map_err(|_| "wait failed".to_string())?;
            map = wait.0;
        }
    }

    pub fn report_result(&mut self, target_pid: i32, data: Value, trace_id: &str, append: bool) -> Result<(), String> {
        if let Some(stream) = self.bus_stream.as_mut() {
            let tid = if trace_id.is_empty() { &self.entry_trace_id } else { trace_id };

            let msg = json!({
                "message_type": "IPM_RELAY",
                "target_pid": target_pid,
                "trace_id": tid,
                "payload": data
            });
            send_framed(stream, &msg).map_err(|e| e.to_string())?;

            if !append && !tid.is_empty() {
                let (lock, cvar) = &*self.responses;
                if let Ok(mut map) = lock.lock() {
                    map.remove(tid);
                    cvar.notify_all();
                }
            }
        }
        Ok(())
    }

    fn connect_bus(&mut self) -> Result<(), String> {
        let mut stream = connect("127.0.0.1", get_port("BUS", 5174), Duration::from_secs(5))?;
        let reg = json!({"message_type":"BUS_REGISTER", "pid": self.velix_pid});
        send_framed(&mut stream, &reg).map_err(|e| e.to_string())?;
        let _ = recv_framed(&mut stream).map_err(|e| e.to_string())?;

        let mut listener = stream.try_clone().map_err(|e| e.to_string())?;
        let responses = Arc::clone(&self.responses);
        let is_running = Arc::clone(&self.is_running);
        thread::spawn(move || {
            while is_running.load(Ordering::SeqCst) {
                let msg = match recv_framed(&mut listener) {
                    Ok(m) => m,
                    Err(_) => break,
                };
                let mt = msg["message_type"].as_str().unwrap_or("");
                if mt == "IPM_PUSH" || mt == "CHILD_TERMINATED" {
                    if let Some(trace_id) = msg["trace_id"].as_str() {
                        let (lock, cvar) = &*responses;
                        if let Ok(mut map) = lock.lock() {
                            map.insert(trace_id.to_string(), msg["payload"].clone());
                            cvar.notify_all();
                        }
                    }
                }
            }
        });

        self.bus_stream = Some(stream);
        Ok(())
    }

    fn start_heartbeat(&self) {
        let pid = self.velix_pid;
        let is_running = Arc::clone(&self.is_running);
        thread::spawn(move || {
            while is_running.load(Ordering::SeqCst) {
                let hb = json!({
                    "message_type": "HEARTBEAT",
                    "pid": pid,
                    "payload": {"status":"RUNNING", "memory_mb":0}
                });
                let _ = request_static("SUPERVISOR", &hb, Duration::from_secs(3));
                thread::sleep(Duration::from_secs(5));
            }
        });
    }

    fn request(&self, service: &str, payload: &Value, timeout: Duration) -> Result<Value, String> {
        request_static(service, payload, timeout)
    }

    fn set_status(&self, status: &str) {
        if let Ok(mut guard) = self.status.lock() {
            *guard = status.to_string();
        }
    }
}

fn extract_tool_calls(reply: &Value) -> Vec<Value> {
    let mut out: Vec<Value> = Vec::new();

    if let Some(arr) = reply.get("exec_blocks").and_then(|v| v.as_array()) {
        for item in arr {
            if item.is_object() {
                out.push(item.clone());
                continue;
            }
            if let Some(raw) = item.as_str() {
                let trimmed = raw.trim();
                if !trimmed.is_empty() {
                    if let Ok(parsed) = serde_json::from_str::<Value>(trimmed) {
                        if parsed.is_object() {
                            out.push(parsed);
                        }
                    }
                }
            }
        }
    }

    if let Some(arr) = reply.get("tool_calls").and_then(|v| v.as_array()) {
        for item in arr {
            if item.is_object() {
                out.push(item.clone());
            }
        }
    }

    let response_text = reply
        .get("response")
        .and_then(|v| v.as_str())
        .unwrap_or("");

    let mut search_at: usize = 0;
    while search_at < response_text.len() {
        let rel_start = match response_text[search_at..].find("EXEC") {
            Some(v) => v,
            None => break,
        };
        let start = search_at + rel_start;
        let rel_end = match response_text[start..].find("END_EXEC") {
            Some(v) => v,
            None => break,
        };
        let end = start + rel_end;

        let raw = response_text[start + "EXEC".len()..end].trim();
        if !raw.is_empty() {
            if let Ok(parsed) = serde_json::from_str::<Value>(raw) {
                if parsed.is_object() {
                    out.push(parsed);
                }
            }
        }

        search_at = end + "END_EXEC".len();
    }

    out
}

fn request_static(service: &str, payload: &Value, timeout: Duration) -> Result<Value, String> {
    let mut stream = connect("127.0.0.1", get_port(service, 0), timeout)?;
    send_framed(&mut stream, payload).map_err(|e| e.to_string())?;
    recv_framed(&mut stream).map_err(|e| e.to_string())
}

fn connect(host: &str, port: i32, timeout: Duration) -> Result<TcpStream, String> {
    if port <= 0 {
        return Err("invalid port".to_string());
    }
    TcpStream::connect_timeout(&(format!("{}:{}", host, port).parse().map_err(|_| "bad addr")?), timeout)
        .map_err(|e| e.to_string())
}

fn send_framed(stream: &mut TcpStream, payload: &Value) -> std::io::Result<()> {
    let body = serde_json::to_vec(payload)?;
    let size = (body.len() as u32).to_be_bytes();
    stream.write_all(&size)?;
    stream.write_all(&body)?;
    Ok(())
}

fn recv_framed(stream: &mut TcpStream) -> std::io::Result<Value> {
    let mut hdr = [0u8; 4];
    stream.read_exact(&mut hdr)?;
    let size = u32::from_be_bytes(hdr) as usize;
    let mut body = vec![0u8; size];
    stream.read_exact(&mut body)?;
    serde_json::from_slice(&body)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))
}

fn get_port(name: &str, fallback: i32) -> i32 {
    for p in ["config/ports.json", "../config/ports.json", "build/config/ports.json"] {
        let raw = match std::fs::read_to_string(p) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let v: Value = match serde_json::from_str(&raw) {
            Ok(v) => v,
            Err(_) => continue,
        };
        return v.get(name).and_then(|x| x.as_i64()).unwrap_or(fallback as i64) as i32;
    }
    fallback
}

fn parse_env_json(name: &str) -> Value {
    let raw = env::var(name).unwrap_or_default();
    if raw.is_empty() {
        return Value::Object(Map::new());
    }
    serde_json::from_str(&raw).unwrap_or(Value::Object(Map::new()))
}

fn now_nanos() -> u128 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or(Duration::from_secs(0)).as_nanos()
}

fn rand_suffix() -> u128 {
    now_nanos() % 100000000
}
