using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

/// <summary>Provenance written by the bundled build script next to the product it produces.</summary>
internal sealed class LocalBuildProvenance
{
    public int SchemaVersion { get; set; }
    public string Profile { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string Compiler { get; set; } = "";

    public const string FileName = "local-build.json";
}

/// <summary>The typed product status reported by <c>--check-products</c>.</summary>
internal enum ProductStatus
{
    Absent,

    /// <summary>The binary matches the installed toolkit and the installed game inputs.</summary>
    Current,

    /// <summary>The toolkit that produced the binary is no longer the installed toolkit.</summary>
    ToolkitChanged,

    /// <summary>
    /// The product exists, but the installation cannot prove its provenance or provide the required
    /// toolkit. It must be repaired before it is launched.
    /// </summary>
    Blocked,

    /// <summary>The binary or the support files it needs are missing or unusable.</summary>
    Broken
}

/// <summary>The result of a non-mutating product inspection.</summary>
internal sealed record ProductState(ProductStatus Status, string Detail)
{
    /// <summary>The stable machine-readable status string of the command line.</summary>
    public string Reason => Status switch
    {
        ProductStatus.Absent => "absent",
        ProductStatus.Current => "current",
        ProductStatus.ToolkitChanged => "toolkit-changed",
        ProductStatus.Blocked => "blocked",
        _ => "broken"
    };

    /// <summary>Whether something must be done before this product may be launched.</summary>
    public bool ActionRequired => Status is not ProductStatus.Current and not ProductStatus.Absent;
}

/// <summary>
/// The on-disk shape of an installation, plus the cheap checks a launch performs before starting the game. Everything
/// here is O(1) or a single file hash; the extracted disc tree is never walked.
/// </summary>
internal sealed class Installation
{
    public const string GameProfile = "base";

    public Installation(string root) => Root = FileSystemUtilities.NormalizePath(root);

    public string Root { get; }
    public string ToolkitDirectory => InstalledLayout.Toolkit(Root);
    public string WorkspaceDirectory => InstalledLayout.Workspace(Root);
    public string WorkspaceAssetsDirectory => Path.Combine(WorkspaceDirectory, "Assets");
    public string WorkspaceIconDirectory => Path.Combine(WorkspaceDirectory, "build", "icon");
    public string GameDirectory => Path.Combine(Root, InstalledLayout.GameDirectoryName);
    public string GameExecutable => Path.Combine(GameDirectory, ProductNames.GameExecutableBaseName + ".exe");
    public string GameDataDirectory => Path.Combine(Root, InstalledLayout.GameAssetsDirectoryName, "DATA");
    public string InstallStatePath => Path.Combine(Root, InstalledLayout.InstallStateFileName);
    public string ToolkitStatePath => Path.Combine(Root, ToolkitState.FileName);

    /// <summary>The portable root this installation lives in, or null for an ordinary installation.</summary>
    public string? PortableRootDirectory => PortableRoot.TryFind(Root);

    /// <summary>
    /// The runtime configuration this installation reads: <c>&lt;portable root&gt;\UserData\Config.toml</c>
    /// when portable, otherwise the shared per-user file.
    /// </summary>
    public string ConfigPath => RuntimeConfiguration.ResolveConfigPath(Root);

    /// <summary>The install directory copy of setup that acts as the launcher.</summary>
    public string SetupCopyPath => Path.Combine(Root, ProductInfo.SetupCopyName);

    private const string Toolkit = InstalledLayout.ToolkitDirectoryName + "\\";
    private const string Workspace = InstalledLayout.WorkspaceDirectoryName + "\\";

    private static readonly string[] RequiredToolkitFiles =
    [
        Toolkit + @"Translator\Translator.Cli.exe",
        Toolkit + @"CMake\bin\cmake.exe",
        Toolkit + @"Ninja\ninja.exe",
        Toolkit + @"llvm-mingw\bin\clang-22.exe",
        Toolkit + @"llvm-mingw\bin\ld.lld.exe",
        Toolkit + @"llvm-mingw\bin\x86_64-w64-mingw32-clang.exe",
        Toolkit + @"llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe",
        Toolkit + @"llvm-mingw\bin\x86_64-w64-mingw32-windres.exe",
        Workspace + "LocalBuild.ps1",
        Workspace + "NativeBuildFlags.ps1",
        Workspace + InstalledLayout.ProjectRelativePath,
        Workspace + @"runtime\CMakeLists.txt",
        Workspace + @"runtime\assets\dsp\dsp_coef.bin"
    ];

