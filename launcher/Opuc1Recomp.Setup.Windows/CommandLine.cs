namespace Opuc1Recomp.Setup.Windows;

internal enum AppMode
{
    Guided,
    Help,
    SilentInstall,
    VerifyInputs,
    Uninstall,
    SilentUninstall,
    UninstallWorker,
    SelfTest,
    Launch,
    CheckProducts,
    RepairProducts,
    Version,
    EmitPayloadIdentities,
    PackPayload,
    VerifyPayload
}

/// <summary>
/// What each mode accepts. Every cross-flag rule below is expressed here once instead of as a per-mode exception
/// list, so adding a mode cannot silently inherit another mode's inputs.
/// </summary>
internal sealed record ModeRules
{
    public string Flag { get; init; } = "this command";
    public bool AcceptsProgressJson { get; init; }
    public bool AcceptsGame { get; init; }
    public bool RequiresGame { get; init; }
    public bool AcceptsSysconf { get; init; }
    public bool AcceptsInstallDirectory { get; init; }
    public bool RequiresInstallDirectory { get; init; }
    public bool RequiresPayloadRoot { get; init; }
    public bool RequiresXz { get; init; }
    public bool RequiresOutput { get; init; }
    public bool AcceptsPortable { get; init; }
}

internal sealed class CommandLine
{
    public AppMode Mode { get; private set; } = AppMode.Help;
    public string? GamePath { get; private set; }
    public string? SysconfPath { get; private set; }
    public string? InstallDirectory { get; private set; }
    public bool Quiet { get; private set; }
    public bool ProgressJson { get; private set; }
    public string? PayloadRootPath { get; private set; }
    public string? XzPath { get; private set; }
    public string? OutputPath { get; private set; }
    public bool Portable { get; private set; }

    public static bool WantsProgressJson(string[] args) =>
        args.Any(argument => argument.Equals("--progress-json", StringComparison.OrdinalIgnoreCase));

