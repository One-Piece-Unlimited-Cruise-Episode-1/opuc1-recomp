using System.Runtime.InteropServices;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// What a double-click does. Started from the installed copy (the Start menu shortcut, or the copy inside the install
/// folder) it launches the game, repairing it first when needed. Started from a downloaded setup it asks for the disc
/// image, installs or updates, and offers to launch. Everything it does is also reachable through the scripted
/// command line; this only collects the answers a script would pass as options.
/// </summary>
internal static class GuidedSetup
{
    public static int Run()
    {
        try { Console.Title = ProductNames.DisplayName + " setup"; } catch (IOException) { }
        var ownsConsole = OwnsConsole();
        try
        {
            return RunCore();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine("Setup stopped: " + ex.Message);
            Console.Error.WriteLine($"The full log is in {Path.GetTempPath()}.");
            return 1;
        }
        finally
        {
            if (ownsConsole && !_launched)
            {
                Console.Out.WriteLine();
                Console.Out.WriteLine("Press any key to close this window.");
                try { Console.ReadKey(intercept: true); } catch (InvalidOperationException) { }
            }
        }
    }

    private static int RunCore()
    {
        Console.Out.WriteLine($"{ProductNames.DisplayName} setup {ProductInfo.Version}");
        Console.Out.WriteLine(new string('-', 60));

        var hasPayload = HasPayload();
        var runningInstall = new Installation(AppContext.BaseDirectory);
        var installDirectory = runningInstall.IsPresent ? runningInstall.Root : ProductInfo.DefaultInstallDirectory;
        var installation = new Installation(installDirectory);

        if (installation.IsPresent)
        {
            var state = GameLaunchService.InspectGameForLaunch(installation);
            if (state.Status == ProductStatus.Current &&
                (!hasPayload || PayloadMatchesInstall(installation)))
            {
                Console.Out.WriteLine($"Installed in {installation.Root}. Starting the game...");
                return LaunchAndWait(installation.Root);
            }

            if (!hasPayload)
            {
                // The installed copy carries no payload, so all it can do is rebuild from what is installed.
                Console.Out.WriteLine($"The installed game needs to be rebuilt: {state.Detail}");
                if (!Confirm("Rebuild it now? This can take a while."))
                    return 2;
                HostCpuRequirement.EnsureSupported(X86CpuProfile.V2);
                var log = ConsoleCommands.OpenLog(ProductNames.SetupBaseName + "-repair.log", out var logPath);
                Console.Out.WriteLine($"Log: {logPath}");
                ConsoleCommands.RepairAsync(installation.Root, null, new ConsoleInstallReporter(log),
                    CancellationToken.None).GetAwaiter().GetResult();
                return OfferLaunch(installation.Root);
            }

            Console.Out.WriteLine($"An installation was found in {installation.Root}; this setup will update it.");
        }
        else if (!hasPayload)
        {
            Console.Out.WriteLine($"{ProductNames.DisplayName} is not installed in {installation.Root}.");
            Console.Out.WriteLine($"Run the downloaded {ProductInfo.SetupCopyName} to install it.");
            return 2;
        }

        Console.Out.WriteLine();
        Console.Out.WriteLine($"Choose your {ProductNames.GameTitle} disc image ({ProductNames.GameId}: ISO, WBFS, RVZ, ...).");
        var game = AskForFile("Select your " + ProductNames.GameTitle + " disc image",
            "Wii disc images (*.iso;*.wbfs;*.rvz;*.wia;*.gcz;*.ciso;*.gcm)\0*.iso;*.wbfs;*.rvz;*.wia;*.gcz;*.ciso;*.gcm\0" +
            "All files (*.*)\0*.*\0\0", "Disc image path");
        if (game is null) return 2;

        if (!installation.IsPresent)
        {
            Console.Out.WriteLine();
            Console.Out.WriteLine($"Install folder: {installDirectory}");
            Console.Out.Write("Press Enter to accept, or type another folder: ");
            var answer = Console.ReadLine()?.Trim().Trim('"');
            if (!string.IsNullOrWhiteSpace(answer)) installDirectory = Path.GetFullPath(answer);
        }

        string? sysconf = null;
        var plannedSysconf = Path.Combine(SysconfSeed.ResolveNandRoot(installDirectory), SysconfSeed.NandRelativePath);
        if (!SysconfSeed.IsValidSysconf(plannedSysconf))
        {
            var found = SysconfSeed.FindDolphinSysconf();
            if (found is not null)
            {
                Console.Out.WriteLine($"Using the Wii system settings (SYSCONF) from {found}.");
            }
            else
            {
                Console.Out.WriteLine();
                Console.Out.WriteLine("No Wii system settings file (SYSCONF) was found in a Dolphin user folder. The game");
                Console.Out.WriteLine("starts without one. To give it your console's settings, choose the SYSCONF from your");
                Console.Out.WriteLine(@"Wii NAND (/shared2/sys/SYSCONF) or from Dolphin (Dolphin Emulator\Wii\shared2\sys\SYSCONF).");
                // Cancelling the dialog and leaving the prompt empty continues without one.
                sysconf = AskForFile("Select a Wii SYSCONF (optional)", "SYSCONF\0SYSCONF\0All files (*.*)\0*.*\0\0",
                    "SYSCONF path");
            }
        }

        Console.Out.WriteLine();
        Console.Out.WriteLine("Setup now extracts the disc, translates the game and compiles it on this PC.");
        Console.Out.WriteLine("This takes a while the first time (about 20 GB of free space are needed).");
        HostCpuRequirement.EnsureSupported(X86CpuProfile.V2);
        var installLog = ConsoleCommands.OpenLog(ProductNames.SetupBaseName + ".log", out var installLogPath);
        Console.Out.WriteLine($"Log: {installLogPath}");
        using (var cancellationSignal = CancellationSignal.ObserveEnvironment())
        {
            new InstallerEngine(new ConsoleInstallReporter(installLog)).InstallAsync(new InstallOptions
            {
                GamePath = game,
                InstallDirectory = installDirectory,
                SysconfPath = sysconf
            }, cancellationSignal.Token).GetAwaiter().GetResult();
        }
        Console.Out.WriteLine();
        Console.Out.WriteLine($"{ProductNames.DisplayName} is installed in {installDirectory}.");
        Console.Out.WriteLine("Start it from the desktop or Start menu shortcut from now on.");
        return OfferLaunch(installDirectory);
    }

