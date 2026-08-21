package inspect

import "strings"

func InferArchiveLaunchers(mapping *InstallMapping, payload []PayloadEntry, packageName string) bool {
	previous := append([]LauncherMapping(nil), mapping.Launchers...)
	previousSource := mapping.BinarySourcePath
	previousDestination := mapping.BinaryDestination

	var launchers []LauncherMapping
	usedPaths := map[string]struct{}{}
	usedCommands := map[string]struct{}{}
	addLauncher := func(launcher LauncherMapping) {
		if launcher.CommandName != "" {
			usedCommands[launcher.CommandName] = struct{}{}
		}
		if launcher.SourcePath != "" {
			usedPaths[launcher.SourcePath] = struct{}{}
		}
		launchers = append(launchers, launcher)
	}

	for _, desktop := range mapping.DesktopEntries {
		if !desktop.Enabled {
			continue
		}
		command := desktopEntryCommand(desktop.Contents)
		if command == "" {
			continue
		}
		commandName := strings.ToLower(command)
		if _, used := usedCommands[commandName]; used {
			continue
		}
		var best *PayloadEntry
		bestScore := -1
		for i := range payload {
			entry := &payload[i]
			if _, used := usedPaths[entry.Path]; used {
				continue
			}
			score := archiveCommandCandidateScore(*entry, command, mapping.CommonPrefix)
			if score > bestScore {
				bestScore = score
				best = entry
			}
		}
		if best != nil && bestScore >= 0 {
			addLauncher(makeArchiveLauncher(
				best.Path, commandName, true, false,
				"Desktop Exec="+command+" matched inspected payload file "+best.Path))
		} else {
			addLauncher(makeArchiveLauncher(
				"", commandName, true, true,
				"Desktop Exec="+command+" has no matching payload file; review the PATH command"))
		}
	}

	desktopMapped := false
	for _, launcher := range launchers {
		if launcher.Enabled && !launcher.Missing {
			desktopMapped = true
			break
		}
	}

	for i := range payload {
		entry := payload[i]
		if entry.Type != "file" {
			continue
		}
		if _, used := usedPaths[entry.Path]; used || looksLikeLibrary(entry.Path) || ignoredCommandDirectory(entry.Path) {
			continue
		}
		if !archiveLikelyUserCommand(entry.Path) {
			continue
		}
		command := strings.ToLower(lastPathComponent(entry.Path))
		if command == "" {
			continue
		}
		if _, used := usedCommands[command]; used {
			continue
		}
		addLauncher(makeArchiveLauncher(
			entry.Path, command, !isDirectOptApplication(entry.Path), false,
			"Executable command detected in the inspected payload"))
	}

	var extras []*PayloadEntry
	for i := range payload {
		entry := &payload[i]
		if entry.Type != "file" {
			continue
		}
		if _, used := usedPaths[entry.Path]; used || looksLikeLibrary(entry.Path) ||
			ignoredCommandDirectory(entry.Path) || !entry.Executable {
			continue
		}
		extras = append(extras, entry)
	}
	var shallow []*PayloadEntry
	for _, entry := range extras {
		if !strings.Contains(strippedPayloadPath(entry.Path, mapping.CommonPrefix), "/") {
			shallow = append(shallow, entry)
		}
	}
	enableLoneShallow := !desktopMapped && len(launchers) == 0 && len(shallow) == 1
	for _, entry := range extras {
		command := strings.ToLower(lastPathComponent(entry.Path))
		if command == "" {
			continue
		}
		if _, used := usedCommands[command]; used {
			continue
		}
		enable := enableLoneShallow && len(shallow) > 0 && entry == shallow[0]
		rationale := "Inspected executable available for explicit command exposure"
		if enable {
			rationale = "Single inspected executable selected as the package command"
		}
		addLauncher(makeArchiveLauncher(entry.Path, command, enable, false, rationale))
	}

	mapping.Launchers = launchers
	syncLauncherBinaryFields(mapping)
	return launchersDiffer(previous, mapping.Launchers) ||
		previousSource != mapping.BinarySourcePath ||
		previousDestination != mapping.BinaryDestination
}

func archiveCommandCandidateScore(entry PayloadEntry, command, prefix string) int {
	if entry.Type != "file" {
		return -1
	}
	if !strings.EqualFold(lastPathComponent(entry.Path), command) {
		return -1
	}
	if looksLikeLibrary(entry.Path) {
		return -1
	}
	score := 100
	if entry.Executable {
		score += 1000
	}
	if archiveLikelyUserCommand(entry.Path) {
		score += 500
	}
	if ignoredCommandDirectory(entry.Path) {
		score -= 400
	} else {
		score += 200
	}
	score -= strings.Count(strippedPayloadPath(entry.Path, prefix), "/") * 50
	return score
}

func makeArchiveLauncher(sourcePath, command string, enabled, missing bool, rationale string) LauncherMapping {
	return LauncherMapping{
		Enabled:     enabled,
		SourcePath:  sourcePath,
		CommandName: strings.ToLower(command),
		Destination: "/usr/bin/" + strings.ToLower(command),
		Missing:     missing,
		Provenance:  deterministicProvenance("", rationale),
	}
}

func syncLauncherBinaryFields(mapping *InstallMapping) {
	mapping.BinarySourcePath = ""
	mapping.BinaryDestination = ""
	mapping.ExecutableLinks = nil
	for _, launcher := range mapping.Launchers {
		if !launcher.Enabled || launcher.Missing || launcher.SourcePath == "" || launcher.CommandName == "" {
			continue
		}
		mapping.BinarySourcePath = launcher.SourcePath
		mapping.BinaryDestination = launcher.Destination
		mapping.ExecutableLinks = append(mapping.ExecutableLinks, launcher.CommandName)
		break
	}
}

func launchersDiffer(left, right []LauncherMapping) bool {
	if len(left) != len(right) {
		return true
	}
	for i := range left {
		if left[i].Enabled != right[i].Enabled || left[i].Missing != right[i].Missing ||
			left[i].SourcePath != right[i].SourcePath || left[i].CommandName != right[i].CommandName ||
			left[i].Destination != right[i].Destination || left[i].Kind != right[i].Kind {
			return true
		}
	}
	return false
}
