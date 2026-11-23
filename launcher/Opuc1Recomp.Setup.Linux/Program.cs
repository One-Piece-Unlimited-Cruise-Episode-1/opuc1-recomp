using System.Runtime.InteropServices;

namespace Opuc1Recomp.Setup.Linux;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        // Checked anywhere in argv, not just args[0]: AppRun (launcher/build-appimage.sh) and the macOS entry point
        // prepend --workspace <cache> and the bundled tool paths ahead of whatever the caller passed.
        if (args.Contains("-h") || args.Contains("--help")) { PrintUsage(); return 0; }
        if (args.Contains("--version")) { Console.WriteLine(ProductInfo.Version); return 0; }

        using var cts = new CancellationTokenSource();
        // SIGINT/SIGTERM are the portable way for a parent (a shell or a frontend) to cancel this process and the
        // build it spawned.
        using var sigint = PosixSignalRegistration.Create(PosixSignal.SIGINT,
            context => { context.Cancel = true; cts.Cancel(); });
        using var sigterm = PosixSignalRegistration.Create(PosixSignal.SIGTERM,
            context => { context.Cancel = true; cts.Cancel(); });
        return await RunAsync(args, cts);
    }

    private static async Task<int> RunAsync(string[] args, CancellationTokenSource cts)
    {
        // The command word is whichever token isn't part of a --flag/value pair, not strictly args[0].
        var (command, flags) = ParseArgs(args);
        if (command is null)
        {
            // Started with no command: from a terminal (double-clicked AppImage with Terminal=true, or typed without
            // arguments) this is the guided flow; anything else - a script, a pipe - gets the usage text.
            if (IsInteractive) return await GuidedAsync(flags, cts.Token);
            PrintUsage();
            return args.Length == 0 ? 0 : 1;
        }

        var progressJson = flags.ContainsKey("progress-json");
        IInstallReporter reporter = progressJson
            ? new NdjsonInstallReporter(ProductInfo.Version)
            : new ConsoleInstallReporter();

        try
        {
            switch (command)
            {
                case "install":
                    await InstallAsync(flags, reporter, cts.Token);
                    break;
                case "uninstall":
                    Uninstall();
                    break;
                case "launch":
                    return Launch();
                case "check-products":
                    CheckProducts();
                    break;
                case "make-icon":
                    MakeIcon(flags);
                    break;
                case "seed-sysconf":
                    SysconfSeed.EnsureSeeded(flags.GetValueOrDefault("install-dir"), NonEmpty(flags, "sysconf"),
                        Console.WriteLine);
                    break;
                default:
                    Console.Error.WriteLine($"Unknown command: {command}");
                    PrintUsage();
                    return 1;
            }
            (reporter as NdjsonInstallReporter)?.Success(InstallDirectory(flags));
            return 0;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("Cancelled.");
            (reporter as NdjsonInstallReporter)?.Failure("cancelled");
            return 130;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"error: {ex.Message}");
            (reporter as NdjsonInstallReporter)?.Failure(ex.Message);
            return 1;
        }
    }

    private static async Task InstallAsync(Dictionary<string, string?> flags, IInstallReporter reporter,
        CancellationToken token)
    {
        // local-build.sh falls back to an x86-64-v2 build when this CPU lacks v3.
        HostCpuRequirement.EnsureSupported(X86CpuProfile.V2);

        var workspace = flags.GetValueOrDefault("workspace") ?? WorkspaceLocator.FindFrom(AppContext.BaseDirectory);
        workspace = Path.GetFullPath(workspace);
        var releases = ProjectManifest.LoadAll(workspace);
        var assetsDir = Path.Combine(workspace, "Assets");
        var installDir = InstallDirectory(flags);

        reporter.Progress(InstallStages.Validate, "Checking prerequisites", 1);
        if (NonEmpty(flags, "game") is { } isoPath)
        {
            await DiscTool.ValidateAndExtractAsync(Path.GetFullPath(isoPath), releases, assetsDir, workspace,
                NonEmpty(flags, "disc-tool-bin"), reporter, token);
        }
        else
        {
            if (!File.Exists(Path.Combine(assetsDir, "main.dol")))
            {
                throw new InvalidOperationException(
                    "No --game disc image was given and no extracted disc is present in the workspace yet. " +
                    $"Pass --game <path to your {ProductNames.GameTitle} (PAL, {ProductNames.GameId}) disc image>.");
            }
            DiscTool.SelectExtracted(releases, assetsDir);
        }

        // The SYSCONF is optional (the game boots without one), but an explicit --sysconf is checked before the long
        // build rather than after it.
        reporter.Progress(InstallStages.Validate, "Checking the Wii system settings (SYSCONF)", 7);
        SysconfSeed.EnsureSeeded(installDir, NonEmpty(flags, "sysconf"), reporter.Diagnostic);

        var dataDir = Path.Combine(assetsDir, "DATA");
        var iconDir = Path.Combine(workspace, "build", "icon");
        reporter.Progress(InstallStages.Validate, "Creating the game icon from the disc", 7);
        ProductIcon.Generate(dataDir, iconDir);

        var sysroot = flags.GetValueOrDefault("sysroot");
        // --sysroot given as a bare flag (ParseArgs stores null) must carry a path; omitting it entirely is fine.
        if (flags.ContainsKey("sysroot") && string.IsNullOrWhiteSpace(sysroot))
            throw new ArgumentException("--sysroot requires a non-empty directory path.");

        var tools = new BuildTools(
            NonEmpty(flags, "translator-bin"), NonEmpty(flags, "cc"), NonEmpty(flags, "cxx"),
            NonEmpty(flags, "fuse-ld"), NonEmpty(flags, "cmake"), NonEmpty(flags, "ninja"),
            NonEmpty(flags, "native-prebuilt-dir"), sysroot);
        await BuildRunner.RunAsync(workspace, installDir, iconDir, flags.ContainsKey("force-clean-build"), tools,
            reporter, token);

        reporter.Progress(InstallStages.Shortcuts, "Creating shortcuts", 98);
        var exeName = ExecutableRelativePath;
        var exePath = Path.Combine(installDir, exeName);
        if (!File.Exists(exePath))
            throw new InvalidDataException($"The build finished without producing {exePath}.");

        var state = JsonState.TryRead<InstallState>(StatePath) ?? new InstallState();
        state.Workspace = workspace;
        state.Game = new ProductInstallRecord
        {
            InstallDirectory = installDir,
            ExecutableName = exeName,
            DolSha256 = DiscTool.Sha256Of(Path.Combine(assetsDir, "main.dol")),
            BuiltUtc = DateTime.UtcNow.ToString("O"),
            SetupVersion = ProductInfo.Version,
        };
        var installedIcon = Path.Combine(installDir, ProductIcon.BaseName + ".png");
        DesktopEntry.Create(exePath, File.Exists(installedIcon) ? installedIcon : null);
        JsonState.Write(StatePath, state);

        // The runtime reads the game files from dvd_root at every launch, not just at translation time.
        RuntimeConfiguration.SetDvdRoot(RuntimeConfiguration.ResolveConfigPath(installDir), dataDir);

        reporter.Progress(InstallStages.Shortcuts, "Install complete", 99);
    }

    private static void Uninstall()
    {
        var state = JsonState.TryRead<InstallState>(StatePath);
        if (state?.Game is { } record)
        {
            if (Directory.Exists(record.InstallDirectory))
                Directory.Delete(record.InstallDirectory, recursive: true);
            Console.WriteLine($"Removed the game from {record.InstallDirectory}");
            if (!string.IsNullOrEmpty(state.Workspace))
            {
                RuntimeConfiguration.RemoveDvdRootIfOwned(
                    RuntimeConfiguration.ResolveConfigPath(record.InstallDirectory),
                    Path.Combine(state.Workspace, "Assets", "DATA"));
            }
            state.Game = null;
            JsonState.Write(StatePath, state);
        }
        else
        {
            Console.WriteLine("Nothing is installed.");
        }
        DesktopEntry.Remove();
        // Saves (NAND), Config.toml and the build workspace stay: they belong to the user, not to one build.
        Console.WriteLine($"Your saves and settings in {UserDataDirectory} were kept.");
    }

    private static int Launch()
    {
        var record = JsonState.TryRead<InstallState>(StatePath)?.Game;
        if (record is null)
        {
            Console.Error.WriteLine("The game is not installed. Run 'install --game <disc image>' first.");
            return 1;
        }
        var exePath = Path.Combine(record.InstallDirectory, record.ExecutableName);
        if (!File.Exists(exePath))
        {
            Console.Error.WriteLine($"The installed executable is missing: {exePath}. Run 'install' again.");
            return 1;
        }
        var startInfo = new System.Diagnostics.ProcessStartInfo(exePath)
        {
            WorkingDirectory = Path.GetDirectoryName(exePath)!,
            UseShellExecute = false,
        };
        using var process = System.Diagnostics.Process.Start(startInfo);
        process?.WaitForExit();
        return process?.ExitCode ?? 1;
    }

    private enum ProductStatus { Absent, Missing, Stale, Current }

    private static (ProductStatus Status, string Detail) InspectProduct()
    {
        var state = JsonState.TryRead<InstallState>(StatePath);
        if (state?.Game is not { } record) return (ProductStatus.Absent, "not installed");
        if (!File.Exists(Path.Combine(record.InstallDirectory, record.ExecutableName)))
            return (ProductStatus.Missing, "the installed executable is missing");
        var dol = Path.Combine(state.Workspace, "Assets", "main.dol");
        if (!File.Exists(dol) || DiscTool.Sha256Of(dol) != record.DolSha256)
            return (ProductStatus.Stale, "the extracted game changed since the last build");
        if (record.SetupVersion != ProductInfo.Version)
            return (ProductStatus.Stale, $"built by setup {record.SetupVersion}, this is {ProductInfo.Version}");
        return (ProductStatus.Current, "current");
    }

    private static void CheckProducts()
    {
        var (status, detail) = InspectProduct();
        var record = JsonState.TryRead<InstallState>(StatePath)?.Game;
        Console.WriteLine(status == ProductStatus.Absent
            ? "Nothing installed."
            : $"game  {status.ToString().ToLowerInvariant(),-8} {detail}  {record?.InstallDirectory}");
    }

    private static void MakeIcon(Dictionary<string, string?> flags)
    {
        var dataRoot = NonEmpty(flags, "data-root") ?? throw new ArgumentException("make-icon requires --data-root DIR.");
        var output = NonEmpty(flags, "out") ?? throw new ArgumentException("make-icon requires --out DIR.");
        Console.WriteLine(ProductIcon.Generate(dataRoot, output));
    }

    /// <summary>
    /// No-argument start from a terminal: launch straight away when the installed game is current, otherwise ask for
    /// the disc image (and a SYSCONF when none can be found), install or update, then offer to launch.
    /// </summary>
    private static async Task<int> GuidedAsync(Dictionary<string, string?> flags, CancellationToken token)
    {
        Console.WriteLine($"{ProductNames.DisplayName} setup {ProductInfo.Version}");
        var (status, detail) = InspectProduct();
        if (status == ProductStatus.Current)
        {
            Console.WriteLine("The game is installed and current. Starting it...");
            return Launch();
        }
        if (status != ProductStatus.Absent) Console.WriteLine($"The installed game needs an update: {detail}.");

        var workspace = flags.GetValueOrDefault("workspace") ?? WorkspaceLocator.FindFrom(AppContext.BaseDirectory);
        var haveExtraction = File.Exists(Path.Combine(workspace, "Assets", "main.dol"));
        Console.WriteLine();
        Console.WriteLine($"This builds {ProductNames.GameTitle} from your own disc image (PAL, {ProductNames.GameId}).");
        Console.WriteLine("ISO, WBFS, RVZ, WIA, GCZ and CISO images are accepted. The first build takes a while.");
        while (true)
        {
            var prompt = haveExtraction
                ? "Disc image path (Enter to reuse the disc already extracted): "
                : "Disc image path: ";
            var game = ReadPath(prompt);
            if (game is null && haveExtraction) break;
            if (game is not null && File.Exists(game)) { flags["game"] = game; break; }
            Console.WriteLine(game is null ? "A disc image is required." : $"No file at {game}.");
        }

        if (!flags.ContainsKey("sysconf") &&
            !SysconfSeed.IsValidSysconf(Path.Combine(SysconfSeed.ResolveNandRoot(null), SysconfSeed.NandRelativePath)) &&
            SysconfSeed.FindDolphinSysconf() is null)
        {
            Console.WriteLine();
            Console.WriteLine("No Wii SYSCONF was found in a Dolphin user folder. The game starts without one; to give it your");
            Console.WriteLine("console settings, enter the SYSCONF from your Wii (NAND /shared2/sys/SYSCONF) or Dolphin.");
            while (true)
            {
                var sysconf = ReadPath("SYSCONF path (empty to continue without): ");
                if (sysconf is null) break;
                if (SysconfSeed.IsValidSysconf(sysconf)) { flags["sysconf"] = sysconf; break; }
                Console.WriteLine($"That is not a Wii SYSCONF (a {SysconfSeed.SysconfLength:N0}-byte file starting with SCv0).");
            }
        }

        var reporter = new ConsoleInstallReporter();
        try
        {
            await InstallAsync(flags, reporter, token);
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("Cancelled.");
            return 130;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"error: {ex.Message}");
            WaitForEnter();
            return 1;
        }

        Console.WriteLine();
        Console.Write("Installation complete. Start the game now? [Y/n] ");
        var answer = Console.ReadLine()?.Trim();
        return string.IsNullOrEmpty(answer) || answer.StartsWith('y') || answer.StartsWith('Y') ? Launch() : 0;
    }

    /// <summary>
    /// Reads a path typed or dropped into the terminal: surrounding quotes and backslash-escaped spaces (what terminals
    /// insert for a dropped file) are removed and a leading ~ is expanded. Empty input returns null.
    /// </summary>
    private static string? ReadPath(string prompt)
    {
        Console.Write(prompt);
        var text = Console.ReadLine()?.Trim();
        if (string.IsNullOrEmpty(text)) return null;
        if (text.Length >= 2 && (text[0] == '\'' || text[0] == '"') && text[^1] == text[0])
            text = text[1..^1];
        else
            text = text.Replace("\\ ", " ");
        if (text == "~" || text.StartsWith("~/", StringComparison.Ordinal))
            text = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + text[1..];
        return Path.GetFullPath(text);
    }

    private static void WaitForEnter()
    {
        Console.Write("Press Enter to close.");
        Console.ReadLine();
    }

    private static bool IsInteractive => !Console.IsInputRedirected && !Console.IsOutputRedirected;

    private static string? NonEmpty(Dictionary<string, string?> flags, string name) =>
        flags.GetValueOrDefault(name) is { Length: > 0 } value ? value : null;

    /// <summary>The runtime's per-user directory (Config.toml, NAND, Logs); this tool keeps its state there too.</summary>
    public static string UserDataDirectory => SysconfSeed.UserDataDirectory(null);

    private static string StatePath => Path.Combine(UserDataDirectory, "install-state.json");

    private static string InstallDirectory(Dictionary<string, string?> flags) =>
        Path.GetFullPath(NonEmpty(flags, "install-dir") ?? Path.Combine(UserDataDirectory, "Install", "Game"));

    /// <summary>The game binary relative to the install directory; macOS builds an .app bundle (publish-app.command).</summary>
    private static string ExecutableRelativePath => OperatingSystem.IsMacOS()
        ? Path.Combine(ProductNames.FileSystemName + ".app", "Contents", "MacOS", ProductNames.GameExecutableBaseName)
        : ProductNames.GameExecutableBaseName;

    /// <summary>
    /// A single pass that finds both the command word and every --flag[=value] pair, regardless of order. The first
    /// token that is neither a --flag nor a value consumed by the preceding --flag is the command.
    /// </summary>
    private static (string? Command, Dictionary<string, string?> Flags) ParseArgs(string[] args)
    {
        string? command = null;
        var flags = new Dictionary<string, string?>();
        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            if (arg.StartsWith("--", StringComparison.Ordinal))
            {
                var name = arg[2..];
                if (i + 1 < args.Length && !args[i + 1].StartsWith("--", StringComparison.Ordinal))
                    flags[name] = args[++i];
                else
                    flags[name] = null; // boolean flag
            }
            else if (command is null)
            {
                command = arg;
            }
        }
        return (command, flags);
    }

    private static void PrintUsage()
    {
        Console.WriteLine($"""
        {ProductNames.DisplayName} setup {ProductInfo.Version}

        Usage: {ProductNames.SetupBaseName} <command> [options]
               {ProductNames.SetupBaseName}            (from a terminal: guided install, or start the game when current)

          install [--game DISC_IMAGE] [--sysconf FILE] [--install-dir DIR] [--force-clean-build]
                  [--translator-bin PATH] [--disc-tool-bin PATH] [--cc PATH] [--cxx PATH]
                  [--fuse-ld NAME_OR_PATH] [--cmake PATH] [--ninja PATH] [--native-prebuilt-dir DIR]
                  [--sysroot PATH] [--progress-json] [--workspace DIR]
                Verify and extract your own {ProductNames.GameId} disc image, seed the NAND SYSCONF, translate
                and compile the game locally, and install it.
          uninstall        Remove the installed game and its menu entry (saves and settings are kept)
          launch           Start the installed game
          check-products   Report whether the installed game is current
          seed-sysconf [--sysconf FILE]          Copy a Wii SYSCONF into the runtime's NAND if it has none
          make-icon --data-root DIR --out DIR    Write the product icon from an extracted disc's DATA root
          --version
        """);
    }
}
