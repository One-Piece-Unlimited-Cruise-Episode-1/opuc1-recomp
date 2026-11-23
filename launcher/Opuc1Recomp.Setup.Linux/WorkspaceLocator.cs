namespace Opuc1Recomp.Setup.Linux;

/// <summary>
/// Finds the checkout this tool runs from by walking up from its own directory looking for launcher/local-build.sh.
/// This tool operates directly on a workspace (a git checkout, the AppImage's writable cache, or the macOS
/// Application Support copy), so there is no installed "Toolkit" layout to anchor on the way the Windows installer has.
/// </summary>
internal static class WorkspaceLocator
{
    private const int MaxSearchDepth = 6;

    public static string FindFrom(string startDirectory)
    {
        var current = new DirectoryInfo(startDirectory);
        for (var level = 0; level <= MaxSearchDepth && current is not null; level++, current = current.Parent)
        {
            if (File.Exists(Path.Combine(current.FullName, "launcher", "local-build.sh")))
            {
                return current.FullName;
            }
        }
        throw new InvalidOperationException(
            $"Could not find the {ProductNames.DisplayName} workspace (looked for launcher/local-build.sh walking up " +
            $"from {startDirectory}). Pass --workspace <path-to-checkout> explicitly.");
    }
}
