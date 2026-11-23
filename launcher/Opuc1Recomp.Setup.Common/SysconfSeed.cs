namespace Opuc1Recomp.Setup.Common;

/// <summary>
/// One Piece Unlimited Cruise runs the Wii SC library translated, and SCInit reads <c>/shared2/sys/SYSCONF</c> from the
/// NAND during OSInit. The runtime answers SCCheckStatus itself (runtime/src/hle/sc.cpp), so the game also boots without
/// the file (checked 2026-11-23: NANDOpen fails, boot continues to the save check). When the user has one (a Wii or
/// Dolphin system settings file), setup copies it into the NAND the runtime will use so the game reads real settings.
/// </summary>
public static class SysconfSeed
{
    public static readonly string NandRelativePath = Path.Combine("shared2", "sys", "SYSCONF");

    /// <summary>A Wii SYSCONF is one 16 KiB block starting with "SCv0".</summary>
    public const int SysconfLength = 0x4000;

    public const string NotFoundNote =
        "No Wii SYSCONF was found in a Dolphin user folder; the game starts without one. To give it your console " +
        "settings, run setup again with --sysconf <file> (your Wii NAND /shared2/sys/SYSCONF, or Dolphin Emulator/Wii/" +
        "shared2/sys/SYSCONF).";

    public static string InvalidMessage(string path) =>
        $"{path} is not a Wii SYSCONF (expected a {SysconfLength:N0}-byte file starting with SCv0).";

    /// <summary>The per-user data directory the runtime uses (portable <c>UserData</c> or the app-data directory).</summary>
    public static string UserDataDirectory(string? installDirectory) =>
        RuntimeConfiguration.ConfigDirectory(RuntimeConfiguration.ResolveConfigPath(installDirectory));

    /// <summary>
    /// The NAND root the runtime will open: Config.toml <c>[paths] nand_root</c> when set, otherwise the managed
    /// <c>NAND</c> directory beside Config.toml (runtime/include/nand_path.h <c>ManagedNandRootPath</c>).
    /// </summary>
    public static string ResolveNandRoot(string? installDirectory)
    {
        var configPath = RuntimeConfiguration.ResolveConfigPath(installDirectory);
        return RuntimeConfiguration.GetResolvedPath(configPath, "nand_root")
               ?? Path.Combine(RuntimeConfiguration.ConfigDirectory(configPath), "NAND");
    }

    /// <summary>Replaces the Dolphin folder search; for the self-tests only, so they never read the real folders.</summary>
    public static IReadOnlyList<string>? DolphinUserDirectoriesOverride { get; set; }

    /// <summary>Dolphin user folders that may hold a SYSCONF, most likely first.</summary>
    public static IEnumerable<string> DolphinUserDirectories()
    {
        if (DolphinUserDirectoriesOverride is { } overridden)
        {
            foreach (var directory in overridden) yield return directory;
            yield break;
        }
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (OperatingSystem.IsWindows())
        {
            yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "Dolphin Emulator");
            yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                "Dolphin Emulator");
        }
        else if (OperatingSystem.IsMacOS())
        {
            yield return Path.Combine(home, "Library", "Application Support", "Dolphin");
        }
        else
        {
            var xdgData = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
            if (!string.IsNullOrWhiteSpace(xdgData)) yield return Path.Combine(xdgData, "dolphin-emu");
            yield return Path.Combine(home, ".local", "share", "dolphin-emu");
            yield return Path.Combine(home, ".dolphin-emu");
            yield return Path.Combine(home, ".var", "app", "org.DolphinEmu.dolphin-emu", "data", "dolphin-emu");
        }
    }

    public static string? FindDolphinSysconf() =>
        DolphinUserDirectories()
            .Select(directory => Path.Combine(directory, "Wii", NandRelativePath))
            .FirstOrDefault(IsValidSysconf);

    public static bool IsValidSysconf(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.Length != SysconfLength) return false;
            using var stream = info.OpenRead();
            Span<byte> magic = stackalloc byte[4];
            return stream.Read(magic) == 4 && magic.SequenceEqual("SCv0"u8);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>
    /// Gives the NAND the runtime will use a SYSCONF when the user has one. An existing one is never replaced, because the game
    /// and the runtime settings bar may already have written to it.
    /// </summary>
    /// <returns>The SYSCONF path inside the NAND, or null when there is none and no source was found.</returns>
    public static string? EnsureSeeded(string? installDirectory, string? explicitSource, Action<string> log)
    {
        var nandRoot = ResolveNandRoot(installDirectory);
        var target = Path.Combine(nandRoot, NandRelativePath);
        if (IsValidSysconf(target))
        {
            if (explicitSource is not null)
                log($"The NAND already has a SYSCONF; {explicitSource} was not copied over it ({target}).");
            return target;
        }

        string source;
        if (explicitSource is not null)
        {
            source = Path.GetFullPath(explicitSource);
            if (!IsValidSysconf(source)) throw new InvalidDataException(InvalidMessage(source));
        }
        else
        {
            var found = FindDolphinSysconf();
            if (found is null)
            {
                log(NotFoundNote);
                return null;
            }
            source = found;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var temporary = target + ".tmp";
        File.Copy(source, temporary, overwrite: true);
        File.Move(temporary, target, overwrite: true);
        log($"Seeded the NAND SYSCONF from {source}.");
        return target;
    }
}
