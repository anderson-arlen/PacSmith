package acp

import (
	"encoding/json"
	"maps"
	"slices"
)

func (r *runner) sharedDefaults() (map[string]any, error) {
	cfg, err := r.service.Settings(r.service.ctx)
	if err != nil {
		return nil, err
	}
	if p := cfg.Harness; p != nil && p.Executable == r.profile.Executable &&
		slices.Equal(p.Arguments, r.profile.Arguments) && maps.Equal(p.Environment, r.profile.Environment) {
		return p.Defaults, nil
	}
	// Conversations stay pinned to their agent when the configured harness is replaced.
	if r.session() == "" {
		return r.profile.Defaults, nil
	}
	return nil, nil
}

func (r *runner) currentOptions() (map[string]any, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	var options []any
	if err := json.Unmarshal([]byte(r.row.ConfigJson), &options); err != nil {
		return nil, err
	}
	return map[string]any{"configOptions": options}, nil
}

func (r *runner) applyDefaults(result map[string]any, values map[string]any) error {
	defaults := maps.Clone(values)
	for len(defaults) > 0 {
		applied := false
		opts, _ := result["configOptions"].([]any)
		opts = slices.Clone(opts)
		// Model changes can reset reasoning and other model-dependent choices.
		isModel := func(v any) bool {
			o := object(v)
			return o["category"] == "model" || o["id"] == "model"
		}
		slices.SortStableFunc(opts, func(a, b any) int {
			if isModel(a) == isModel(b) {
				return 0
			}
			if isModel(a) {
				return -1
			}
			return 1
		})
		for _, v := range opts {
			o := object(v)
			key := str(o["id"])
			value, ok := defaults[key]
			if !ok {
				continue
			}
			if r.codex && key == "mode" {
				delete(defaults, key)
				continue
			}
			_, boolean := value.(bool)
			if !(o["type"] == "boolean" && boolean) && !accepts(o["options"], value) {
				continue
			}
			delete(defaults, key)
			if o["currentValue"] == value {
				continue
			}
			params := map[string]any{"sessionId": r.session(), "configId": key, "value": value}
			if boolean {
				params["type"] = "boolean"
			}
			var err error
			result, err = r.call("session/set_config_option", params)
			if err != nil {
				return err
			}
			applied = true
			break
		}
		if !applied {
			break
		}
	}
	r.options(result)
	return nil
}
