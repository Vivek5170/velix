const net = require('net');
const fs = require('fs');
const os = require('os');
const crypto = require('crypto');

class VelixProcess {
  constructor(name, role) {
    this.processName = name;
    this.role = role;
    this.osPid = process.pid;
    this.velixPid = -1;
    this.treeId = '';
    this.parentPid = Number(process.env.VELIX_PARENT_PID || -1);
    this.entryTraceId = process.env.VELIX_TRACE_ID || '';
    this.userId = process.env.VELIX_USER_ID || '';
    this.params = this.#parseJson(process.env.VELIX_PARAMS || '{}');
    this.isRunning = false;
    this.busSocket = null;
    this.responses = new Map();
    this.pendingToolWaiters = new Map();
    this.status = 'STARTING';
  }

  async run() {
    throw new Error('run() must be overridden');
  }

  async start() {
    if (this.isRunning) return;
    this.isRunning = true;

    const reg = {
      message_type: 'REGISTER_PID',
      payload: {
        register_intent: this.parentPid <= 0 ? 'NEW_TREE' : 'JOIN_PARENT_TREE',
        role: this.role,
        os_pid: this.osPid,
        process_name: this.processName,
        trace_id: this.entryTraceId,
        status: 'STARTING',
        memory_mb: 0,
      },
    };
    if (this.parentPid > 0) reg.source_pid = this.parentPid;

    const reply = await this.#request('SUPERVISOR', reg, 5000);
    if (reply.status !== 'ok') {
      throw new Error(`registration failed: ${JSON.stringify(reply)}`);
    }

    this.velixPid = Number(reply.process?.pid || -1);
    this.treeId = String(reply.process?.tree_id || '');

    await this.#connectBus();
    this.#startHeartbeat();

    try {
      await this.run();
    } finally {
      this.shutdown();
    }
  }

  shutdown() {
    this.isRunning = false;
    if (this.busSocket) {
      this.busSocket.destroy();
      this.busSocket = null;
    }
  }

  async callLlm(convoId, userMessage = '', systemMessage = '', userId = '', mode = 'user_conversation') {
    return this._callLlmInternal(convoId, userMessage, systemMessage, userId, mode);
  }

  /**
   * Resumes a conversation with an out-of-band tool result.
   */
  async callLlmResume(convoId, toolResult, userId = '') {
    return this._callLlmInternal(convoId || '', '', '', userId || '', 'user_conversation', toolResult);
  }

  async _callLlmInternal(convoId, userMessage, systemMessage, userId, mode, toolResultOverride = null) {
    const effectiveUserId = userId || '';
    const m = mode || (!convoId && !effectiveUserId ? 'simple' : (effectiveUserId ? 'user_conversation' : 'conversation'));

    const basePayload = {
      message_type: 'LLM_REQUEST',
      mode: m,
      tree_id: this.tree_id,
      source_pid: this.velixPid,
      priority: 1,
      convo_id: convoId || '',
      user_id: effectiveUserId,
      owner_pid: this.velixPid,
    };

    let nextMessages = [];
    if (systemMessage) nextMessages.push({ role: 'system', content: systemMessage });
    if (userMessage) nextMessages.push({ role: 'user', content: userMessage });

    const maxIterations = 15;
    for (let i = 0; i < maxIterations; i++) {
      this.status = 'WAITING_LLM';

      const payload = {
        ...basePayload,
        request_id: `req_${this.velixPid}_${crypto.randomUUID().slice(0, 8)}`,
        trace_id: crypto.randomUUID().replace(/-/g, ''),
      };

      if (i === 0 && toolResultOverride) {
        payload.tool_message = toolResultOverride;
      }

      if (nextMessages.length) {
        payload.messages = [...nextMessages];
      }

      const resp = await this.#request('LLM_SCHEDULER', payload, 120000);
      if (resp.status !== 'ok') {
        this.status = 'ERROR';
        throw new Error(resp.error || 'llm request failed');
      }

      if (!resp.tool_calls || !Array.isArray(resp.tool_calls) || resp.tool_calls.length === 0) {
        this.status = 'RUNNING';
        return String(resp.response || '');
      }

      const toolCalls = resp.tool_calls;
      const toolMessages = [];
      let toolExecuted = false;

      for (const toolCall of toolCalls) {
        const name = String(toolCall?.function?.name || toolCall?.name || '').trim();
        if (!name) continue;
        const args = toolCall?.function?.arguments || toolCall?.arguments || {};

        this.status = 'RUNNING';
        const result = await this.executeTool(name, args);
        toolExecuted = true;
        toolMessages.push({
          role: 'tool',
          content: JSON.stringify(result),
          tool_call_id: toolCall.id || toolCall.trace_id || '',
        });
      }

      if (!toolExecuted) {
        this.status = 'RUNNING';
        return String(resp.response || '');
      }

      nextMessages = toolMessages;
    }

    this.status = 'ERROR';
    return 'Failure: Agent state machine exceeded max iterations.';
  }

