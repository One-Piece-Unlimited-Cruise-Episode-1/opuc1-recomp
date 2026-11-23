namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// Stable stage identifiers reported by <c>--progress-json</c>. The strings may gain new members, but an existing
/// identifier never changes meaning.
/// </summary>
internal static class InstallStages
{
    public const string Validate = "validate";
    public const string ExtractToolkit = "extract-toolkit";
    public const string ExtractDisc = "extract-disc";
    public const string Build = "build";
    public const string Publish = "publish";
}
