using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;
using Microsoft.Win32;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static partial class ShellIntegration
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
        foreach (var path in ShortcutPaths())
            CreateShortcut(path, cli, "--launch", installDirectory, cli, "Play " + ProductNames.GameTitle);
    }

    /// <summary>
    /// Writes one .lnk through the shell's ShellLink object (IShellLinkW + IPersistFile). The setup ships as
    /// Native AOT, so this uses source-generated COM interop instead of late-bound WScript.Shell automation.
    /// </summary>
    internal static void CreateShortcut(string path, string target, string arguments, string workingDirectory,
        string iconPath, string description)
    {
        using var com = ComApartment.Enter();
        var link = CreateShellLink();
        link.SetPath(target);
        link.SetArguments(arguments);
        link.SetWorkingDirectory(workingDirectory);
        link.SetIconLocation(iconPath, 0);
        link.SetDescription(description);
        ((IPersistFile)link).Save(path, true);
    }

    /// <summary>The target path and arguments a .lnk stores; used by the self-tests to check a written shortcut.</summary>
    internal static (string Target, string Arguments) ReadShortcut(string path)
    {
        using var com = ComApartment.Enter();
        var link = CreateShellLink();
        ((IPersistFile)link).Load(path, 0 /* STGM_READ */);
        var buffer = new char[32768];
        link.GetPath(buffer, buffer.Length, IntPtr.Zero, 0x4 /* SLGP_RAWPATH */);
        var target = new string(buffer, 0, Array.IndexOf(buffer, '\0'));
        Array.Clear(buffer);
        link.GetArguments(buffer, buffer.Length);
        return (target, new string(buffer, 0, Array.IndexOf(buffer, '\0')));
    }

    private static IShellLinkW CreateShellLink()
    {
        var clsid = new Guid("00021401-0000-0000-C000-000000000046"); // CLSID_ShellLink
        var iid = typeof(IShellLinkW).GUID;
        Marshal.ThrowExceptionForHR(CoCreateInstance(clsid, IntPtr.Zero, 1 /* CLSCTX_INPROC_SERVER */, iid, out var unknown));
        try
        {
            return (IShellLinkW)ComWrappers.GetOrCreateObjectForComInstance(unknown, CreateObjectFlags.UniqueInstance);
        }
        finally
        {
            Marshal.Release(unknown);
        }
    }

    private static readonly StrategyBasedComWrappers ComWrappers = new();

    /// <summary>Joins the calling thread to an apartment for the duration of a shell call, if it has none yet.</summary>
    private readonly struct ComApartment : IDisposable
    {
        private readonly bool _initialized;
        private ComApartment(bool initialized) => _initialized = initialized;

        // S_OK / S_FALSE must be balanced by CoUninitialize; RPC_E_CHANGED_MODE means the thread already
        // has an apartment of the other kind, which ShellLink (ThreadingModel=Both) also accepts.
        public static ComApartment Enter() => new(CoInitializeEx(IntPtr.Zero, 0x2 /* COINIT_APARTMENTTHREADED */) >= 0);

        public void Dispose()
        {
            if (_initialized) CoUninitialize();
        }
    }

    [LibraryImport("ole32.dll")]
    private static partial int CoCreateInstance(in Guid rclsid, IntPtr pUnkOuter, uint dwClsContext, in Guid riid, out IntPtr ppv);

    [LibraryImport("ole32.dll")]
    private static partial int CoInitializeEx(IntPtr pvReserved, uint dwCoInit);

    [LibraryImport("ole32.dll")]
    private static partial void CoUninitialize();

    // Vtable order matters: every method of the native interface is declared, in order, including the
    // ones this file never calls.
    [GeneratedComInterface(StringMarshalling = StringMarshalling.Utf16)]
    [Guid("000214F9-0000-0000-C000-000000000046")]
    internal partial interface IShellLinkW
    {
        void GetPath([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] file, int fileLength, IntPtr findData, uint flags);
        void GetIDList(out IntPtr idList);
        void SetIDList(IntPtr idList);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] name, int nameLength);
        void SetDescription(string name);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] directory, int directoryLength);
        void SetWorkingDirectory(string directory);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] arguments, int argumentsLength);
        void SetArguments(string arguments);
        void GetHotkey(out ushort hotkey);
        void SetHotkey(ushort hotkey);
        void GetShowCmd(out int showCommand);
        void SetShowCmd(int showCommand);
        void GetIconLocation([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] iconPath, int iconPathLength, out int iconIndex);
        void SetIconLocation(string iconPath, int iconIndex);
        void SetRelativePath(string relativePath, uint reserved);
        void Resolve(IntPtr window, uint flags);
        void SetPath(string file);
    }

    [GeneratedComInterface(StringMarshalling = StringMarshalling.Utf16)]
    [Guid("0000010B-0000-0000-C000-000000000046")]
    internal partial interface IPersistFile
    {
        void GetClassID(out Guid classId); // IPersist
        [PreserveSig] int IsDirty();
        void Load(string fileName, uint mode);
        void Save(string? fileName, [MarshalAs(UnmanagedType.Bool)] bool remember);
        void SaveCompleted(string fileName);
        void GetCurFile(out IntPtr fileName);
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
