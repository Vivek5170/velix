# LLM Provider Setup

Velix is model-agnostic and works with any LLM provider that supports OpenAI-compatible chat completions. This guide covers configuration for common providers.

## Quick Reference

All LLM providers require **two key fields** in `config/model.json`:

1. **`base_url`**: The full root endpoint including path (e.g., `http://127.0.0.1:8033/v1`)
2. **`chat_completions_path`**: The chat endpoint path relative to base_url (e.g., `/chat/completions`)

These combine to form the complete endpoint URL: `{base_url}{chat_completions_path}`

**Important**: You must set BOTH fields correctly. If either is missing or incorrect, the system will fail to connect to the LLM provider.

---

## Configuration Structure

Each adapter in `config/model.json` has the following fields:

```json
{
  "adapter_name": {
    "api_style": "openai-chat-completions",
    "base_url": "http://127.0.0.1:8033/v1",
    "api_key": "",
    "host": "127.0.0.1",
    "port": 8033,
    "use_https": false,
    "base_path": "/v1",
    "chat_completions_path": "/chat/completions",
    "model": "your-model-name",
    "enable_tools": true,
    "enable_streaming": true,
    "api_key_env": "",
    "stop_tokens": []
  }
}
```

### Field Descriptions

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `api_style` | string | Yes | API format: `openai-chat-completions`, `ollama-chat`, or provider-specific |
| `base_url` | string | Yes | Full base URL including protocol and path (e.g., `http://localhost:8033/v1`) |
| `api_key` | string | No | API key if required (usually empty, use `api_key_env` instead) |
| `host` | string | Yes | Hostname or IP address |
| `port` | number | Yes | Port number |
| `use_https` | boolean | Yes | Whether to use HTTPS (usually `false` for local) |
| `base_path` | string | Yes | Base path component (e.g., `/v1`, empty for Ollama) |
| `chat_completions_path` | string | Yes | Chat completions endpoint path (e.g., `/chat/completions`) |
| `model` | string | Yes | Model name/ID to use |
| `enable_tools` | boolean | Yes | Whether to enable function calling |
| `enable_streaming` | boolean | Yes | Whether to enable token streaming |
| `api_key_env` | string | No | Environment variable name for API key |
| `stop_tokens` | array | No | Custom stop tokens (usually empty) |

---

## Environment Variables

API keys should be stored in `.env` file rather than directly in `config/model.json`:

- File: `.env` (project root, same level as `README.md`)
- Format: Plain dotenv, one `KEY=VALUE` per line
- No JSON/YAML in `.env`

Example `.env`:

```dotenv
OPENAI_API_KEY=sk-xxxxxxxx
ANTHROPIC_API_KEY=sk-ant-xxxxxxxx
OLLAMA_API_KEY=optional-if-required
```
Set the `api_key_env` field to reference the environment variable:

```json
{
  "openai": {
    "api_key_env": "OPENAI_API_KEY",
    "api_key": ""
  }
}
```

---

## Local Providers

### llama.cpp

High-performance C++ LLM inference server.

**1. Start llama.cpp server:**

```bash
./llama-server -m /absolute/path/to/your-model.gguf --host 127.0.0.1 --port 8033
```

**2. Configure in `config/model.json`:**

```json
{
  "active_adapter": "llama.cpp",
  "adapters": {
    "llama.cpp": {
      "api_style": "openai-chat-completions",
      "base_url": "http://127.0.0.1:8033/v1",
      "api_key": "",
      "host": "127.0.0.1",
      "port": 8033,
      "use_https": false,
      "base_path": "/v1",
      "chat_completions_path": "/chat/completions",
      "model": "your-model-name",
      "enable_tools": true,
      "enable_streaming": true,
      "api_key_env": "",
      "stop_tokens": []
    }
  }
}
```

**Critical fields:**
- `base_url`: `http://127.0.0.1:8033/v1` (includes the `/v1` path)
- `chat_completions_path`: `/chat/completions`
- Combined endpoint: `http://127.0.0.1:8033/v1/chat/completions`

**Notes:**
- Port `8033` is arbitrary; adjust to your setup
- `model` should match your loaded model name
- Supports function calling when `enable_tools: true`

---

### Ollama

Simple, standalone LLM service.

**1. Start Ollama and pull a model:**

```bash
ollama serve
```

In another terminal:

```bash
ollama pull llama3
```

**2. Configure in `config/model.json`:**

```json
{
  "active_adapter": "ollama",
  "adapters": {
    "ollama": {
      "api_style": "ollama-chat",
      "base_url": "http://127.0.0.1:11434",
      "api_key": "",
      "host": "127.0.0.1",
      "port": 11434,
      "use_https": false,
      "base_path": "",
      "chat_completions_path": "/api/chat",
      "model": "llama3",
      "enable_tools": true,
      "enable_streaming": true,
      "api_key_env": "",
      "stop_tokens": []
    }
  }
}
```

**Critical fields:**
- `base_url`: `http://127.0.0.1:11434` (NO path component)
- `base_path`: `` (empty)
- `chat_completions_path`: `/api/chat`
- Combined endpoint: `http://127.0.0.1:11434/api/chat`

**Notes:**
- Ollama listens on port `11434` by default
- `model` must match a pulled model name (e.g., `llama3`, `mistral`)
- `api_style` should be `ollama-chat`

---

## Cloud Providers

### OpenAI

**1. Set API key in `.env`:**

```dotenv
OPENAI_API_KEY=sk-xxxxxxxx
```

**2. Configure in `config/model.json`:**

