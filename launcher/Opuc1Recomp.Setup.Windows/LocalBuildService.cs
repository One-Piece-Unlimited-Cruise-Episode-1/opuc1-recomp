using System.Diagnostics;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal sealed class LocalBuildService
{
    private readonly IInstallReporter _reporter;

    public LocalBuildService(IInstallReporter reporter) => _reporter = reporter;

    /// <summary>
    /// Runs the bundled translate-and-compile script against <paramref name="installStaging"/>, which holds a complete
    /// <c>Toolkit</c> and <c>BuildWorkspace</c> pair - either an installation being staged or an existing
    /// installation being repaired in place.
    /// </summary>
    public async Task BuildAsync(string installStaging, string outputDirectory, CancellationToken cancellationToken,
        ToolkitFingerprintComponents? toolkitComponents = null, bool forceCleanBuild = false,
        BuildProgressWindow? progress = null, string? iconDirectory = null)
    {
        var workspace = InstalledLayout.Workspace(installStaging);
        var toolkit = InstalledLayout.Toolkit(installStaging);
        var script = Path.Combine(workspace, "LocalBuild.ps1");
        if (!File.Exists(script)) throw new FileNotFoundException("The local build script is missing.", script);

        var arguments = new List<string>
        {
            "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
            "-Workspace", workspace, "-Toolkit", toolkit,
            "-OutputDirectory", outputDirectory,
            "-X86CpuProfile", HostCpuRequirement.SelectBuildProfile() == X86CpuProfile.V2 ? "v2" : "v3"
        };
        if (iconDirectory is not null && File.Exists(Path.Combine(iconDirectory, ProductIcon.BaseName + ".rc")))
        {
            arguments.Add("-IconDirectory");
            arguments.Add(iconDirectory);
        }
        // The script is the single owner of every cache-reuse decision; these identities are the evidence it
        // compares against the provenance it recorded with the caches themselves. Without them (or with
        // -ForceCleanBuild) it degrades to a full clean translate and build.
        if (toolkitComponents is not null)
        {
            arguments.Add("-TranslationFingerprint");
            arguments.Add(toolkitComponents.Translation);
            arguments.Add("-NativeToolchainFingerprint");
            arguments.Add(toolkitComponents.NativeToolchain);
        }
        if (forceCleanBuild)
            arguments.Add("-ForceCleanBuild");

        // Scrubs the ambient Visual Studio environment so the bundled clang-mingw toolchain is the only one visible.
        // The actual search path is set by LocalBuild.ps1's one canonical definition (shared with
        // Prepare-NativePrebuilt.ps1), not duplicated here, to avoid drift.
        void ScrubEnvironment(ProcessStartInfo start)
        {
            start.WorkingDirectory = workspace;
            foreach (var inherited in new[]
                     { "INCLUDE", "LIB", "LIBPATH", "VSINSTALLDIR", "VCToolsInstallDir", "WindowsSdkDir" })
                start.Environment.Remove(inherited);
        }

        void Observe(string line)
        {
            if (string.IsNullOrWhiteSpace(line)) return;
            if (progress is not null) progress.Observe(line);
            else _reporter.Diagnostic(line);
        }

        var build = await ProcessRunner.RunAsync(
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                "WindowsPowerShell", "v1.0", "powershell.exe"),
            arguments, Observe, cancellationToken, ScrubEnvironment, capture: false,
            ex => _reporter.Diagnostic(
                "The cancelled build process could not be terminated immediately: " + ex.Message));
        if (build.ExitCode != 0)
            throw new InvalidOperationException(
                $"Local recompilation failed with exit code {build.ExitCode}. See the setup log for the failed " +
                "translator or compiler command.");

        var executable = Path.Combine(outputDirectory, ProductNames.GameExecutableBaseName + ".exe");
        if (!File.Exists(executable) || new FileInfo(executable).Length < 64 * 1024)
            throw new InvalidDataException("The local build completed without producing a valid game executable.");
        if (!File.Exists(Path.Combine(outputDirectory, LocalBuildProvenance.FileName)))
            throw new InvalidDataException("The local build did not produce provenance information.");
    }

    /// <summary>
    /// Records what the game was built from: <paramref name="toolkitFingerprint"/> ties it to the
    /// translator/workspace, <paramref name="dolSha256"/> to the translated main.dol, so a later launch or update can
    /// tell whether the binary is still correct.
    /// </summary>
    public static void WriteFingerprint(string outputDirectory, string toolkitFingerprint, string dolSha256)
    {
        var executable = Path.Combine(outputDirectory, ProductNames.GameExecutableBaseName + ".exe");
        if (!File.Exists(executable))
            throw new FileNotFoundException("Cannot record build provenance because the compiled executable is missing.",
                executable);
        var local = JsonState.TryRead<LocalBuildProvenance>(Path.Combine(outputDirectory, LocalBuildProvenance.FileName), StateJsonContext.Default);
        if (local is not { SchemaVersion: 1 } ||
            !string.Equals(local.DolSha256, dolSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The local build did not translate the expected main.dol.");

        JsonState.Write(Path.Combine(outputDirectory, ProductFingerprint.FileName), new ProductFingerprint
        {
            Profile = Installation.GameProfile,
            ToolkitFingerprint = toolkitFingerprint,
            DolSha256 = dolSha256,
            ExecutableSha256 = InputValidation.Sha256File(executable)
        }, StateJsonContext.Default);
    }
}
