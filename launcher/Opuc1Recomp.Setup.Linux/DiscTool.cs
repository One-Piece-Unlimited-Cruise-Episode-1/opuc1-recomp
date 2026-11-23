using System.Security.Cryptography;

namespace Opuc1Recomp.Setup.Linux;

/// <summary>
/// Validates and extracts the user's own One Piece Unlimited Cruise Episode 1 disc via `nodtool` (see
/// Opuc1Recomp.Setup.Common/NodToolProvider.cs), a prebuilt MIT/Apache-2.0 CLI from encounter/nod.
/// </summary>
internal static class DiscTool
{
    /// <summary>Files an extracted disc must contain, relative to its DATA root.</summary>
    public static readonly string[] RequiredFiles =
    [
        Path.Combine("sys", "main.dol"),
        Path.Combine("sys", "boot.bin"),
        Path.Combine("sys", "fst.bin"),
        ProductIcon.TplRelativePath,
    ];

    /// <summary>Validates and extracts the disc, returning the supported release it is.</summary>
    public static async Task<ProjectManifest> ValidateAndExtractAsync(
        string isoPath, IReadOnlyList<ProjectManifest> releases, string assetsDirectory, string workspace,
        string? nodToolBin, IInstallReporter reporter, CancellationToken cancellationToken)
    {
        if (!File.Exists(isoPath)) throw new FileNotFoundException("The disc image does not exist.", isoPath);
        var nodTool = nodToolBin ?? await NodToolProvider.ResolveAsync(workspace, cancellationToken);

        // `nodtool info` only decodes the disc/partition headers (milliseconds); `nodtool extract` copies the whole
        // data partition. The game id picks the release, so a wrong disc fails fast and an existing extraction is only
        // reused for the same release.
        reporter.Progress(InstallStages.ExtractDisc, "Reading the disc header", 2);
        var info = NodToolInfoParser.Parse(await RunAsync(nodTool, ["info", isoPath], cancellationToken));
        var manifest = releases.FirstOrDefault(release =>
                           string.Equals(info.GameId, release.GameId, StringComparison.Ordinal))
                       ?? throw new InvalidOperationException(
                           $"This disc is '{info.GameId}'. Only the PAL release of {ProductNames.GameTitle} " +
                           $"({ProjectManifest.Describe(releases)}) is supported. Use your own dump of that disc.");

        if (IsAlreadyExtracted(manifest, assetsDirectory))
        {
            reporter.Progress(InstallStages.ExtractDisc, "Reusing the verified extracted disc", 6);
            return manifest;
        }

        // Extracted straight into Assets/DATA (kept, not a scratch dir): the runtime reads the game files from this
        // directory at every launch through [paths] dvd_root, so it has to survive past this install.
        reporter.Progress(InstallStages.ExtractDisc, "Extracting the disc image", 4);
        var dataDir = Path.Combine(assetsDirectory, "DATA");
        Directory.CreateDirectory(assetsDirectory);
        File.Delete(Path.Combine(assetsDirectory, ExtractionMarker));
        if (Directory.Exists(dataDir)) Directory.Delete(dataDir, recursive: true);
        await RunAsync(nodTool, ["extract", isoPath, dataDir, "-q"], cancellationToken);

        ValidateExtracted(dataDir, manifest);
        File.Copy(Path.Combine(dataDir, "sys", "main.dol"), Path.Combine(assetsDirectory, "main.dol"), overwrite: true);
        File.WriteAllText(Path.Combine(assetsDirectory, ExtractionMarker), ExtractionRecord(manifest.DolSha256, dataDir));
        reporter.Progress(InstallStages.ExtractDisc, "Disc validated and extracted", 6);
        return manifest;
    }

