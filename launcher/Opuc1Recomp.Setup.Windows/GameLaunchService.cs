using System.Diagnostics;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static class GameLaunchService
{
    /// <summary>
    /// Launches the installed game after the cheap provenance checks. <paramref name="installDirectory"/> defaults
    /// to the directory of this setup copy, which is what the Start menu shortcut runs.
    /// </summary>
    public static int Launch(string? installDirectory, bool waitForExit = true)
    {
        var installation = new Installation(installDirectory ?? AppContext.BaseDirectory);
        Process process;
        using (InstallOperationLock.Acquire(installation.Root))
        {
            // A portable installation the user moved is reconciled before anything is read from its recorded
            // location. Launch never needs the discarded native build tree.
            PortableInstallHealing.HealMovedInstall(installation,
                new DelegatingInstallReporter(Console.Error.WriteLine));
            EnsureGameIsCurrent(installation);
            process = StartInstalledGame(installation);
        }
        using (process)
        {
            if (!waitForExit) return 0;
            process.WaitForExit();
            return process.ExitCode;
        }
    }

    private static void EnsureGameIsCurrent(Installation installation)
    {
        var state = InspectGameForLaunch(installation);
        if (state.Status == ProductStatus.Current) return;
        if (state.Status == ProductStatus.Absent)
            throw new InvalidDataException(
                $"{ProductNames.DisplayName} is not installed at {installation.Root}. Run setup with your disc image first.");

        throw new InvalidDataException((string.IsNullOrWhiteSpace(state.Detail)
            ? "The game must be repaired before it can be launched."
            : state.Detail) + $" Run setup again (or {ProductInfo.SetupCopyName} --repair-products --install-dir " +
                              $"\"{installation.Root}\").");
    }

    /// <summary>Validates the executable, inputs, build provenance and runtime assets before a launch.</summary>
    internal static ProductState InspectGameForLaunch(Installation installation) =>
        installation.CheckGame(installation.ResolveToolkitFingerprint());

    private static Process StartInstalledGame(Installation installation)
    {
        if (!File.Exists(installation.GameExecutable))
            throw new FileNotFoundException("The installed game is missing. Run setup again.", installation.GameExecutable);
        if (!File.Exists(Path.Combine(installation.GameDataDirectory, "sys", "fst.bin")))
            throw new InvalidDataException(
                $"The installed {ProductNames.GameTitle} game data is missing. Run setup again with the disc image.");

        var info = new ProcessStartInfo
        {
            FileName = installation.GameExecutable,
            WorkingDirectory = installation.GameDirectory,
            UseShellExecute = false
        };
        return Process.Start(info) ?? throw new InvalidOperationException("Could not start the game.");
    }
}
