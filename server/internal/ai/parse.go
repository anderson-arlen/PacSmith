package ai

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"
	"unicode"
)

func extractResponseText(root map[string]any) string {
	if text := asString(root["output_text"]); text != "" {
		return text
	}
	var result strings.Builder
	for _, output := range asArray(root["output"]) {
		for _, content := range asArray(asObject(output)["content"]) {
			object := asObject(content)
			if text := asString(object["text"]); text != "" {
				result.WriteString(text)
			}
		}
	}
	return result.String()
}

func eventErrorMessage(event map[string]any) string {
	if errObj, ok := event["error"].(map[string]any); ok {
		if message := asString(errObj["message"]); message != "" {
			return message
		}
	}
	if message, ok := event["error"].(string); ok && message != "" {
		return message
	}
	responseError := asObject(asObject(event["response"])["error"])
	if message := asString(responseError["message"]); message != "" {
		return message
	}
	return asString(event["message"])
}

type sseState struct {
	text     string
	err      string
	terminal bool
}

func inspectChatGPTSSE(body []byte) sseState {
	var deltas, completed strings.Builder
	var state sseState
	for _, rawLine := range bytes.Split(body, []byte("\n")) {
		line := bytes.TrimSpace(rawLine)
		if !bytes.HasPrefix(line, []byte("data:")) {
			continue
		}
		line = bytes.TrimSpace(line[5:])
		if len(line) == 0 {
			continue
		}
		if bytes.Equal(line, []byte("[DONE]")) {
			state.terminal = true
			continue
		}
		var event map[string]any
		if err := json.Unmarshal(line, &event); err != nil {
			continue
		}
		typ := asString(event["type"])
		switch typ {
		case "response.output_text.delta":
			deltas.WriteString(asString(event["delta"]))
		case "response.completed":
			completed.WriteString(extractResponseText(asObject(event["response"])))
			state.terminal = true
		case "response.failed", "error":
			state.err = eventErrorMessage(event)
			if state.err == "" {
				state.err = "The provider reported a stream failure without a message"
			}
			state.terminal = true
		default:
			if _, ok := event["output"]; ok {
				completed.WriteString(extractResponseText(event))
			}
		}
	}
	if completed.Len() > 0 {
		state.text = completed.String()
	} else {
		state.text = deltas.String()
	}
	return state
}

func providerErrorMessage(body []byte) string {
	var object map[string]any
	if err := json.Unmarshal(body, &object); err != nil {
		return ""
	}
	if errObj, ok := object["error"].(map[string]any); ok {
		if message := asString(errObj["message"]); message != "" {
			return message
		}
	}
	if message, ok := object["error"].(string); ok {
		return message
	}
	if message := asString(object["message"]); message != "" {
		return message
	}
	return asString(object["detail"])
}

func parseResolutionObject(settings Settings, jsonText []byte) Resolution {
	resolution := Resolution{
		Provider:            settings.Provider,
		Model:               settings.Model,
		InformationRequests: []InformationRequest{},
		Changes:             []FieldChange{},
		FindingResolutions:  []FindingResolution{},
	}
	var object map[string]any
	if err := json.Unmarshal(jsonText, &object); err != nil {
		resolution.Error = fmt.Sprintf("AI output did not match PacSmith's JSON contract: %v", err)
		return resolution
	}
	for _, value := range asArray(object["informationRequests"]) {
		item := asObject(value)
		resolution.InformationRequests = append(resolution.InformationRequests, InformationRequest{
			ID:       asString(item["id"]),
			Kind:     asString(item["kind"]),
			Argument: asString(item["argument"]),
			Reason:   asString(item["reason"]),
		})
	}
	for _, value := range asArray(object["changes"]) {
		item := asObject(value)
		resolution.Changes = append(resolution.Changes, FieldChange{
			Field:     asString(item["field"]),
			Value:     asString(item["value"]),
			Rationale: asString(item["rationale"]),
		})
	}
	for _, value := range asArray(object["findingResolutions"]) {
		item := asObject(value)
		resolution.FindingResolutions = append(resolution.FindingResolutions, FindingResolution{
			EvidenceFingerprint: asString(item["evidenceFingerprint"]),
			Disposition:         asString(item["disposition"]),
			Summary:             asString(item["summary"]),
			Rationale:           asString(item["rationale"]),
		})
	}
	resolution.LifecycleScript = asString(object["lifecycleScript"])
	resolution.Rationale = asString(object["rationale"])
	if len(resolution.InformationRequests) > 0 {
		var details []string
		for _, request := range resolution.InformationRequests {
			details = append(details, fmt.Sprintf("%s: %s — %s", request.Kind, request.Argument, request.Reason))
		}
		resolution.Error = "AI requested follow-up information, but PacSmith reviews are single-request"
		resolution.ErrorDetails = "PacSmith did not execute or send any requested local queries. Items that cannot be resolved from the initial package evidence must remain unresolved.\n\n" +
			strings.Join(details, "\n")
		return resolution
	}
	if asString(object["status"]) != "resolved" {
		resolution.Error = "AI did not return the required resolved status for the single-request review"
		return resolution
	}
	resolution.Success = true
	return resolution
}

func truncatedBody(body []byte, limit int) string {
	if len(body) == 0 {
		return "<empty>"
	}
	visible := body
	suffix := ""
	if len(visible) > limit {
		visible = visible[:limit]
		suffix = ", truncated"
	}
	var formatted bytes.Buffer
	if json.Valid(visible) && json.Compact(&formatted, visible) == nil {
		return formatted.String() + suffix
	}
	cleaned := strings.Map(func(r rune) rune {
		if unicode.IsPrint(r) || unicode.IsSpace(r) {
			return r
		}
		return -1
	}, string(visible))
	return cleaned + suffix
}
