using System.Diagnostics;
using System.Runtime.InteropServices;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static class UninstallService
{
    public static void StartWorker(string installDirectory, bool quiet = false)
    {
        var source = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate the uninstaller.");
        var temporary = Path.Combine(Path.GetTempPath(), $"{ProductNames.SetupBaseName}-uninstall-{Guid.NewGuid():N}.exe");
        File.Copy(source, temporary, overwrite: true);
        var info = new ProcessStartInfo
        {
            FileName = temporary,
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        };
        info.ArgumentList.Add("--uninstall-worker");
        info.ArgumentList.Add("--install-dir");
        info.ArgumentList.Add(Path.GetFullPath(installDirectory));
        if (quiet) info.ArgumentList.Add("--quiet");
        _ = Process.Start(info) ?? throw new InvalidOperationException("Could not start the uninstall worker.");
    }

    public static int RunWorker(string installDirectory, bool quiet)
    {
        installDirectory = Path.GetFullPath(installDirectory);
        Thread.Sleep(750);
        TryCleanup("remove shortcuts", quiet, ShellIntegration.RemoveShortcuts);

        // A portable installation never registered an uninstall entry, and the single machine-wide
        // key may belong to a normal installation on the same account. Removing it here would
        // uninstall that one from Windows' point of view.
        bool? isPortable = null;
        TryCleanup("determine whether the installation is portable", quiet,
            () => isPortable = PortableRoot.TryFind(installDirectory) is not null);
        if (isPortable == false)
            TryCleanup("remove the Add/Remove Programs entry", quiet,
                ShellIntegration.UnregisterUninstaller);

        var installation = new Installation(installDirectory);
        string? configPath = null;
        TryCleanup("locate the runtime configuration", quiet,
            () => configPath = RuntimeConfiguration.ResolveConfigPath(installDirectory));

        if (configPath is not null)
        {
            TryCleanup("remove the installed game path from the runtime configuration", quiet,
                () => RuntimeConfiguration.RemoveDvdRootIfOwned(configPath,
                    Path.Combine(installDirectory, InstalledLayout.GameAssetsDirectoryName, "DATA")));
        }

        Exception? lastError = null;
        for (var attempt = 0; attempt < 20; attempt++)
        {
            try
            {
                if (Directory.Exists(installDirectory)) Directory.Delete(installDirectory, recursive: true);
                lastError = null;
                break;
            }
            catch (Exception ex)
            {
                lastError = ex;
                Thread.Sleep(250);
            }
        }

        var current = Environment.ProcessPath;
        if (current is not null) MoveFileEx(current, null, MoveFileFlags.DelayUntilReboot);
        if (lastError is not null)
        {
            if (!quiet)
                Console.Error.WriteLine("Uninstall could not remove every file. " + lastError.Message);
            return 1;
        }

        if (!quiet)
            Console.Out.WriteLine(ProductNames.DisplayName + " was removed from this computer. Your saves and settings stay in " +
                                  (configPath is null ? "the user data folder" : Path.GetDirectoryName(configPath)) + ".");
        return 0;
    }

    private static void TryCleanup(string description, bool quiet, Action cleanup)
    {
        try
        {
            cleanup();
        }
        catch (Exception ex)
        {
            if (!quiet)
                Console.Error.WriteLine($"Uninstall could not {description}; continuing. {ex.Message}");
        }
    }

    [Flags]
    private enum MoveFileFlags : uint { DelayUntilReboot = 0x4 }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileEx(string existingFileName, string? newFileName, MoveFileFlags flags);
}
