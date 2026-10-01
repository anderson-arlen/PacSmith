package library

import "github.com/anderson-arlen/pacsmith/server/internal/recipe"

func validateReleaseLifecycle(document map[string]any, contents string) recipe.LifecycleValidation {
	if stringValue(document, "sourceType") == "arch-package" {
		for _, script := range maintainerScriptsFromDocument(document) {
			if script.Name == ".INSTALL" && script.Contents == contents {
				return recipe.ValidateOriginalArchLifecycle(contents)
			}
		}
	}
	return recipe.ValidateLifecycle(contents)
}
