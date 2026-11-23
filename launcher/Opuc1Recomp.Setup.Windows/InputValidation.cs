using System.Diagnostics;
using System.Security.Cryptography;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static class InputValidation
{
    private static readonly HashSet<string> SupportedDiscImageExtensions = new(
        [".iso", ".gcm", ".gcz", ".ciso", ".wbfs", ".wia", ".rvz"],
        StringComparer.OrdinalIgnoreCase);

    public static void ValidateExtension(string gamePath)
    {
        if (!File.Exists(gamePath))
            throw new FileNotFoundException("The selected game image does not exist.", gamePath);
        var extension = Path.GetExtension(gamePath);
        if (!SupportedDiscImageExtensions.Contains(extension))
            throw new InvalidDataException(
                "Select a complete Wii disc image in ISO, GCM, GCZ, CISO, WBFS, WIA, or RVZ format.");
    }

    public static async Task<DiscHeader> ReadDiscHeaderAsync(string nodTool, string gamePath,
        CancellationToken cancellationToken = default)
    {
        ValidateExtension(gamePath);
        var result = await ProcessRunner.RunAsync(nodTool,
            ["info", Path.GetFullPath(gamePath)], null, cancellationToken);
        if (result.ExitCode != 0)
            throw new InvalidDataException("nodtool could not read this disc image. " + result.CombinedOutput.Trim());

        var info = NodToolInfoParser.Parse(result.StandardOutput);
        return new DiscHeader
        {
            GameId = info.GameId,
            InternalName = info.Title,
            Region = RegionFromGameId(info.GameId),
            Revision = info.Revision,
        };
    }

    internal static string RegionFromGameId(string gameId) => gameId.Length >= 4
        ? gameId[3] switch
        {
            'P' => "PAL",
            'E' => "NTSC-U",
            'J' => "NTSC-J",
            'K' => "Korea",
            'W' => "Taiwan",
            _ => gameId[3].ToString(),
        }
        : "Unknown";

    /// <summary>The supported release this disc is, or a clear rejection naming the supported one.</summary>
    public static GameRelease EnsureCompatibleDisc(DiscHeader header, PayloadManifest manifest) =>
        manifest.SupportedReleases.FirstOrDefault(release =>
            release.GameId.Equals(header.GameId, StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidDataException(
            "This build supports " + ProductNames.GameTitle + " " +
            string.Join(", ", manifest.SupportedReleases.Select(release =>
                $"{RegionFromGameId(release.GameId)} ({release.GameId})")) +
            $" only. The selected image is {header.GameId} ({header.InternalName}, {header.Region}).");

    public static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }
}

internal sealed record ProcessResult(int ExitCode, string StandardOutput, string StandardError)
{
    public string CombinedOutput => StandardOutput + Environment.NewLine + StandardError;
}

internal static class ProcessRunner
{
    /// <summary>Runs a redirected child process to completion. <paramref name="configure"/> sets up a
    /// working directory or scrubbed environment; <paramref name="capture"/> is off for callers that only
    /// forward output live, so a build's output isn't buffered in memory for nobody to read.</summary>
    public static async Task<ProcessResult> RunAsync(string executable, IReadOnlyList<string> arguments,
        Action<string>? output, CancellationToken cancellationToken,
        Action<ProcessStartInfo>? configure = null, bool capture = true,
        Action<Exception>? onTerminationFailure = null)
    {
        var info = new ProcessStartInfo
        {
            FileName = executable,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        configure?.Invoke(info);

        using var process = new Process { StartInfo = info, EnableRaisingEvents = true };
        var stdout = new List<string>();
        var stderr = new List<string>();
        process.OutputDataReceived += (_, e) => { if (e.Data is not null) { if (capture) stdout.Add(e.Data); output?.Invoke(e.Data); } };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) { if (capture) stderr.Add(e.Data); output?.Invoke(e.Data); } };
        if (!process.Start()) throw new InvalidOperationException($"Could not start {executable}.");
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await WaitForExitAsync(process, cancellationToken, onTerminationFailure);
        return new ProcessResult(process.ExitCode, string.Join(Environment.NewLine, stdout),
            string.Join(Environment.NewLine, stderr));
    }

    public static async Task WaitForExitAsync(Process process, CancellationToken cancellationToken,
        Action<Exception>? onTerminationFailure = null)
    {
        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            try
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
            }
            catch (Exception ex)
            {
                onTerminationFailure?.Invoke(ex);
            }
            await process.WaitForExitAsync(CancellationToken.None);
            process.WaitForExit();
            throw;
        }
        process.WaitForExit();
    }
}
