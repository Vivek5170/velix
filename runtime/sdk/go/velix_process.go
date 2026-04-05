package velixruntime

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"sync"
	"time"
)

type VelixProcess struct {
	ProcessName  string
	Role         string
	OSPid        int
	VelixPid     int
	TreeID       string
	ParentPid    int
	EntryTraceID string
	UserID       string
	Params       map[string]any
	Status       string

	isRunning bool
	busConn   net.Conn

	mu        sync.Mutex
	responses map[string]map[string]any
	respCond  *sync.Cond
}

func NewVelixProcess(name, role string) *VelixProcess {
	p := &VelixProcess{
		ProcessName: name,
		Role:        role,
		OSPid:       os.Getpid(),
		VelixPid:    -1,
		ParentPid:   readEnvInt("VELIX_PARENT_PID", -1),
		EntryTraceID: os.Getenv("VELIX_TRACE_ID"),
		UserID:      os.Getenv("VELIX_USER_ID"),
		Params:      readEnvJSON("VELIX_PARAMS"),
		Status:      "STARTING",
		responses:   map[string]map[string]any{},
	}
	p.respCond = sync.NewCond(&p.mu)
	return p
}

func (p *VelixProcess) Start(run func(*VelixProcess) error) error {
	if p.isRunning {
		return nil
	}
	p.isRunning = true

	reg := map[string]any{
		"message_type": "REGISTER_PID",
		"payload": map[string]any{
			"register_intent": ternary(p.ParentPid <= 0, "NEW_TREE", "JOIN_PARENT_TREE"),
			"role":            p.Role,
			"os_pid":          p.OSPid,
			"process_name":    p.ProcessName,
			"trace_id":        p.EntryTraceID,
			"status":          "STARTING",
			"memory_mb":       0,
		},
	}
	if p.ParentPid > 0 {
		reg["source_pid"] = p.ParentPid
	}

	reply, err := p.request("SUPERVISOR", reg, 5*time.Second)
	if err != nil {
		return err
	}
	if reply["status"] != "ok" {
		return fmt.Errorf("registration failed: %v", reply)
	}

	procObj, _ := reply["process"].(map[string]any)
	p.VelixPid = toInt(procObj["pid"])
	if tree, ok := procObj["tree_id"].(string); ok {
		p.TreeID = tree
	}

	if err := p.connectBus(); err != nil {
		return err
	}
	go p.heartbeatLoop()

	defer p.Shutdown()
	return run(p)
}

func (p *VelixProcess) Shutdown() {
	p.isRunning = false
	if p.busConn != nil {
		_ = p.busConn.Close()
		p.busConn = nil
	}
}

func (p *VelixProcess) CallLLM(convoID, userMessage, systemMessage, userID, mode string) (string, error) {
	return p.callLLMInternal(convoID, userMessage, systemMessage, userID, mode, nil)
}

func (p *VelixProcess) CallLLMResume(convoID string, toolResult map[string]any, userID string) (string, error) {
	return p.callLLMInternal(convoID, "", "", userID, "user_conversation", toolResult)
}

func (p *VelixProcess) callLLMInternal(convoID, userMessage, systemMessage, userID, mode string, toolResultOverride map[string]any) (string, error) {
	if mode == "" {
		if convoID == "" && userID == "" {
			mode = "simple"
		} else if userID != "" {
			mode = "user_conversation"
		} else {
			mode = "conversation"
		}
	}

	basePayload := map[string]any{
		"message_type": "LLM_REQUEST",
		"mode":         mode,
		"tree_id":      p.TreeID,
		"source_pid":   p.VelixPid,
		"priority":     1,
		"convo_id":     convoID,
		"user_id":      userID,
		"owner_pid":    p.VelixPid,
	}

	nextMessages := make([]map[string]any, 0)
	if systemMessage != "" {
		nextMessages = append(nextMessages, map[string]any{"role": "system", "content": systemMessage})
	}
	if userMessage != "" {
		nextMessages = append(nextMessages, map[string]any{"role": "user", "content": userMessage})
	}

	const maxIterations = 15
	for i := 0; i < maxIterations; i++ {
		p.Status = "WAITING_LLM"

		payload := map[string]any{}
		for k, v := range basePayload {
			payload[k] = v
		}
		payload["request_id"] = fmt.Sprintf("req_%d_%d", p.VelixPid, time.Now().UnixNano()%100000000)
		payload["trace_id"] = fmt.Sprintf("trace_%d", time.Now().UnixNano())

		if i == 0 && toolResultOverride != nil {
			payload["tool_message"] = toolResultOverride
		}

		if len(nextMessages) > 0 {
			payload["messages"] = nextMessages
		}

		resp, err := p.request("LLM_SCHEDULER", payload, 120*time.Second)
		if err != nil {
			p.Status = "ERROR"
			return "", err
		}
		if resp["status"] != "ok" {
			p.Status = "ERROR"
			return "", fmt.Errorf("scheduler rejected: %v", resp["error"])
		}

		toolCallsRaw, _ := resp["tool_calls"].([]any)
		if len(toolCallsRaw) == 0 {
			p.Status = "RUNNING"
			if s, ok := resp["response"].(string); ok {
				return s, nil
			}
			return "", nil
		}

		toolMessages := make([]map[string]any, 0)
		toolExecuted := false

		for _, item := range toolCallsRaw {
			call, ok := item.(map[string]any)
			if !ok {
				continue
			}

			fn, _ := call["function"].(map[string]any)
			name, _ := fn["name"].(string)
			if name == "" {
				name, _ = call["name"].(string)
			}
			if name == "" {
				continue
			}

			args, _ := fn["arguments"].(map[string]any)
			if args == nil {
				args, _ = call["arguments"].(map[string]any)
			}
			if args == nil {
				args = map[string]any{}
			}

			p.Status = "RUNNING"
			res, err := p.ExecuteTool(name, args)
			if err != nil {
				p.Status = "ERROR"
				return "", err
			}
			toolExecuted = true

			callID, _ := call["id"].(string)
			if callID == "" {
				callID, _ = call["trace_id"].(string)
			}

			resJSON, _ := json.Marshal(res)
			toolMessages = append(toolMessages, map[string]any{
				"role":         "tool",
				"content":      string(resJSON),
				"tool_call_id": callID,
			})
		}

		if !toolExecuted {
			p.Status = "RUNNING"
			if s, ok := resp["response"].(string); ok {
				return s, nil
			}
			return "", nil
		}

		nextMessages = toolMessages
	}

	p.Status = "ERROR"
	return "Failure: Agent state machine exceeded max iterations.", nil
}

