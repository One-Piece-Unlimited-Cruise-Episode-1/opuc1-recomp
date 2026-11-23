using System.Reflection;

namespace Opuc1Recomp.Setup.Linux;

internal static class ProductInfo
{
    public const string Name = ProductNames.DisplayName;
    public static readonly string Version =
        typeof(ProductInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()!
            .InformationalVersion;
}

/// <summary>The installed game's record inside install-state.json.</summary>
internal sealed class ProductInstallRecord
{
    public string InstallDirectory { get; set; } = "";
    /// <summary>Relative to <see cref="InstallDirectory"/>; on macOS the binary inside the .app bundle.</summary>
    public string ExecutableName { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string BuiltUtc { get; set; } = "";
    public string SetupVersion { get; set; } = "";
}

/// <summary>
/// The flat state document this tool keeps at <c>&lt;user data&gt;/install-state.json</c> (the runtime's own per-user
/// directory, see <see cref="Program.UserDataDirectory"/>). Deliberately not a fingerprint tree: local-build.sh does its
/// own incremental-rebuild caching, so this only remembers where the game was installed and what it was built from.
/// </summary>
internal sealed class InstallState
{
    public int SchemaVersion { get; set; } = 1;
    public string Workspace { get; set; } = "";
    public ProductInstallRecord? Game { get; set; }
}
