package repo

import "context"

func (s *Service) discardDisabledSoaksLocked(ctx context.Context) error {
	soaks, err := s.DB.Queries.ListActiveSoaks(ctx)
	if err != nil {
		return err
	}
	checked := make(map[string]bool)
	for _, soak := range soaks {
		if !soak.ProjectID.Valid || checked[soak.ProjectID.String] {
			continue
		}
		projectID := soak.ProjectID.String
		checked[projectID] = true
		policy, err := s.projectPolicy(ctx, projectID)
		if err != nil {
			return err
		}
		if !policy.AutomaticSoak {
			// Older builds created timers even with automatic promotion disabled.
			// These never finish and otherwise pin every historical package forever.
			if err := s.DB.Queries.DeleteSoaksForProject(ctx, soak.ProjectID); err != nil {
				return err
			}
		}
	}
	return nil
}