    /// <summary>The release already extracted into Assets, chosen by its main.dol like local-build.sh.</summary>
    public static ProjectManifest SelectExtracted(IReadOnlyList<ProjectManifest> releases, string assetsDirectory)
    {
        var dolSha = Sha256Of(Path.Combine(assetsDirectory, "main.dol"));
        var manifest = releases.FirstOrDefault(release => string.Equals(dolSha, release.DolSha256, StringComparison.Ordinal))
                       ?? throw new InvalidOperationException(
                           $"Assets/main.dol (sha256 {dolSha}) is not the clean {ProductNames.GameId} main.dol " +
                           $"({ProjectManifest.Describe(releases)}).");
        ValidateExtracted(Path.Combine(assetsDirectory, "DATA"), manifest);
        return manifest;
    }

    /// <summary>
    /// The extracted tree must be the clean ROUPAF disc: boot.bin names the game, main.dol has the pinned hash, and the
    /// files the runtime and setup need are present.
    /// </summary>
    public static void ValidateExtracted(string dataDir, ProjectManifest manifest)
    {
        var missing = RequiredFiles.Where(relative => !File.Exists(Path.Combine(dataDir, relative))).ToList();
        if (missing.Count > 0)
            throw new InvalidDataException(
                $"The disc extraction is missing required {ProductNames.GameTitle} files: " +
                string.Join(", ", missing.Select(path => path.Replace('\\', '/'))) + ".");

        var bootId = ReadGameId(Path.Combine(dataDir, "sys", "boot.bin"));
        if (!string.Equals(bootId, manifest.GameId, StringComparison.Ordinal))
            throw new InvalidDataException(
                $"sys/boot.bin names {bootId}, not {manifest.GameId}; extract the disc again.");

        var dolSha = Sha256Of(Path.Combine(dataDir, "sys", "main.dol"));
        if (!string.Equals(dolSha, manifest.DolSha256, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"main.dol sha256 mismatch: expected {manifest.DolSha256}, got {dolSha}. " +
                "Patched or otherwise modified game code cannot be installed.");
        }
    }

    private static string ReadGameId(string bootBin)
    {
        using var stream = File.OpenRead(bootBin);
        Span<byte> id = stackalloc byte[6];
        return stream.Read(id) == 6 ? System.Text.Encoding.ASCII.GetString(id) : "";
    }

    private const string ExtractionMarker = ".disc-extracted";

    // Each release has one accepted revision, so a completed, verified extraction is identical to a fresh one.
    private static bool IsAlreadyExtracted(ProjectManifest manifest, string assetsDirectory)
    {
        var marker = Path.Combine(assetsDirectory, ExtractionMarker);
        var dol = Path.Combine(assetsDirectory, "main.dol");
        var dataDir = Path.Combine(assetsDirectory, "DATA");
        if (!File.Exists(marker) || !File.Exists(dol) || !Directory.Exists(Path.Combine(dataDir, "files")))
            return false;
        return string.Equals(File.ReadAllText(marker), ExtractionRecord(manifest.DolSha256, dataDir),
                   StringComparison.Ordinal) &&
               string.Equals(Sha256Of(dol), manifest.DolSha256, StringComparison.Ordinal);
    }

    // The DOL hash plus every extracted file's path and size, so a later deletion or truncation inside DATA forces a
    // fresh extraction.
    private static string ExtractionRecord(string dolSha, string dataDir)
    {
        var record = new System.Text.StringBuilder(dolSha).Append('\n');
        foreach (var path in Directory.EnumerateFiles(dataDir, "*", SearchOption.AllDirectories)
                     .Select(path => Path.GetRelativePath(dataDir, path).Replace('\\', '/'))
                     .Order(StringComparer.Ordinal))
            record.Append(new FileInfo(Path.Combine(dataDir, path)).Length).Append(' ').Append(path).Append('\n');
        return record.ToString();
    }

    public static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static async Task<string> RunAsync(string nodTool, string[] arguments, CancellationToken cancellationToken)
    {
        var startInfo = new System.Diagnostics.ProcessStartInfo(nodTool)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var argument in arguments) startInfo.ArgumentList.Add(argument);
        using var process = System.Diagnostics.Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Failed to start {nodTool}.");
        var stdoutTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderrTask = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        var stdout = await stdoutTask;
        var stderr = await stderrTask;
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"nodtool {arguments[0]} failed (exit {process.ExitCode}): {stderr}{stdout}".Trim());
        }
        return stdout;
    }
}
