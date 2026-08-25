package library

import (
	"context"
	"fmt"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
)

func (s *Service) InspectPayloadFile(
	ctx context.Context, releaseID, path string,
) (inspect.PayloadFileInspection, error) {
	release, err := s.GetRelease(ctx, releaseID)
	if err != nil {
		return inspect.PayloadFileInspection{}, err
	}
	found := false
	for _, entry := range objectSlice(release.Document["payload"]) {
		if stringValue(entry, "path") == path {
			found = true
			break
		}
	}
	if !found {
		return inspect.PayloadFileInspection{}, fmt.Errorf("%w: payload path was not found", ErrNotFound)
	}
	sourceID := stringValue(release.Document, "sourceArtifactId")
	if sourceID == "" {
		return inspect.PayloadFileInspection{}, fmt.Errorf("%w: release source artifact is missing", ErrNotFound)
	}
	record, file, err := s.Artifacts.Open(ctx, sourceID)
	if err != nil {
		return inspect.PayloadFileInspection{}, err
	}
	defer file.Close()
	result, err := inspect.InspectPayloadFile(file.Name(), record.OriginalFilename, path)
	if err != nil {
		return inspect.PayloadFileInspection{}, fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}
	return result, nil
}
