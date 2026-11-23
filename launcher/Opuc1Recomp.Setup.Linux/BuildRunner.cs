using System.Diagnostics;

namespace Opuc1Recomp.Setup.Linux;

/// <summary>Toolchain overrides forwarded to the build script (AppRun and the macOS entry point pass the bundled ones).</summary>
internal sealed record BuildTools(
    string? TranslatorBin, string? Cc, string? Cxx, string? FuseLd, string? Cmake, string? Ninja,
    string? NativePrebuiltDir, string? Sysroot);

/// <summary>
/// Invokes launcher/local-build.sh (Linux) or launcher/local-build-macos.command (macOS) and turns its stdout into
/// progress reports. There is no PowerShell dependency here, just bash.
/// </summary>
internal static class BuildRunner
{
    public static async Task RunAsync(
        string workspace, string outputDir, string iconDir, bool forceCleanBuild, BuildTools tools,
        IInstallReporter reporter, CancellationToken cancellationToken)
    {
        // macOS has its own build script (clang from Xcode, an .app bundle) that takes the same flags minus the
        // Linux-only toolchain overrides, which it rejects as unknown options.
        var macOS = OperatingSystem.IsMacOS();
        var script = Path.Combine(workspace, "launcher", macOS ? "local-build-macos.command" : "local-build.sh");
        if (!File.Exists(script)) throw new FileNotFoundException($"{Path.GetFileName(script)} is missing", script);

        var startInfo = new ProcessStartInfo("bash")
        {
            WorkingDirectory = workspace,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        void Add(string option, string? value)
        {
            if (string.IsNullOrEmpty(value)) return;
            startInfo.ArgumentList.Add(option);
            startInfo.ArgumentList.Add(value);
        }

        startInfo.ArgumentList.Add(script);
        Add("--workspace", workspace);
        Add("--output-dir", outputDir);
        Add("--icon-dir", iconDir);
        if (forceCleanBuild) startInfo.ArgumentList.Add("--force-clean-build");
        Add("--translator-bin", tools.TranslatorBin);
        Add("--cmake", tools.Cmake);
        Add("--ninja", tools.Ninja);
        if (!macOS)
        {
            // Forwarded by AppRun so the AppImage's bundled clang/lld (prepare-portable-tools.sh) and the precompiled
            // aurora package (Prepare-NativePrebuilt.sh) are used instead of whatever is on $PATH.
            Add("--cc", tools.Cc);
            Add("--cxx", tools.Cxx);
            Add("--fuse-ld", tools.FuseLd);
            Add("--native-prebuilt-dir", tools.NativePrebuiltDir);
            Add("--sysroot", tools.Sysroot);
        }
        Add("--setup-version", ProductInfo.Version);

        using var process = new Process { StartInfo = startInfo };
        var window = new BuildProgressWindow(reporter, InstallStages.Build, start: 8, end: 96);

        process.OutputDataReceived += (_, e) => { if (e.Data is not null) window.Observe(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) reporter.Diagnostic(e.Data); };

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            try { process.Kill(entireProcessTree: true); } catch { /* best-effort */ }
            throw;
        }

        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"{Path.GetFileName(script)} failed (exit {process.ExitCode}). See the diagnostics above.");
        }
    }
}
