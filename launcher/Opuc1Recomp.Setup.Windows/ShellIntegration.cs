using Microsoft.Win32;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static class ShellIntegration
{
    private const string ShortcutFileName = ProductNames.FileSystemName + ".lnk";

    public static void RegisterUninstaller(string installDirectory)
    {
        using var key = Registry.CurrentUser.CreateSubKey(ProductInfo.UninstallKey, writable: true)
                        ?? throw new InvalidOperationException("Could not register the uninstaller.");
        var cli = Path.Combine(installDirectory, ProductInfo.SetupCopyName);
        key.SetValue("DisplayName", ProductInfo.Name);
        key.SetValue("DisplayVersion", ProductInfo.Version);
        key.SetValue("Publisher", ProductNames.DisplayName + " contributors");
        key.SetValue("InstallLocation", installDirectory);
        key.SetValue("DisplayIcon", cli);
        key.SetValue("UninstallString", $"\"{cli}\" --uninstall --install-dir \"{installDirectory}\"");
        key.SetValue("QuietUninstallString", $"\"{cli}\" --silent-uninstall --install-dir \"{installDirectory}\"");
        key.SetValue("NoModify", 1, RegistryValueKind.DWord);
        key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        key.SetValue("EstimatedSize", EstimateSizeKb(installDirectory), RegistryValueKind.DWord);
        key.SetValue("Comments", "Native PC build of " + ProductNames.GameTitle + ", compiled from your own disc");
    }

    public static void UnregisterUninstaller() =>
        Registry.CurrentUser.DeleteSubKeyTree(ProductInfo.UninstallKey, throwOnMissingSubKey: false);

    /// <summary>Creates the desktop and Start Menu shortcuts that launch the game.</summary>
    public static void CreateShortcuts(string installDirectory)
    {
        var cli = Path.Combine(installDirectory, ProductInfo.SetupCopyName);
        var shellType = Type.GetTypeFromProgID("WScript.Shell")
                        ?? throw new InvalidOperationException("The Windows Script Host shell is unavailable.");
        dynamic shell = Activator.CreateInstance(shellType)!;
        foreach (var path in ShortcutPaths())
        {
            dynamic shortcut = shell.CreateShortcut(path);
            shortcut.TargetPath = cli;
            shortcut.Arguments = "--launch";
            shortcut.WorkingDirectory = installDirectory;
            shortcut.IconLocation = cli + ",0";
            shortcut.Description = "Play " + ProductNames.GameTitle;
            shortcut.Save();
        }
    }

    public static void RemoveShortcuts() => RemoveShortcuts(ShortcutPaths());

    internal static void RemoveShortcuts(IEnumerable<string> shortcutPaths)
    {
        var failures = new List<Exception>();
        foreach (var path in shortcutPaths)
            DeleteFileBestEffort(path, failures);

        if (failures.Count != 0)
            throw new AggregateException("One or more " + ProductNames.DisplayName + " shortcuts could not be removed.", failures);
    }

    private static string[] ShortcutPaths() =>
    [
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ShortcutFileName),
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.StartMenu), "Programs", ShortcutFileName),
    ];

    private static void DeleteFileBestEffort(string path, List<Exception> failures)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception ex)
        {
            failures.Add(new IOException($"Could not delete shortcut {path}: {ex.Message}", ex));
        }
    }

    private static int EstimateSizeKb(string directory)
    {
        try
        {
            var bytes = Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories)
                .Sum(path => new FileInfo(path).Length);
            return (int)Math.Min(int.MaxValue, (bytes + 1023) / 1024);
        }
        catch
        {
            return 0;
        }
    }
}