    // The source trees and pinned dependency roots LocalBuild.ps1 consumes during every native configure/build.
    // Presence is a cheap structural test, not another full toolkit hash on every status check.
    private static readonly string[] RequiredNonEmptyToolkitDirectories =
    [
        Workspace + @"runtime\src",
        Workspace + InstalledLayout.AuroraRelativePath,
        .. InstalledLayout.DependencyNames.Select(name => Workspace + @"Dependencies\" + name)
    ];

    public bool Exists => File.Exists(InstallStatePath);

    /// <summary>
    /// Whether this directory holds an installation at all. A missing or empty directory is not a broken
    /// installation, it is simply nothing.
    /// </summary>
    public bool IsPresent => Directory.Exists(Root) && (Exists || File.Exists(GameExecutable));

    /// <summary>
    /// A cheap structural check for the programs and workspace inputs LocalBuild.ps1 actually invokes.
    /// </summary>
    public bool HasToolkit => RequiredToolkitFiles.All(relative => File.Exists(Path.Combine(Root, relative))) &&
                              RequiredNonEmptyToolkitDirectories.All(relative =>
                                  IsNonEmptyRegularDirectory(Path.Combine(Root, relative)));

    public InstallState? ReadInstallState() => JsonState.TryRead<InstallState>(InstallStatePath);

    /// <summary>The six-character game id in an extracted disc's <c>sys/boot.bin</c>.</summary>
    internal static string ReadDiscGameId(string dataRoot)
    {
        using var stream = File.OpenRead(Path.Combine(dataRoot, "sys", "boot.bin"));
        var id = new byte[6];
        stream.ReadExactly(id);
        return System.Text.Encoding.ASCII.GetString(id);
    }

    internal ToolkitState? ReadToolkitState() => JsonState.TryRead<ToolkitState>(ToolkitStatePath);

    /// <summary>
    /// The identity of the installed toolkit. Inspection never writes or adopts state: a missing toolkit provenance
    /// record must not silently make an existing product look trustworthy.
    /// </summary>
    public string ResolveToolkitFingerprint()
    {
        var state = ReadToolkitState();
        if (state is { SchemaVersion: 2 } && !string.IsNullOrEmpty(state.ToolkitFingerprint) &&
            !string.IsNullOrEmpty(state.ToolkitPackageFingerprint) && HasToolkit)
            return state.ToolkitFingerprint;
        return "";
    }

    /// <summary>The recorded package identity of the installed Toolkit directory, under the same fail-closed rules
    /// as <see cref="ResolveToolkitFingerprint"/>.</summary>
    public string ResolveToolkitPackageFingerprint()
    {
        var state = ReadToolkitState();
        if (state is { SchemaVersion: 2 } && !string.IsNullOrEmpty(state.ToolkitFingerprint) &&
            !string.IsNullOrEmpty(state.ToolkitPackageFingerprint) && HasToolkit)
            return state.ToolkitPackageFingerprint;
        return "";
    }

    public void WriteToolkitState(string fingerprint, string packageFingerprint, string releaseTag,
        string runtimeAssetsFingerprint = "", string translationFingerprint = "",
        string nativeToolchainFingerprint = "") =>
        JsonState.Write(ToolkitStatePath, new ToolkitState
        {
            ToolkitFingerprint = fingerprint,
            ToolkitPackageFingerprint = packageFingerprint,
            RuntimeAssetsFingerprint = runtimeAssetsFingerprint,
            TranslationFingerprint = translationFingerprint,
            NativeToolchainFingerprint = nativeToolchainFingerprint,
            ToolkitReleaseTag = releaseTag
        });

    /// <summary>
    /// Reads the complete product provenance. A product without the current fingerprint schema is intentionally
    /// untrusted rather than guessed from an older, partial provenance document.
    /// </summary>
    public ProductFingerprint? ReadProductFingerprint()
    {
        var recorded = JsonState.TryRead<ProductFingerprint>(Path.Combine(GameDirectory, ProductFingerprint.FileName));
        return recorded is { SchemaVersion: 1 } ? recorded : null;
    }

    public ProductState CheckGame(string toolkitFingerprint)
    {
        var state = CheckGameCore(toolkitFingerprint);
        if (state.Status != ProductStatus.Current) return state;
        var runtimeAssetsError = ValidateCopiedRuntimeAssets(toolkitFingerprint);
        return runtimeAssetsError is null ? state : new ProductState(ProductStatus.Broken, runtimeAssetsError);
    }

    /// <summary>
    /// Everything about the product except its copied support files. Repair inspects those separately because they
    /// are republished from the installed workspace without recompiling.
    /// </summary>
    internal ProductState CheckGameCore(string toolkitFingerprint)
    {
        if (!IsPresent)
            return new ProductState(ProductStatus.Absent, ProductNames.DisplayName + " is not installed here.");
        if (!IsUsableExecutable(GameExecutable))
            return new ProductState(ProductStatus.Broken, "The game executable is missing.");
        if (!HasUsableToolkit(toolkitFingerprint))
            return MissingToolkit();
        var installState = ReadCurrentInstallState();
        if (installState is null)
            return new ProductState(ProductStatus.Blocked,
                "The installation state is missing, unsupported, or belongs to another directory.");
        var fingerprint = ReadProductFingerprint();
        if (fingerprint is null)
            return new ProductState(ProductStatus.Blocked,
                "The game has no current build provenance. Repair it with the current setup.");
        if (!fingerprint.ToolkitFingerprint.Equals(toolkitFingerprint, StringComparison.Ordinal))
            return new ProductState(ProductStatus.ToolkitChanged,
                "The game was produced by a different recompilation toolkit.");
        var provenanceError = ValidateProductProvenance(fingerprint, installState);
        return provenanceError is null
            ? new ProductState(ProductStatus.Current, "")
            : new ProductState(ProductStatus.Blocked, provenanceError);
    }

    internal bool ProductUsesGameInputs(string toolkitFingerprint, string expectedDolSha256)
    {
        var fingerprint = ReadProductFingerprint();
        return fingerprint is not null &&
               fingerprint.ToolkitFingerprint.Equals(toolkitFingerprint, StringComparison.Ordinal) &&
               fingerprint.DolSha256.Equals(expectedDolSha256, StringComparison.OrdinalIgnoreCase);
    }

    private bool HasUsableToolkit(string toolkitFingerprint) =>
        HasToolkit && !string.IsNullOrWhiteSpace(toolkitFingerprint);

    private static ProductState MissingToolkit() =>
        new(ProductStatus.Blocked,
            "The recompilation toolkit is missing or incomplete. Repair the setup before launching or updating the game.");

    private InstallState? ReadCurrentInstallState()
    {
        var state = ReadInstallState();
        if (state is not { SchemaVersion: 1 } || string.IsNullOrWhiteSpace(state.InstallDir)) return null;
        try
        {
            return FileSystemUtilities.PathsEqual(state.InstallDir, Root) ? state : null;
        }
        catch { return null; }
    }

    private string? ValidateProductProvenance(ProductFingerprint fingerprint, InstallState state)
    {
        if (!fingerprint.Profile.Equals(GameProfile, StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(fingerprint.DolSha256) ||
            string.IsNullOrWhiteSpace(fingerprint.ExecutableSha256))
            return "The game's build provenance is incomplete.";

        try
        {
            if (!InputValidation.Sha256File(GameExecutable).Equals(fingerprint.ExecutableSha256,
                    StringComparison.OrdinalIgnoreCase))
                return "The game executable no longer matches its recorded build identity.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return $"The game executable cannot be verified: {ex.Message}";
        }

        var dol = Path.Combine(GameDataDirectory, "sys", "main.dol");
        var fst = Path.Combine(GameDataDirectory, "sys", "fst.bin");
        if (!File.Exists(dol) || !File.Exists(fst))
            return "The installed game assets are incomplete. Run setup again with the disc image.";
        try
        {
            var dolSha = InputValidation.Sha256File(dol);
            if (!dolSha.Equals(fingerprint.DolSha256, StringComparison.OrdinalIgnoreCase) ||
                !dolSha.Equals(state.DolSha256, StringComparison.OrdinalIgnoreCase))
                return "The installed game inputs do not match the build provenance.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return "The installed game inputs cannot be verified: " + ex.Message;
        }

        var local = JsonState.TryRead<LocalBuildProvenance>(Path.Combine(GameDirectory, LocalBuildProvenance.FileName));
        if (local is not { SchemaVersion: 1 } ||
            !local.Profile.Equals(GameProfile, StringComparison.Ordinal) ||
            !local.DolSha256.Equals(fingerprint.DolSha256, StringComparison.OrdinalIgnoreCase))
            return "The game executable has invalid local-build provenance.";

        return null;
    }

    /// <summary>
    /// Runtime support files are copied beside the game rather than embedded in the executable. The installed
    /// toolkit-state is their authoritative release identity; a product with missing or altered copies cannot be
    /// declared launchable merely because its .exe still hashes.
    /// </summary>
    internal string? ValidateCopiedRuntimeAssets(string toolkitFingerprint)
    {
        var toolkitState = ReadToolkitState();
        if (toolkitState is not { SchemaVersion: 2 } ||
            !string.Equals(toolkitState.ToolkitFingerprint, toolkitFingerprint, StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(toolkitState.RuntimeAssetsFingerprint))
        {
            return "The installed runtime-asset provenance is missing or does not belong to the current toolkit.";
        }

        string? failure = null;
        if (!ToolkitFingerprint.ProductRuntimeAssetsMatch(GameDirectory,
                toolkitState.RuntimeAssetsFingerprint, diagnostic: line => failure = line))
        {
            // An access-denied or half-deleted tree must not masquerade as an ordinary hash mismatch.
            return failure ?? "The game's copied runtime assets are missing, corrupt, or stale.";
        }
        return null;
    }

    private static bool IsUsableExecutable(string path)
    {
        try { return File.Exists(path) && new FileInfo(path).Length >= 64 * 1024; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    private static bool IsNonEmptyRegularDirectory(string path)
    {
        try
        {
            if (!Directory.Exists(path)) return false;
            var directory = new DirectoryInfo(path);
            if ((directory.Attributes & FileAttributes.ReparsePoint) != 0) return false;
            using var entries = Directory.EnumerateFileSystemEntries(path).GetEnumerator();
            return entries.MoveNext();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }
}
