namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// Names of the installed/staged layout. Not cosmetic: payload and toolkit identities hash relative paths that begin
/// with these strings, so changing one (including casing) invalidates every shipped fingerprint. Build-Installer.ps1
/// stages the payload with exactly these names.
/// </summary>
internal static class InstalledLayout
{
    public const string ToolkitDirectoryName = "Toolkit";
    public const string WorkspaceDirectoryName = "BuildWorkspace";
    public const string GameDirectoryName = "Game";
    public const string GameAssetsDirectoryName = "GameAssets";
    public const string InstallStateFileName = "install-state.json";
    public const string PayloadManifestFileName = "payload-manifest.json";

    /// <summary>The one translation project, below the build workspace.</summary>
    public const string ProjectRelativePath = @"projects\opuc1\recomp.yml";

    /// <summary>The aurora source tree, below the build workspace (the repository keeps it in third_party/aurora).</summary>
    public const string AuroraRelativePath = @"third_party\aurora";

    /// <summary>Payload entry names use forward slashes; the archive is written by tar.</summary>
    public const string ToolkitEntryPrefix = ToolkitDirectoryName + "/";
    public const string WorkspaceEntryPrefix = WorkspaceDirectoryName + "/";

    public static string Toolkit(string root) => Path.Combine(root, ToolkitDirectoryName);
    public static string Workspace(string root) => Path.Combine(root, WorkspaceDirectoryName);

    /// <summary>
    /// The pinned offline dependency sources shipped under <c>BuildWorkspace\Dependencies</c>. This is the one list:
    /// Build-Installer.ps1 stages exactly these directories and launcher/Test-PinnedFacts.ps1 fails the release build
    /// if its copy and this one disagree.
    /// </summary>
    public static readonly string[] DependencyNames =
    [
        "abseil-cpp", "dawn_prebuilt", "fmt", "freetype", "imgui", "libusb", "native_prebuilt",
        "png", "SDL", "sqlite3", "tracy", "xxhash", "zlib", "zstd"
    ];
}

/// <summary>
/// Source-owned files copied verbatim beside the game. One inventory drives identity
/// (<see cref="ToolkitFingerprint.ComputeRuntimeAssets"/>), verification, and publication. Renaming an identity key
/// below requires bumping the fingerprint's version salt.
/// </summary>
internal static class ProductRuntimeAssets
{
    /// <summary>Single files, given as their path below <c>runtime/assets</c> and their product name.</summary>
    public static readonly (string[] SourceRelativePath, string ProductFileName)[] Files =
    [
        (["dsp", "dsp_coef.bin"], "dsp_coef.bin")
    ];

    public static string SourceFile(string sourceAssets, string[] relativePath) =>
        Path.Combine([sourceAssets, .. relativePath]);
}