  #extractToolCalls(reply) {
    const out = [];

    if (Array.isArray(reply?.exec_blocks)) {
      for (const block of reply.exec_blocks) {
        if (block && typeof block === 'object' && !Array.isArray(block)) {
          out.push(block);
          continue;
        }
        if (typeof block === 'string') {
          const raw = block.trim();
          if (!raw) continue;
          try {
            const parsed = JSON.parse(raw);
            if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
              out.push(parsed);
            }
          } catch (_) {}
        }
      }
    }

    if (Array.isArray(reply?.tool_calls)) {
      for (const call of reply.tool_calls) {
        if (call && typeof call === 'object') {
          out.push(call);
        }
      }
    }

    const responseText = String(reply?.response || '');
    let searchAt = 0;
    while (true) {
      const start = responseText.indexOf('EXEC', searchAt);
      if (start === -1) break;
      const end = responseText.indexOf('END_EXEC', start);
      if (end === -1) break;

      const raw = responseText.slice(start + 4, end).trim();
      if (raw) {
        try {
          const parsed = JSON.parse(raw);
          if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
            out.push(parsed);
          }
        } catch (_) {}
      }
      searchAt = end + 'END_EXEC'.length;
    }

    return out;
  }

  async executeTool(name, params) {
    const traceId = crypto.randomUUID().replace(/-/g, '');
    const req = {
      message_type: 'EXEC_VELIX_PROCESS',
      trace_id: traceId,
      tree_id: this.treeId,
      source_pid: this.velixPid,
      name,
      params,
    };
    if (this.userId) {
      req.user_id = this.userId;
    }
    const ack = await this.#request('EXECUTIONER', req, 5000);
    if (ack.status !== 'ok') {
      throw new Error(ack.error || 'executioner rejected');
    }

    if (this.responses.has(traceId)) {
      const payload = this.responses.get(traceId);
      this.responses.delete(traceId);
      if (payload && payload.error === 'child_terminated') {
        throw new Error(`child terminated: ${JSON.stringify(payload)}`);
      }
      return payload || {};
    }

    return await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        const waiters = this.pendingToolWaiters.get(traceId) || [];
        this.pendingToolWaiters.set(
          traceId,
          waiters.filter((w) => w.resolve !== resolve)
        );
        reject(new Error('tool result timeout'));
      }, 60 * 60 * 1000);

      const waiter = {
        resolve: (payload) => {
          clearTimeout(timeout);
          if (payload && payload.error === 'child_terminated') {
            reject(new Error(`child terminated: ${JSON.stringify(payload)}`));
            return;
          }
          resolve(payload || {});
        },
      };

      const waiters = this.pendingToolWaiters.get(traceId) || [];
      waiters.push(waiter);
      this.pendingToolWaiters.set(traceId, waiters);
    });
  }

  reportResult(targetPid, data, traceId = '', append = true) {
    if (!this.busSocket) return;
    const tid = traceId || this.entryTraceId;
    this.#sendFramed(this.busSocket, {
      message_type: 'IPM_RELAY',
      target_pid: targetPid,
      trace_id: tid,
      payload: data,
    });

    if (append) {
      // Logic for internal tool-calling wait-state
      // (Wait, Node.js uses responses/waiters map directly in executeTool)
    } else {
      // Async: remove trace from waiters so executeTool stops waiting.
      if (tid) {
        this.pendingToolWaiters.delete(tid);
        this.responses.delete(tid);
      }
    }
  }

  async #connectBus() {
    const port = this.#getPort('BUS', 5174);
    this.busSocket = await this.#connect('127.0.0.1', port, 5000);
    await this.#sendAndRecv(this.busSocket, { message_type: 'BUS_REGISTER', pid: this.velixPid });

    let buffer = Buffer.alloc(0);
    this.busSocket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      while (buffer.length >= 4) {
        const size = buffer.readUInt32BE(0);
        if (buffer.length < 4 + size) break;
        const body = buffer.slice(4, 4 + size);
        buffer = buffer.slice(4 + size);
        try {
          const msg = JSON.parse(body.toString('utf-8'));
          if (msg.message_type === 'IPM_PUSH' || msg.message_type === 'CHILD_TERMINATED') {
            const traceId = msg.trace_id || '';
            if (traceId) {
              const payload = msg.payload || {};
              const waiters = this.pendingToolWaiters.get(traceId);
              if (waiters && waiters.length) {
                this.pendingToolWaiters.delete(traceId);
                for (const w of waiters) {
                  try {
                    w.resolve(payload);
                  } catch (_) {}
                }
              } else {
                this.responses.set(traceId, payload);
              }
            }
          }
        } catch (_) {}
      }
    });
  }

  #startHeartbeat() {
    const tick = async () => {
      if (!this.isRunning) return;
      try {
        await this.#request('SUPERVISOR', {
          message_type: 'HEARTBEAT',
          pid: this.velixPid,
          payload: { status: 'RUNNING', memory_mb: 0 },
        }, 3000);
      } catch (_) {}
      setTimeout(tick, 5000);
    };
    setTimeout(tick, 5000);
  }

  async #request(service, payload, timeoutMs) {
    const port = this.#getPort(service, 0);
    if (!port) throw new Error(`invalid port for ${service}`);
    const sock = await this.#connect('127.0.0.1', port, timeoutMs);
    try {
      return await this.#sendAndRecv(sock, payload);
    } finally {
      sock.destroy();
    }
  }

  #connect(host, port, timeoutMs) {
    return new Promise((resolve, reject) => {
      const sock = net.createConnection({ host, port });
      sock.setTimeout(timeoutMs);
      sock.once('connect', () => resolve(sock));
      sock.once('timeout', () => reject(new Error('socket timeout')));
      sock.once('error', reject);
    });
  }

  #sendAndRecv(sock, payload) {
    return new Promise((resolve, reject) => {
      let buffer = Buffer.alloc(0);
      let done = false;
      const onData = (chunk) => {
        if (done) return;
        buffer = Buffer.concat([buffer, chunk]);
        if (buffer.length < 4) return;
        const size = buffer.readUInt32BE(0);
        if (buffer.length < 4 + size) return;
        const body = buffer.slice(4, 4 + size);
        done = true;
        cleanup();
        try {
          resolve(JSON.parse(body.toString('utf-8')));
        } catch (e) {
          reject(e);
        }
      };
      const onError = (e) => {
        if (done) return;
        done = true;
        cleanup();
        reject(e);
      };
      const onTimeout = () => {
        if (done) return;
        done = true;
        cleanup();
        sock.destroy();
        reject(new Error('socket timeout'));
      };
      const cleanup = () => {
        sock.off('data', onData);
        sock.off('error', onError);
        sock.off('timeout', onTimeout);
      };
      sock.on('data', onData);
      sock.on('error', onError);
      sock.on('timeout', onTimeout);
      this.#sendFramed(sock, payload);
    });
  }

  #sendFramed(sock, payload) {
    const body = Buffer.from(JSON.stringify(payload), 'utf-8');
    const header = Buffer.alloc(4);
    header.writeUInt32BE(body.length, 0);
    sock.write(Buffer.concat([header, body]));
  }

  #getPort(name, fallback) {
    const candidates = ['config/ports.json', '../config/ports.json', 'build/config/ports.json'];
    try {
      for (const p of candidates) {
        if (!fs.existsSync(p)) continue;
        const raw = fs.readFileSync(p, 'utf-8');
        const ports = JSON.parse(raw);
        return Number(ports[name] || fallback);
      }
      return fallback;
    } catch (_) {
      return fallback;
    }
  }

  #parseJson(raw) {
    try {
      const obj = JSON.parse(raw);
      return typeof obj === 'object' && obj ? obj : {};
    } catch (_) {
      return {};
    }
  }
}

module.exports = { VelixProcess };