func extractToolCalls(reply map[string]any) []map[string]any {
	out := make([]map[string]any, 0)

	if raw, ok := reply["exec_blocks"].([]any); ok {
		for _, item := range raw {
			switch block := item.(type) {
			case map[string]any:
				out = append(out, block)
			case string:
				var parsed map[string]any
				if err := json.Unmarshal([]byte(block), &parsed); err == nil && parsed != nil {
					out = append(out, parsed)
				}
			}
		}
	}

	if raw, ok := reply["tool_calls"].([]any); ok {
		for _, item := range raw {
			if call, ok := item.(map[string]any); ok {
				out = append(out, call)
			}
		}
	}

	responseText, _ := reply["response"].(string)
	searchAt := 0
	for {
		start := indexOf(responseText, "EXEC", searchAt)
		if start < 0 {
			break
		}
		end := indexOf(responseText, "END_EXEC", start)
		if end < 0 {
			break
		}

		raw := responseText[start+len("EXEC") : end]
		var parsed map[string]any
		if err := json.Unmarshal([]byte(raw), &parsed); err == nil && parsed != nil {
			out = append(out, parsed)
		}
		searchAt = end + len("END_EXEC")
	}

	return out
}

func indexOf(s, substr string, from int) int {
	if from < 0 {
		from = 0
	}
	if from >= len(s) {
		return -1
	}
	idx := bytes.Index([]byte(s[from:]), []byte(substr))
	if idx < 0 {
		return -1
	}
	return from + idx
}

func (p *VelixProcess) ExecuteTool(name string, params map[string]any) (map[string]any, error) {
	traceID := fmt.Sprintf("trace_%d", time.Now().UnixNano())
	req := map[string]any{
		"message_type": "EXEC_VELIX_PROCESS",
		"trace_id":     traceID,
		"tree_id":      p.TreeID,
		"source_pid":   p.VelixPid,
		"name":         name,
		"params":       params,
	}
	if p.UserID != "" {
		req["user_id"] = p.UserID
	}
	ack, err := p.request("EXECUTIONER", req, 5*time.Second)
	if err != nil {
		return nil, err
	}
	if ack["status"] != "ok" {
		return nil, fmt.Errorf("executioner rejected: %v", ack)
	}

	deadline := time.Now().Add(60 * time.Minute)
	p.mu.Lock()
	defer p.mu.Unlock()
	for {
		if v, ok := p.responses[traceID]; ok {
			delete(p.responses, traceID)
			if v["error"] == "child_terminated" {
				return nil, fmt.Errorf("child terminated: %v", v)
			}
			return v, nil
		}
		if time.Now().After(deadline) {
			return nil, errors.New("tool result timeout")
		}
		p.respCond.Wait()
	}
}

func (p *VelixProcess) ReportResult(targetPID int, data map[string]any, traceID string, appendToConvo bool) error {
	if p.busConn == nil {
		return nil
	}
	if traceID == "" {
		traceID = p.EntryTraceID
	}

	msg := map[string]any{
		"message_type": "IPM_RELAY",
		"target_pid":   targetPID,
		"trace_id":     traceID,
		"payload":      data,
	}
	err := sendFramed(p.busConn, msg)
	if err != nil {
		return err
	}

	if !appendToConvo && traceID != "" {
		p.mu.Lock()
		delete(p.responses, traceID)
		p.mu.Unlock()
		p.respCond.Broadcast()
	}

	return nil
}

