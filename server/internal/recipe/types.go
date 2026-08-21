package recipe

// IdentityVariablesFile is the per-release identity file sourced by generated
// PKGBUILDs. Its contents come from IdentityVariables.
const IdentityVariablesFile = "pacsmith.vars"

// SourceType is a vendor artifact class. Empty is treated as Debian, matching
// the C++ PackageRelease default.
type SourceType string

const (
	SourceUnknown     SourceType = "not-inspected"
	SourceDebian      SourceType = "deb"
	SourceRPM         SourceType = "rpm"
	SourceArchPackage SourceType = "arch-package"
	SourceArchive     SourceType = "archive"
	SourceAppImage    SourceType = "appimage"
	SourceELF         SourceType = "elf-binary"
)

// AcquisitionKind names match C++ acquisitionKindName.
type AcquisitionKind string

const (
	AcquisitionLocalFile     AcquisitionKind = "local-file"
	AcquisitionDirectURL     AcquisitionKind = "direct-url"
	AcquisitionAptRepository AcquisitionKind = "apt-repository"
	AcquisitionRpmRepository AcquisitionKind = "rpm-repository"
	AcquisitionGitHubRelease AcquisitionKind = "github-release"
)

// MappingStatus names match C++ mappingStatusName.
type MappingStatus string

const (
	MappingUnresolved MappingStatus = "Unresolved"
	MappingResolved   MappingStatus = "Resolved"
	MappingIgnored    MappingStatus = "Ignored"
	MappingBundled    MappingStatus = "Bundled"
	MappingProvided   MappingStatus = "Provided"
)

// ArchiveLayout names match C++ InstallMapping JSON.
type ArchiveLayout string

const (
	ArchiveOptBundle    ArchiveLayout = "opt-bundle"
	ArchivePreserveRoot ArchiveLayout = "preserve-root"
)

// LauncherKind names match C++ LauncherMapping JSON. Empty is a symlink.
type LauncherKind string

const (
	LauncherSymlink LauncherKind = "symlink"
	LauncherWrapper LauncherKind = "wrapper"
)

// Release is the recipe input. It is the subset of C++ PackageRelease that
// Generate and IdentityVariables read. Shared inspect/domain types were not
// present when this package was added.
type Release struct {
	ID                     string
	ProjectID              string
	DisplayName            string
	ArchPackageName        string
	SourceType             SourceType
	Acquisition            Acquisition
	InstallMapping         InstallMapping
	OriginalSourceFilename string
	SourceSHA256           string
	ArchPkgrel             int
	ArchPkgrelOverride     string
	Debian                 DebianMetadata
	Dependencies           []Dependency
	PayloadRules           []PayloadRule
	Lifecycle              LifecycleScript
}

type Acquisition struct {
	Kind              AcquisitionKind
	CanonicalIdentity string
}

type DebianMetadata struct {
	Package      string
	Version      string
	Architecture string
	Description  string
	Homepage     string
}

type Dependency struct {
	ArchPackage string
	Status      MappingStatus
	Ignored     bool
	Bundled     bool
	Provided    bool
}

type PayloadRule struct {
	Path     string
	Excluded bool
}

type LifecycleScript struct {
	FileName         string
	Contents         string
	ValidationPassed bool
}

type InstallMapping struct {
	ArchiveLayout     ArchiveLayout
	OptDirectory      string
	CommonPrefix      string
	StripCommonPrefix bool
	AppImageOffset    int64
	BinaryDestination string
	Launchers         []Launcher
	DesktopEntries    []DesktopEntry
	Icon              Icon
	AppRun            AppRun
}

type Launcher struct {
	// Enabled defaults to false in Go. C++ LauncherMapping.enabled defaults
	// to true; callers must set Enabled when constructing a launcher.
	Enabled     bool
	SourcePath  string
	CommandName string
	Destination string
	Kind        LauncherKind
	Missing     bool
}

type DesktopEntry struct {
	// Enabled defaults to false in Go. C++ DesktopEntryConfiguration.enabled
	// defaults to true; callers must set Enabled when the entry should ship.
	ID          string
	Enabled     bool
	SourcePath  string
	Destination string
	Contents    string
}

type Icon struct {
	ProjectPath string
	SourcePath  string
	SHA256      string
	Format      string
	IconName    string
	Missing     bool
}

type AppRun struct {
	Script           bool
	Contents         string
	OriginalContents string
	UserModified     bool
}

func (r Release) resolvedSourceType() SourceType {
	if r.SourceType == "" {
		return SourceDebian
	}
	return r.SourceType
}

func (r Release) optDirectory() string {
	if r.InstallMapping.OptDirectory != "" {
		return r.InstallMapping.OptDirectory
	}
	return r.ArchPackageName
}

func (m InstallMapping) archiveLayout() ArchiveLayout {
	if m.ArchiveLayout == "" {
		return ArchiveOptBundle
	}
	return m.ArchiveLayout
}

func (l Launcher) kind() LauncherKind {
	if l.Kind == "" {
		return LauncherSymlink
	}
	return l.Kind
}

func (a Acquisition) kindName() string {
	if a.Kind == "" {
		return string(AcquisitionLocalFile)
	}
	return string(a.Kind)
}
