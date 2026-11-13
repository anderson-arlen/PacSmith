package acp

import (
	"context"
	"encoding/json"
	"os"
	"testing"
)

const settingsAgent = `import json,sys,uuid
options=[dict(id='reasoning',name='Reasoning',type='select',currentValue='low',options=[dict(value='low',name='Low')]),dict(id='model',name='Model',type='select',currentValue='standard',options=[dict(value='standard',name='Standard'),dict(value='deliberate',name='Deliberate')]),dict(id='fast',name='Fast',type='boolean',currentValue=False)]
def reply(i,r): print(json.dumps(dict(jsonrpc='2.0',id=i,result=r)),flush=True)
for line in sys.stdin:
 m=json.loads(line); method=m.get('method'); p=m.get('params',{})
 if method=='initialize': reply(m['id'],dict(protocolVersion=1,agentCapabilities=dict(loadSession=True)))
 elif method in ('session/new','session/load'):
  session=p['sessionId'] if method=='session/load' else str(uuid.uuid4())
  state=sys.argv[0]+'.'+session
  if method=='session/load':
   with open(state) as f: options=json.load(f)
  with open(state,'w') as f: json.dump(options,f)
  reply(m['id'],dict(sessionId=session,configOptions=options))
 elif method=='session/set_config_option':
  option=next(o for o in options if o['id']==p['configId'])
  if option['type']=='boolean': assert p['type']=='boolean'
  else: assert any(o['value']==p['value'] for o in option['options'])
  option['currentValue']=p['value']
  if p['configId']=='model':
   reasoning=next(o for o in options if o['id']=='reasoning')
   reasoning['currentValue']='low'
   reasoning['options']=[dict(value='low',name='Low')]+([dict(value='high',name='High')] if p['value']=='deliberate' else [])
  with open(state,'w') as f: json.dump(options,f)
  reply(m['id'],dict(configOptions=options))
 elif method=='session/prompt':
  if p['prompt'][0]['text']=='Wait': prompt=m['id']
  else: reply(m['id'],dict(stopReason='end_turn'))
 elif method=='session/cancel': reply(prompt,dict(stopReason='cancelled'))
`

func newSettingsService(t *testing.T) *Service {
	t.Helper()
	s := newTestService(t)
	cfg, err := s.Settings(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(cfg.Harness.Arguments[0], []byte(settingsAgent), 0600); err != nil {
		t.Fatal(err)
	}
	return s
}

func setSharedDefaults(t *testing.T, s *Service, values map[string]any) {
	t.Helper()
	cfg, err := s.Settings(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	cfg.Harness.Defaults = values
	if _, err := s.Configure(context.Background(), cfg); err != nil {
		t.Fatal(err)
	}
}

func assertSettings(t *testing.T, s *Service, id string, want map[string]any) {
	t.Helper()
	values := waitSettings(t, s, id)
	for key, want := range want {
		if values[key] != want {
			t.Errorf("%s = %v, want %v", key, values[key], want)
		}
	}
}

func waitSettings(t *testing.T, s *Service, id string) map[string]any {
	t.Helper()
	values := map[string]any{}
	eventually(t, func() bool {
		row, err := s.Get(context.Background(), id)
		if err != nil {
			t.Fatal(err)
		}
		if row.Status == "failed" {
			t.Fatal(row.Error)
		}
		if row.Status != "idle" {
			return false
		}
		var options []map[string]any
		if err := json.Unmarshal([]byte(row.ConfigJson), &options); err != nil {
			t.Fatal(err)
		}
		for _, option := range options {
			values[str(option["id"])] = option["currentValue"]
		}
		return true
	})
	return values
}

func TestUnstartedConversationUsesLatestDefaults(t *testing.T) {
	s := newSettingsService(t)
	ctx := context.Background()
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	values := map[string]any{"model": "deliberate", "reasoning": "high", "fast": true}
	setSharedDefaults(t, s, values)
	if err := s.Initialize(row.ID); err != nil {
		t.Fatal(err)
	}
	assertSettings(t, s, row.ID, values)
}

func TestExistingConversationUsesSharedSettingsOnNextTurn(t *testing.T) {
	for _, resume := range []bool{false, true} {
		name := "idle"
		if resume {
			name = "resumed"
		}
		t.Run(name, func(t *testing.T) {
			s := newSettingsService(t)
			ctx := context.Background()
			initial := map[string]any{"model": "standard", "reasoning": "low", "fast": false}
			setSharedDefaults(t, s, initial)
			row, err := s.Create(ctx, CreateRequest{})
			if err != nil {
				t.Fatal(err)
			}
			if err := s.Initialize(row.ID); err != nil {
				t.Fatal(err)
			}
			assertSettings(t, s, row.ID, initial)
			if resume {
				s.Close()
				if err := s.Start(ctx); err != nil {
					t.Fatal(err)
				}
			}
			for _, values := range []map[string]any{
				{"model": "deliberate", "reasoning": "high", "fast": true}, initial,
			} {
				setSharedDefaults(t, s, values)
				if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Continue"}}); err != nil {
					t.Fatal(err)
				}
				assertSettings(t, s, row.ID, values)
			}
		})
	}
}

func TestSharedSettingsStayWithTheirAgent(t *testing.T) {
	s := newSettingsService(t)
	ctx := context.Background()
	initial := map[string]any{"model": "deliberate", "reasoning": "high", "fast": true}
	setSharedDefaults(t, s, initial)
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Initialize(row.ID); err != nil {
		t.Fatal(err)
	}
	assertSettings(t, s, row.ID, initial)
	cfg, err := s.Settings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Harness.Environment = map[string]string{"ANOTHER_AGENT": "1"}
	cfg.Harness.Defaults = map[string]any{"model": "standard", "reasoning": "low", "fast": false}
	if _, err := s.Configure(ctx, cfg); err != nil {
		t.Fatal(err)
	}
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Continue"}}); err != nil {
		t.Fatal(err)
	}
	assertSettings(t, s, row.ID, initial)
}

func TestSharedSettingsDoNotChangeRunningTurn(t *testing.T) {
	s := newSettingsService(t)
	ctx := context.Background()
	initial := map[string]any{"model": "standard", "reasoning": "low", "fast": false}
	setSharedDefaults(t, s, initial)
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Wait"}}); err != nil {
		t.Fatal(err)
	}
	eventually(t, func() bool {
		row, _ = s.Get(ctx, row.ID)
		if row.Status == "failed" {
			t.Fatal(row.Error)
		}
		return row.Status == "running"
	})
	values := map[string]any{"model": "deliberate", "reasoning": "high", "fast": true}
	setSharedDefaults(t, s, values)
	unchanged, err := s.Get(ctx, row.ID)
	if err != nil {
		t.Fatal(err)
	}
	if unchanged.Status != "running" || unchanged.ConfigJson != row.ConfigJson {
		t.Fatal("settings changed during an active turn")
	}
	if err := s.Cancel(row.ID); err != nil {
		t.Fatal(err)
	}
	assertSettings(t, s, row.ID, initial)
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Continue"}}); err != nil {
		t.Fatal(err)
	}
	assertSettings(t, s, row.ID, values)
}