    public static CommandLine Parse(string[] args)
    {
        var result = new CommandLine();
        if (args.Length == 0)
        {
            result.Mode = AppMode.Guided;
            return result;
        }
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
                case "--help": case "-h": case "/?": result.Mode = AppMode.Help; break;
                case "--version": result.Mode = AppMode.Version; break;
                case "--silent": result.Mode = AppMode.SilentInstall; break;
                case "--verify-inputs": result.Mode = AppMode.VerifyInputs; break;
                case "--uninstall": result.Mode = AppMode.Uninstall; break;
                case "--silent-uninstall": result.Mode = AppMode.SilentUninstall; result.Quiet = true; break;
                case "--uninstall-worker": result.Mode = AppMode.UninstallWorker; break;
                case "--self-test": result.Mode = AppMode.SelfTest; break;
                case "--launch": result.Mode = AppMode.Launch; break;
                case "--check-products": result.Mode = AppMode.CheckProducts; break;
                case "--repair-products": result.Mode = AppMode.RepairProducts; break;
                case "--portable": result.Portable = true; break;
                case "--emit-payload-identities": result.Mode = AppMode.EmitPayloadIdentities; break;
                case "--payload-root": result.PayloadRootPath = RequireValue(args, ref i); break;
                case "--pack-payload": result.Mode = AppMode.PackPayload; break;
                case "--verify-payload": result.Mode = AppMode.VerifyPayload; break;
                case "--xz": result.XzPath = RequireValue(args, ref i); break;
                case "--output": result.OutputPath = RequireValue(args, ref i); break;
                case "--quiet": result.Quiet = true; break;
                case "--progress-json": result.ProgressJson = true; break;
                case "--game": result.GamePath = RequireValue(args, ref i); break;
                case "--sysconf": result.SysconfPath = RequireValue(args, ref i); break;
                case "--install-dir": result.InstallDirectory = RequireValue(args, ref i); break;
                default:
                    throw new ArgumentException($"Unknown option: {args[i]}. Run with --help for the options.");
            }
        }

        result.Validate();
        return result;
    }

    private static readonly Dictionary<AppMode, ModeRules> Rules = new()
    {
        [AppMode.SilentInstall] = new ModeRules
        {
            Flag = "--silent",
            AcceptsProgressJson = true, AcceptsGame = true, RequiresGame = true, AcceptsSysconf = true,
            AcceptsInstallDirectory = true, AcceptsPortable = true
        },
        [AppMode.VerifyInputs] = new ModeRules
        {
            Flag = "--verify-inputs", AcceptsProgressJson = true, AcceptsGame = true, RequiresGame = true
        },
        [AppMode.CheckProducts] = new ModeRules
        {
            Flag = "--check-products", AcceptsProgressJson = true, AcceptsInstallDirectory = true
        },
        [AppMode.RepairProducts] = new ModeRules
        {
            Flag = "--repair-products", AcceptsProgressJson = true, AcceptsSysconf = true,
            AcceptsInstallDirectory = true, RequiresInstallDirectory = true
        },
        [AppMode.Launch] = new ModeRules { Flag = "--launch", AcceptsInstallDirectory = true },
        [AppMode.Uninstall] = new ModeRules
        {
            Flag = "--uninstall", AcceptsInstallDirectory = true, RequiresInstallDirectory = true
        },
        [AppMode.SilentUninstall] = new ModeRules
        {
            Flag = "--silent-uninstall", AcceptsInstallDirectory = true, RequiresInstallDirectory = true
        },
        [AppMode.UninstallWorker] = new ModeRules
        {
            Flag = "--uninstall-worker", AcceptsInstallDirectory = true, RequiresInstallDirectory = true
        },
        [AppMode.EmitPayloadIdentities] = new ModeRules
        {
            Flag = "--emit-payload-identities", RequiresPayloadRoot = true
        },
        [AppMode.PackPayload] = new ModeRules
        {
            Flag = "--pack-payload", RequiresPayloadRoot = true, RequiresXz = true, RequiresOutput = true
        },
        [AppMode.VerifyPayload] = new ModeRules { Flag = "--verify-payload", RequiresPayloadRoot = true }
    };

    private void Validate()
    {
        var rules = Rules.GetValueOrDefault(Mode) ?? new ModeRules();

        void Reject(bool present, string option)
        {
            if (present) throw new ArgumentException($"{option} is not valid with {rules.Flag}.");
        }

        Reject(GamePath is not null && !rules.AcceptsGame, "--game");
        Reject(SysconfPath is not null && !rules.AcceptsSysconf, "--sysconf");
        Reject(InstallDirectory is not null && !rules.AcceptsInstallDirectory, "--install-dir");
        Reject(ProgressJson && !rules.AcceptsProgressJson, "--progress-json");
        Reject(PayloadRootPath is not null && !rules.RequiresPayloadRoot, "--payload-root");
        Reject(Portable && !rules.AcceptsPortable, "--portable");
        Reject(XzPath is not null && !rules.RequiresXz, "--xz");
        Reject(OutputPath is not null && !rules.RequiresOutput, "--output");

        if (rules.RequiresGame && string.IsNullOrWhiteSpace(GamePath))
            throw new ArgumentException("--game is required.");
        if (rules.RequiresPayloadRoot && string.IsNullOrWhiteSpace(PayloadRootPath))
            throw new ArgumentException($"--payload-root is required with {rules.Flag}.");
        if (rules.RequiresXz && string.IsNullOrWhiteSpace(XzPath))
            throw new ArgumentException($"--xz is required with {rules.Flag}.");
        if (rules.RequiresOutput && string.IsNullOrWhiteSpace(OutputPath))
            throw new ArgumentException($"--output is required with {rules.Flag}.");

        // A portable installation has no default location so the caller must state where that root is.
        if (Portable && string.IsNullOrWhiteSpace(InstallDirectory))
            throw new ArgumentException(
                "--install-dir is required with --portable. Its parent folder becomes the portable root.");
        if (Mode is AppMode.SilentInstall && string.IsNullOrWhiteSpace(InstallDirectory))
            InstallDirectory = ProductInfo.DefaultInstallDirectory;
        if (rules.RequiresInstallDirectory && string.IsNullOrWhiteSpace(InstallDirectory))
            throw new ArgumentException($"--install-dir is required with {rules.Flag}.");
    }

    private static string RequireValue(string[] args, ref int index)
    {
        if (++index >= args.Length || string.IsNullOrWhiteSpace(args[index]))
            throw new ArgumentException($"A value is required after {args[index - 1]}.");
        return args[index];
    }
}
