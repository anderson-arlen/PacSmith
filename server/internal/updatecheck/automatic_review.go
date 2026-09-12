package updatecheck

import "errors"

type automaticReviewPending struct {
	status  string
	message string
}

func (e *automaticReviewPending) Error() string { return e.message }

func setAutomaticBuildError(result *Result, err error) {
	result.AutomaticStatus = "paused"
	result.AutomaticMessage = err.Error()
	var pending *automaticReviewPending
	if errors.As(err, &pending) {
		result.AutomaticStatus = pending.status
	}
}
