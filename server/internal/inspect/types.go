package inspect

import "time"

type SourceType int

const (
	SourceUnknown SourceType = iota
	SourceDebian
	SourceRPM
	SourceArchPackage
	SourceArchive
	SourceAppImage
	SourceELF
)

func (t SourceType) String() string {
	switch t {
	case SourceDebian:
		return "debian"
	case SourceRPM:
		return "rpm"
	case SourceArchPackage:
		return "arch"
	case SourceArchive:
		return "archive"
	case SourceAppImage:
		return "appimage"
	case SourceELF:
		return "elf"
	default:
		return "unknown"
	}
}

type MappingStatus int

const (
	MappingUnresolved MappingStatus = iota
	MappingResolved
	MappingIgnored
	MappingBundled
	MappingProvided
)

type ValueOrigin int

const (
	OriginUnknown ValueOrigin = iota
	OriginDeterministic
	OriginAI
	OriginUser
)

func (o ValueOrigin) Name() string {
	switch o {
	case OriginDeterministic:
		return "deterministic"
	case OriginAI:
		return "ai"
	case OriginUser:
		return "user"
	default:
		return "unknown"
	}
}

type ScriptDisposition int

const (
	DispositionHandledByPacSmith ScriptDisposition = iota
	DispositionHandledByArch
	DispositionLifecycleRequired
	DispositionNotApplicable
	DispositionUnresolved
)

func (d ScriptDisposition) Name() string {
	switch d {
	case DispositionHandledByPacSmith:
		return "handled-by-pacsmith"
	case DispositionHandledByArch:
		return "handled-by-arch"
	case DispositionLifecycleRequired:
		return "lifecycle-required"
	case DispositionNotApplicable:
		return "not-applicable"
	default:
		return "unresolved"
	}
}

type ArchiveLayout int

const (
	LayoutOptBundle ArchiveLayout = iota
	LayoutPreserveRoot
)

type LauncherKind int

const (
	LauncherSymlink LauncherKind = iota
	LauncherWrapper
)

type IconSourceKind int

const (
	IconNone IconSourceKind = iota
	IconPayload
	IconLocalFile
	IconRemoteURL
)

type FieldProvenance struct {
	Origin            ValueOrigin
	Provider          string
	Model             string
	SourceFingerprint string
	Rationale         string
	Timestamp         time.Time
	UserApproved      bool
}

type Metadata struct {
	Package      string
	Version      string
	Architecture string
	Maintainer   string
	Description  string
	Homepage     string
	Depends      string
	PreDepends   string
	Recommends   string
	Suggests     string
	Conflicts    string
	Provides     string
	RawFields    map[string]string
}

type LauncherMapping struct {
	Enabled           bool
	SourcePath        string
	CommandName       string
	Destination       string
	Kind              LauncherKind
	SourceFingerprint string
	Missing           bool
	Provenance        FieldProvenance
}

type DesktopEntry struct {
	ID                     string
	Enabled                bool
	SourcePath             string
	Destination            string
	Contents               string
	SourceSHA256           string
	OriginalContentsSHA256 string
	Generated              bool
	UserModified           bool
	Missing                bool
	Provenance             FieldProvenance
}

type IconConfiguration struct {
	SourceKind  IconSourceKind
	SourcePath  string
	SourceURL   string
	ProjectPath string
	SHA256      string
	Format      string
	IconName    string
	Missing     bool
	Provenance  FieldProvenance
}

type AppRunConfiguration struct {
	Present                 bool
	Script                  bool
	Contents                string
	OriginalContents        string
	OriginalContentsSHA256  string
	AcknowledgedFingerprint string
	UserModified            bool
	ReviewReason            string
	Provenance              FieldProvenance
}

type InstallMapping struct {
	ArchiveLayout     ArchiveLayout
	OptDirectory      string
	CommonPrefix      string
	StripCommonPrefix bool
	AppImageOffset    int64
	BinarySourcePath  string
	BinaryDestination string
	ExecutableLinks   []string
	Launchers         []LauncherMapping
	DesktopEntries    []DesktopEntry
	Icon              IconConfiguration
	AppRun            AppRunConfiguration
}

type DependencyAlternative struct {
	PackageName     string
	VersionOperator string
	Version         string
}

type Dependency struct {
	RawExpression string
	Alternatives  []DependencyAlternative
	ArchPackage   string
	Status        MappingStatus
	MappingSource string
	Confidence    float64
	UserOverride  bool
	Ignored       bool
	Bundled       bool
	Provided      bool
}

type MaintainerScript struct {
	Name                    string
	Contents                string
	AcknowledgedFingerprint string
}

type ScriptFinding struct {
	ScriptName          string
	Kind                string
	Summary             string
	Evidence            string
	EvidenceFingerprint string
	Disposition         ScriptDisposition
	Provenance          FieldProvenance
}

type PayloadEntry struct {
	Path             string
	Type             string
	SymlinkTarget    string
	Size             int64
	RequiresReview   bool
	ReviewReason     string
	ContentSHA256    string
	TextPreview      string
	PreviewTruncated bool
	Executable       bool
}

type PayloadRule struct {
	Path                    string
	Excluded                bool
	Reason                  string
	UserDecision            bool
	AcknowledgedFingerprint string
}

type AptRepositoryCandidate struct {
	URI           string
	Suite         string
	Components    []string
	Architectures []string
	SignedBy      string
	SourcePath    string
}

type RPMRepositoryCandidate struct {
	BaseURL      string
	Architecture string
	KeyURLs      []string
	SourcePath   string
}

type ExtractedSigningKey struct {
	Contents          []byte
	SourcePath        string
	SourceFingerprint string
}

type ExtractedIcon struct {
	SourcePath string
	Contents   []byte
}

// Analysis is the unified result of inspecting a vendor artifact.
type Analysis struct {
	Type               SourceType
	Metadata           Metadata
	Dependencies       []Dependency
	MaintainerScripts  []MaintainerScript
	ScriptFindings     []ScriptFinding
	Payload            []PayloadEntry
	PayloadRules       []PayloadRule
	UpdateCandidates   []string
	AptCandidates      []AptRepositoryCandidate
	RPMCandidates      []RPMRepositoryCandidate
	SigningKeys        []ExtractedSigningKey
	Icon               *ExtractedIcon
	Install            InstallMapping
	UpstreamArchPkgrel string
}

type scriptEvidence struct {
	Findings      []ScriptFinding
	AptCandidates []AptRepositoryCandidate
	RPMCandidates []RPMRepositoryCandidate
	SigningKeys   []ExtractedSigningKey
}

type iconCandidate struct {
	path     string
	contents []byte
}

type rpmHeaderAnalysis struct {
	Metadata          Metadata
	Dependencies      []Dependency
	MaintainerScripts []MaintainerScript
	ScriptFindings    []ScriptFinding
	FileCapabilities  map[string]string
	payloadOffset     int64
	payloadFormat     string
	payloadCompressor string
}