```json
{
  "active_adapter": "openai",
  "adapters": {
    "openai": {
      "api_style": "openai-chat-completions",
      "base_url": "https://api.openai.com/v1",
      "api_key": "",
      "host": "api.openai.com",
      "port": 443,
      "use_https": true,
      "base_path": "/v1",
      "chat_completions_path": "/chat/completions",
      "model": "gpt-4o",
      "enable_tools": true,
      "enable_streaming": true,
      "api_key_env": "OPENAI_API_KEY",
      "stop_tokens": []
    }
  }
}
```

**Critical fields:**
- `base_url`: `https://api.openai.com/v1` (with /v1 path)
- `chat_completions_path`: `/chat/completions`
- `api_key_env`: `OPENAI_API_KEY`
- `use_https`: `true`

**Notes:**
- Keep `api_key` empty; use `api_key_env` with `.env` file
- Supports all GPT models (gpt-4o, gpt-4-turbo, gpt-3.5-turbo)
- Function calling fully supported

---

### Anthropic

**1. Set API key in `.env`:**

```dotenv
ANTHROPIC_API_KEY=sk-ant-xxxxxxxx
```

**2. Configure in `config/model.json`:**

```json
{
  "active_adapter": "anthropic",
  "adapters": {
    "anthropic": {
      "api_style": "openai-chat-completions",
      "base_url": "https://api.anthropic.com/v1",
      "api_key": "",
      "host": "api.anthropic.com",
      "port": 443,
      "use_https": true,
      "base_path": "/v1",
      "chat_completions_path": "/messages",
      "model": "claude-3-5-sonnet-20241022",
      "enable_tools": true,
      "enable_streaming": true,
      "api_key_env": "ANTHROPIC_API_KEY",
      "stop_tokens": []
    }
  }
}
```

**Critical fields:**
- `base_url`: `https://api.anthropic.com/v1`
- `chat_completions_path`: `/messages`
- `api_key_env`: `ANTHROPIC_API_KEY`
- `use_https`: `true`

**Notes:**
- Anthropic uses different endpoint path (`/messages` instead of `/chat/completions`)
- Supports tool use (function calling)

---

## Custom/Self-Hosted Providers

For any OpenAI-compatible API (vLLM, LM Studio, TGI, etc.):

**1. Identify your endpoint components:**

If your service runs at `http://localhost:5000` with chat completions at `/v1/chat/completions`:
- `host`: `localhost`
- `port`: `5000`
- `base_path`: `/v1`
- `base_url`: `http://localhost:5000/v1`
- `chat_completions_path`: `/chat/completions`

**2. Configure in `config/model.json`:**

```json
{
  "active_adapter": "custom",
  "adapters": {
    "custom": {
      "api_style": "openai-chat-completions",
      "base_url": "http://localhost:5000/v1",
      "api_key": "",
      "host": "localhost",
      "port": 5000,
      "use_https": false,
      "base_path": "/v1",
      "chat_completions_path": "/chat/completions",
      "model": "your-model-name",
      "enable_tools": true,
      "enable_streaming": true,
      "api_key_env": "CUSTOM_API_KEY",
      "stop_tokens": []
    }
  }
}
```

**3. If API key is required, add to `.env`:**

```dotenv
CUSTOM_API_KEY=your-key-here
```

---

## Debugging Connection Issues

### Test Your Endpoint

Before starting Velix, verify the endpoint is reachable:

```bash
# Test llama.cpp
curl http://127.0.0.1:8033/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"test","messages":[{"role":"user","content":"hi"}]}'

# Test Ollama
curl http://127.0.0.1:11434/api/chat \
  -H "Content-Type: application/json" \
  -d '{"model":"llama3","messages":[{"role":"user","content":"hi"}]}'
```

### Common Errors

**"Connection refused"**
- Verify LLM server is running
- Check host and port are correct
- Verify firewall allows the connection

**"404 Not Found" or "Endpoint not found"**
- Verify `chat_completions_path` is correct
- Check `base_url` doesn't duplicate the path component
- Example: If endpoint is `/v1/chat/completions`:
  - `base_url`: `http://localhost:8033/v1`
  - `chat_completions_path`: `/chat/completions`
  - NOT: `base_url`: `http://localhost:8033/v1/chat/completions` (wrong!)

**"Invalid API key"**
- Verify `.env` file exists in project root
- Check `api_key_env` field matches the env var name
- Ensure no trailing whitespace in `.env`

**"Model not found"**
- Verify model name matches loaded model
- For Ollama: run `ollama list` to see available models
- For llama.cpp: ensure model is loaded in server

---

## Switching Providers at Runtime

To switch providers, update `active_adapter` in `config/model.json`:

```json
{
  "active_adapter": "openai"
}
```

Then restart Velix:

```bash
./build/integration_kernel
```

---

## Model Selection Recommendations

### Local (llama.cpp / Ollama)
- Qwen 2.5 - Fast, good reasoning, good tool use
- Mistral - Well-balanced, good reasoning
- Llama 3 - Broad capability, good for general tasks

### Cloud (OpenAI)
- gpt-4o - Best overall capability and tool use
- gpt-4-turbo - Good balance of speed and capability
- gpt-3.5-turbo - Fast, lower cost

### Cloud (Anthropic)
- claude-3-5-sonnet - Best overall capability
- claude-3-opus - More advanced reasoning

---

## Troubleshooting Checklist

- [ ] `base_url` includes protocol (`http://` or `https://`)
- [ ] `base_url` includes the path component (e.g., `/v1`)
- [ ] `chat_completions_path` starts with `/`
- [ ] Combined URL is correct: `{base_url}{chat_completions_path}`
- [ ] Server is running and accessible
- [ ] API key (if required) is set in `.env` file
- [ ] `api_key_env` field matches the `.env` variable name
- [ ] Model name matches the loaded/available model
- [ ] `enable_tools` is `true` if you want function calling
- [ ] `enable_streaming` is `true` for token-by-token output
