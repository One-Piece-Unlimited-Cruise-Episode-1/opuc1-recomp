using System.Text.Json;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static class ConsoleCommands
{
    public static int Help()
    {
        Console.Out.WriteLine($"{ProductNames.DisplayName} setup {ProductInfo.Version}");
        Console.Out.WriteLine(
            $"Installs {ProductNames.DisplayName} from your own {ProductNames.GameTitle} disc ({ProductNames.GameId}):");
        Console.Out.WriteLine("checks the image, extracts it, translates and compiles the game on this PC, and launches it.");
        Console.Out.WriteLine("Started without options, setup asks for the disc image and guides you through it.");
        Console.Out.WriteLine();
        Console.Out.WriteLine("Commands:");
        Console.Out.WriteLine("  (no options)                        guided install, update or launch");
        Console.Out.WriteLine("  --silent --game <image> [--install-dir <dir>] [--sysconf <file>] [--portable] [--progress-json]");
        Console.Out.WriteLine("  --verify-inputs --game <image> [--progress-json]");
        Console.Out.WriteLine("  --check-products [--install-dir <dir>] [--progress-json]");
        Console.Out.WriteLine("  --repair-products --install-dir <dir> [--sysconf <file>] [--progress-json]");
        Console.Out.WriteLine("  --launch [--install-dir <dir>]");
        Console.Out.WriteLine("  --uninstall --install-dir <dir>");
        Console.Out.WriteLine("  --self-test | --version | --help");
        Console.Out.WriteLine();
        Console.Out.WriteLine($"Default install folder: {ProductInfo.DefaultInstallDirectory}");
        Console.Out.WriteLine("--sysconf: optional Wii system settings (NAND /shared2/sys/SYSCONF) copied into the game's NAND.");
        Console.Out.WriteLine("Without it setup uses the one in your Dolphin user folder if there is one; the game also starts without.");
        return 0;
    }

    public static int Version()
    {
        Console.Out.WriteLine(ProductInfo.Version);
        return 0;
    }

    public static int EmitPayloadIdentities(CommandLine command)
    {
        var root = Path.GetFullPath(command.PayloadRootPath!);
        if (!Directory.Exists(InstalledLayout.Toolkit(root)) ||
            !Directory.Exists(InstalledLayout.Workspace(root)))
        {
            throw new InvalidDataException($"{root} is not a staged payload root: expected " +
                $"{InstalledLayout.ToolkitDirectoryName} and {InstalledLayout.WorkspaceDirectoryName} " +
                "directories.");
        }

        var components = ToolkitFingerprint.ComputeComponents(root);
        var identities = new PayloadIdentities(
            components.Compile,
            components.Translation,
            components.NativeToolchain,
            ToolkitFingerprint.ComputePackage(root),
            ToolkitFingerprint.ComputeRuntimeAssets(root));
        Console.Out.WriteLine(JsonSerializer.Serialize(identities));
        return 0;
    }

    private sealed record PayloadIdentities(string ToolkitFingerprint, string TranslationFingerprint,
        string NativeToolchainFingerprint, string ToolkitPackageFingerprint,
        string RuntimeAssetsFingerprint);

    public static int VerifyInputs(CommandLine command)
    {
        var reporter = command.ProgressJson ? new NdjsonInstallReporter(ProductInfo.Version) : null;
        var temp = Path.Combine(Path.GetTempPath(), "opuc1-recomp-verify-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            using var payload = PayloadArchive.OpenCurrent();
            var manifest = payload.ReadManifest();
            var tool = payload.ExtractDiscTool(temp);
            reporter?.Progress(InstallStages.Validate, "Checking the Wii disc image...", 10);
            var header = InputValidation.ReadDiscHeaderAsync(tool, command.GamePath!).GetAwaiter().GetResult();
            var release = InputValidation.EnsureCompatibleDisc(header, manifest);
            if (reporter is null)
                Console.Out.WriteLine($"{header.GameId} ({header.InternalName}, {header.Region}, revision " +
                                      $"{header.Revision}) is supported; main.dol must have SHA-256 {release.DolSha256}.");
            reporter?.Success(ProductInfo.DefaultInstallDirectory);
            return 0;
        }
        catch (Exception ex) when (reporter is not null)
        {
            reporter.Failure(ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            reporter?.EnsureFinished("Input verification stopped before it reached a result.");
            if (Directory.Exists(temp)) Directory.Delete(temp, recursive: true);
        }
    }

    /// <summary>The setup log of one operation, in %TEMP%.</summary>
    internal static Action<string> OpenLog(string fileName, out string logPath)
    {
        var path = Path.Combine(Path.GetTempPath(), fileName);
        logPath = path;
        var logLock = new object();
        File.WriteAllText(path, $"{ProductNames.DisplayName} setup {ProductInfo.Version}{Environment.NewLine}");
        return message =>
        {
            lock (logLock)
            {
                File.AppendAllText(path, $"[{DateTime.Now:O}] {message}{Environment.NewLine}");
            }
        };
    }

    public static async Task<int> Install(CommandLine command, CancellationToken cancellationToken = default)
    {
        var log = OpenLog(ProductNames.SetupBaseName + ".log", out _);
        var ndjson = command.ProgressJson ? new NdjsonInstallReporter(ProductInfo.Version, log) : null;
        IInstallReporter reporter = ndjson ?? (IInstallReporter)new ConsoleInstallReporter(log);
        var installDirectory = Path.GetFullPath(command.InstallDirectory!);
        try
        {
            var engine = new InstallerEngine(reporter);
            await engine.InstallAsync(new InstallOptions
            {
                GamePath = command.GamePath!,
                InstallDirectory = installDirectory,
                SysconfPath = command.SysconfPath,
                Portable = command.Portable
            }, cancellationToken);
            ndjson?.Success(installDirectory);
            return 0;
        }
        catch (Exception ex) when (ndjson is not null)
        {
            // The caller reads stdout as a protocol, so the failure has to arrive as the terminal result line; the
            // exception detail belongs on stderr and in the setup log.
            ndjson.Failure(ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            ndjson?.EnsureFinished("The installer stopped before it reached a result.");
        }
    }

    /// <summary>Reports whether the installed game is current, without building or mutating anything. Exit code 2
    /// means work is required, not a protocol failure.</summary>
    public static int CheckProducts(CommandLine command, CancellationToken cancellationToken = default)
    {
        var reporter = command.ProgressJson ? new NdjsonInstallReporter(ProductInfo.Version) : null;
        var installation = new Installation(string.IsNullOrWhiteSpace(command.InstallDirectory)
            ? AppContext.BaseDirectory
            : Path.GetFullPath(command.InstallDirectory));
        try
        {
            // The lock also performs exact journal/scratch recovery. Acquire it for absent and partial roots too: a
            // killed first install can have recoverable state even when the game and state file have not reached
            // the live directory yet.
            using var operationLock = InstallOperationLock.Acquire(installation.Root, reporter);
            PortableInstallHealing.HealMovedInstall(installation, reporter);
            cancellationToken.ThrowIfCancellationRequested();
            var report = InspectProducts(installation);
            Console.Out.WriteLine(command.ProgressJson
                ? SerializeProductsReport(report)
                : $"game: {report.Game.Reason} {report.Game.Detail}".TrimEnd());
            Console.Out.Flush();
            reporter?.Success(installation.Root);
            return report.RebuildRequired ? 2 : 0;
        }
        catch (Exception ex) when (reporter is not null)
        {
            reporter.Failure(ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            reporter?.EnsureFinished("The product check stopped before it reached a result.");
        }
    }

    /// <summary>
    /// Brings an existing installation up to date without the disc: rebuilds the game when its toolkit or inputs
    /// moved, republishes copied runtime assets, and seeds a missing SYSCONF. The final inspection is authoritative,
    /// so success is reported only when the game is current.
    /// </summary>
    public static async Task<int> RepairProducts(CommandLine command, CancellationToken cancellationToken = default)
    {
        var log = OpenLog(ProductNames.SetupBaseName + "-repair.log", out _);
        var ndjson = command.ProgressJson ? new NdjsonInstallReporter(ProductInfo.Version, log) : null;
        IInstallReporter reporter = ndjson is null ? new ConsoleInstallReporter(log) : ndjson;
        var installDirectory = Path.GetFullPath(command.InstallDirectory!);
        try
        {
            await RepairAsync(installDirectory, command.SysconfPath, reporter, cancellationToken);
            ndjson?.Success(installDirectory);
            return 0;
        }
        catch (Exception ex) when (ndjson is not null)
        {
            ndjson.Failure(ex is OperationCanceledException ? "Operation canceled." : ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            ndjson?.EnsureFinished("Product repair stopped before it reached a result.");
        }
    }

    internal static async Task RepairAsync(string installDirectory, string? sysconfPath, IInstallReporter reporter,
        CancellationToken cancellationToken)
    {
        var installation = new Installation(installDirectory);
        if (!installation.IsPresent)
            throw new InvalidOperationException($"{ProductNames.DisplayName} is not installed at {installDirectory}.");

        using var operationLock = InstallOperationLock.Acquire(installDirectory, reporter);
        PortableInstallHealing.HealMovedInstall(installation, reporter);

        cancellationToken.ThrowIfCancellationRequested();
        var service = new ProductRepairService(installation, reporter);
        using var scratch = InstallScratchSpace.CreateInsideInstall(installDirectory, reporter);
        await service.RepairAsync(new ProductRepairService.ReconcileOptions { ScratchRoot = scratch.Root },
            cancellationToken);
        SysconfSeed.EnsureSeeded(installDirectory, sysconfPath, reporter.Diagnostic);

        var after = InspectProducts(installation);
        if (after.RebuildRequired)
            throw new InvalidOperationException(
                "Repair completed without producing a current installation: " + after.Game.Detail);
    }

    /// <summary>The <c>products</c> record: a typed status and a human-readable detail for the game.</summary>
    internal sealed record ProductsReport(string InstallDirectory, ProductState Game)
    {
        public bool RebuildRequired => Game.ActionRequired;
    }

    internal static ProductsReport InspectProducts(Installation installation) =>
        // Probing a path nothing was installed to is a valid answer. It must not create a directory or turn a
        // first-install probe into a repair request.
        installation.IsPresent
            ? new ProductsReport(installation.Root, installation.CheckGame(installation.ResolveToolkitFingerprint()))
            : new ProductsReport(installation.Root,
                new ProductState(ProductStatus.Absent, ProductNames.DisplayName + " is not installed here."));

    /// <summary>Emits the stable <c>products</c> record. Callers branch on <c>status</c> only.</summary>
    internal static string SerializeProductsReport(ProductsReport report) =>
        JsonSerializer.Serialize(new
        {
            type = "products",
            setupVersion = ProductInfo.Version,
            installDir = report.InstallDirectory,
            rebuildRequired = report.RebuildRequired,
            game = new { status = report.Game.Reason, detail = report.Game.Detail }
        });
}