func (p *VelixProcess) connectBus() error {
	conn, err := connect("127.0.0.1", getPort("BUS", 5174), 5*time.Second)
	if err != nil {
		return err
	}
	if err := sendFramed(conn, map[string]any{"message_type": "BUS_REGISTER", "pid": p.VelixPid}); err != nil {
		return err
	}
	_, err = recvFramed(conn)
	if err != nil {
		return err
	}
	p.busConn = conn
	go p.busListenerLoop()
	return nil
}

func (p *VelixProcess) busListenerLoop() {
	for p.isRunning && p.busConn != nil {
		msg, err := recvFramed(p.busConn)
		if err != nil {
			return
		}
		mt, _ := msg["message_type"].(string)
		if mt == "IPM_PUSH" || mt == "CHILD_TERMINATED" {
			traceID, _ := msg["trace_id"].(string)
			payload, _ := msg["payload"].(map[string]any)
			if traceID != "" {
				p.mu.Lock()
				p.responses[traceID] = payload
				p.mu.Unlock()
				p.respCond.Broadcast()
			}
		}
	}
}

func (p *VelixProcess) heartbeatLoop() {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for p.isRunning {
		<-ticker.C
		_, _ = p.request("SUPERVISOR", map[string]any{
			"message_type": "HEARTBEAT",
			"pid":          p.VelixPid,
			"payload":      map[string]any{"status": "RUNNING", "memory_mb": 0},
		}, 3*time.Second)
	}
}

func (p *VelixProcess) request(service string, payload map[string]any, timeout time.Duration) (map[string]any, error) {
	port := getPort(service, 0)
	if port <= 0 {
		return nil, fmt.Errorf("invalid port for %s", service)
	}
	conn, err := connect("127.0.0.1", port, timeout)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	if err := sendFramed(conn, payload); err != nil {
		return nil, err
	}
	return recvFramed(conn)
}

func connect(host string, port int, timeout time.Duration) (net.Conn, error) {
	return net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), timeout)
}

func sendFramed(conn net.Conn, payload map[string]any) error {
	body, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	header := make([]byte, 4)
	binary.BigEndian.PutUint32(header, uint32(len(body)))
	_, err = conn.Write(append(header, body...))
	return err
}

func recvFramed(conn net.Conn) (map[string]any, error) {
	header, err := recvExact(conn, 4)
	if err != nil {
		return nil, err
	}
	size := binary.BigEndian.Uint32(header)
	body, err := recvExact(conn, int(size))
	if err != nil {
		return nil, err
	}
	var out map[string]any
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, err
	}
	return out, nil
}

func getPort(name string, fallback int) int {
	paths := []string{"config/ports.json", "../config/ports.json", "build/config/ports.json"}
	var raw []byte
	var err error
	for _, p := range paths {
		raw, err = os.ReadFile(p)
		if err == nil {
			break
		}
	}
	if err != nil {
		return fallback
	}
	var ports map[string]int
	if err := json.Unmarshal(raw, &ports); err != nil {
		return fallback
	}
	if v, ok := ports[name]; ok {
		return v
	}
	return fallback
}

func readEnvInt(name string, fallback int) int {
	v := os.Getenv(name)
	if v == "" {
		return fallback
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return fallback
	}
	return n
}

func readEnvJSON(name string) map[string]any {
	raw := os.Getenv(name)
	if raw == "" {
		return map[string]any{}
	}
	var out map[string]any
	if err := json.Unmarshal([]byte(raw), &out); err != nil {
		return map[string]any{}
	}
	return out
}

func ternary[T any](cond bool, a, b T) T {
	if cond {
		return a
	}
	return b
}

func toInt(v any) int {
	switch t := v.(type) {
	case float64:
		return int(t)
	case int:
		return t
	default:
		return -1
	}
}

func recvExact(conn net.Conn, n int) ([]byte, error) {
	buf := make([]byte, n)
	total := 0
	for total < n {
		r, err := conn.Read(buf[total:])
		if err != nil {
			return nil, err
		}
		total += r
	}
	return buf, nil
}

func readFramedSafe(conn net.Conn) (map[string]any, error) {
	header, err := recvExact(conn, 4)
	if err != nil {
		return nil, err
	}
	size := binary.BigEndian.Uint32(header)
	body, err := recvExact(conn, int(size))
	if err != nil {
		return nil, err
	}
	var out map[string]any
	decoder := json.NewDecoder(bytes.NewReader(body))
	if err := decoder.Decode(&out); err != nil {
		return nil, err
	}
	return out, nil
}
