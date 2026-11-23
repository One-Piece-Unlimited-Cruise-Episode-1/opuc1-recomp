namespace Opuc1Recomp.Setup.Linux;

/// <summary>
/// freedesktop.org .desktop application-menu entry for the installed game. Replaces the Windows installer's
/// registry uninstall entry and .lnk shortcuts.
/// </summary>
internal static class DesktopEntry
{
    private static string ApplicationsDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "applications");

    public static string EntryPath => Path.Combine(ApplicationsDirectory, ProductNames.TechnicalId + ".desktop");

    public static void Create(string exePath, string? iconPath)
    {
        // macOS has no freedesktop menu; the installed .app bundle is the launchable entry itself.
        if (OperatingSystem.IsMacOS()) return;
        Directory.CreateDirectory(ApplicationsDirectory);
        // The default install path contains spaces ("One Piece Unlimited Recompilation Episode 1"), so Exec quotes it
        // as the Desktop Entry spec requires; Path is a plain string value and needs no quoting.
        var contents =
            "[Desktop Entry]\n" +
            "Type=Application\n" +
            $"Name={ProductNames.DisplayName}\n" +
            $"Comment={ProductNames.GameTitle}, compiled natively from your own disc\n" +
            $"Exec={QuoteExec(exePath)}\n" +
            $"Path={Path.GetDirectoryName(exePath)}\n" +
            (iconPath is not null ? $"Icon={iconPath}\n" : "") +
            "Categories=Game;\n" +
            "Terminal=false\n";
        File.WriteAllText(EntryPath, contents);
    }

    public static void Remove()
    {
        if (File.Exists(EntryPath)) File.Delete(EntryPath);
    }

    // Desktop Entry spec: inside a quoted argument the characters " ` $ and \ are backslash-escaped, and a literal
    // percent sign is written twice.
    private static string QuoteExec(string path)
    {
        var escaped = new System.Text.StringBuilder("\"");
        foreach (var c in path)
        {
            if (c is '"' or '`' or '$' or '\\') escaped.Append('\\');
            escaped.Append(c == '%' ? "%%" : c.ToString());
        }
        return escaped.Append('"').ToString();
    }
}