    private static bool _launched;

    /// <summary>
    /// The game shares this console window, so closing the window would end the game. The guided setup therefore
    /// stays with the game until it exits, exactly as the shortcut (<c>--launch</c>) does, and asks for no key press.
    /// </summary>
    private static int LaunchAndWait(string installDirectory)
    {
        _launched = true;
        return GameLaunchService.Launch(installDirectory, waitForExit: true);
    }

    private static int OfferLaunch(string installDirectory)
    {
        if (!Confirm("Start the game now?")) return 0;
        return LaunchAndWait(installDirectory);
    }

    private static bool HasPayload()
    {
        try
        {
            using var payload = PayloadArchive.OpenCurrent();
            return true;
        }
        catch (InvalidDataException)
        {
            return false;
        }
    }

    /// <summary>Whether the installed toolkit is the one this setup carries (an update is due when it is not).</summary>
    private static bool PayloadMatchesInstall(Installation installation)
    {
        using var payload = PayloadArchive.OpenCurrent();
        var manifest = payload.ReadManifest();
        return installation.ResolveToolkitFingerprint().Equals(manifest.ToolkitFingerprint, StringComparison.Ordinal);
    }

    private static bool Confirm(string question)
    {
        Console.Out.Write(question + " [Y/n] ");
        var answer = Console.ReadLine();
        return answer is null || answer.Trim().Length == 0 ||
               answer.Trim().StartsWith("y", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>A file chosen in the Windows file dialog, or typed in the console when the dialog is unavailable.</summary>
    private static string? AskForFile(string title, string filter, string prompt)
    {
        var picked = OpenFileDialog(title, filter);
        if (picked is not null)
        {
            Console.Out.WriteLine($"{prompt}: {picked}");
            return picked;
        }

        Console.Out.Write($"{prompt} (empty to cancel): ");
        var typed = Console.ReadLine()?.Trim().Trim('"');
        if (string.IsNullOrWhiteSpace(typed)) return null;
        if (!File.Exists(typed)) throw new FileNotFoundException("The file does not exist.", typed);
        return Path.GetFullPath(typed);
    }

    private static string? OpenFileDialog(string title, string filter)
    {
        if (Environment.GetEnvironmentVariable("OPUC1_RECOMP_NO_DIALOG") == "1") return null;
        var buffer = Marshal.AllocHGlobal(32768 * sizeof(char));
        var filterPointer = Marshal.StringToHGlobalUni(filter);
        try
        {
            Marshal.WriteInt16(buffer, 0);
            var dialog = new OpenFileName
            {
                lStructSize = Marshal.SizeOf<OpenFileName>(),
                hwndOwner = GetConsoleWindow(),
                lpstrFilter = filterPointer,
                lpstrFile = buffer,
                nMaxFile = 32768,
                lpstrTitle = title,
                Flags = OfnFileMustExist | OfnPathMustExist | OfnNoChangeDir | OfnExplorer
            };
            return GetOpenFileName(ref dialog) ? Marshal.PtrToStringUni(buffer) : null;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return null;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
            Marshal.FreeHGlobal(filterPointer);
        }
    }

    /// <summary>Whether this process created its console window (double-click) rather than inheriting a shell's.</summary>
    private static bool OwnsConsole()
    {
        try { return GetConsoleProcessList(new uint[2], 2) == 1; }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    private const int OfnFileMustExist = 0x00001000;
    private const int OfnPathMustExist = 0x00000800;
    private const int OfnNoChangeDir = 0x00000008;
    private const int OfnExplorer = 0x00080000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct OpenFileName
    {
        public int lStructSize;
        public IntPtr hwndOwner;
        public IntPtr hInstance;
        public IntPtr lpstrFilter;
        public IntPtr lpstrCustomFilter;
        public int nMaxCustFilter;
        public int nFilterIndex;
        public IntPtr lpstrFile;
        public int nMaxFile;
        public IntPtr lpstrFileTitle;
        public int nMaxFileTitle;
        public string? lpstrInitialDir;
        public string? lpstrTitle;
        public int Flags;
        public short nFileOffset;
        public short nFileExtension;
        public string? lpstrDefExt;
        public IntPtr lCustData;
        public IntPtr lpfnHook;
        public string? lpTemplateName;
        public IntPtr pvReserved;
        public int dwReserved;
        public int FlagsEx;
    }

    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetOpenFileName(ref OpenFileName dialog);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetConsoleWindow();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetConsoleProcessList([Out] uint[] processList, uint processCount);
}
