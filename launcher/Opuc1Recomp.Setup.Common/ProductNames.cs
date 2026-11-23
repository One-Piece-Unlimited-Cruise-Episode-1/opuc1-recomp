namespace Opuc1Recomp.Setup.Common;

/// <summary>
/// The names every setup host, script, and the runtime agree on. The file-system name is the runtime's
/// <c>kApplicationDirectoryName</c> (runtime/include/runtime_config.h): it names the per-user data directory,
/// so the two must never drift apart.
/// </summary>
public static class ProductNames
{
    public const string DisplayName = "One Piece: Unlimited Recompilation Episode 1";
    public const string FileSystemName = "One Piece Unlimited Recompilation Episode 1";
    public const string TechnicalId = "opuc1-recomp";

    /// <summary>The translated game executable without extension (CMake target <c>opuc1_recomp</c>).</summary>
    public const string GameExecutableBaseName = "opuc1-recomp";

    public const string SetupBaseName = "opuc1-recomp-setup";

    /// <summary>The only supported disc: One Piece Unlimited Cruise Episode 1, PAL.</summary>
    public const string GameId = "ROUPAF";
    public const string GameTitle = "One Piece Unlimited Cruise Episode 1";

    public static string GameExecutableFileName =>
        OperatingSystem.IsWindows() ? GameExecutableBaseName + ".exe" : GameExecutableBaseName;
}
