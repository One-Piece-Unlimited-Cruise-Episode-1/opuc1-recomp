using System.Text.RegularExpressions;

namespace Opuc1Recomp.Setup.Linux;

/// <summary>
/// The facts this tool needs out of a projects/*/recomp.yml: the game id and the pinned main.dol SHA-256. Parsed
/// literally line by line - the same approach launcher/build-cache.sh uses - rather than pulling in a YAML library,
/// since the manifest has a fixed shape.
/// </summary>
internal sealed class ProjectManifest
{
    public required string Path { get; init; }
    public required string GameId { get; init; }
    public required string DolSha256 { get; init; }

    public static ProjectManifest Load(string path)
    {
        if (!File.Exists(path)) throw new FileNotFoundException("Translation project file is missing", path);

        string? gameId = null, dolSha = null;
        var section = "";
        var inputKey = "";

        foreach (var raw in File.ReadLines(path))
        {
            var line = Regex.Replace(raw, "#.*$", "");
            if (string.IsNullOrWhiteSpace(line)) continue;

            var sectionMatch = Regex.Match(line, "^([A-Za-z0-9_]+):");
            if (sectionMatch.Success)
            {
                section = sectionMatch.Groups[1].Value;
                inputKey = "";
                continue;
            }

            if (section == "inputs")
            {
                var keyMatch = Regex.Match(line, @"^\s{2}([A-Za-z0-9_]+):\s*$");
                if (keyMatch.Success) { inputKey = keyMatch.Groups[1].Value; continue; }

                var shaMatch = Regex.Match(line, @"^\s*sha256:\s*([0-9a-fA-F]{64})\s*$");
                if (shaMatch.Success && inputKey == "dol") dolSha = shaMatch.Groups[1].Value.ToLowerInvariant();
            }
            else if (section == "project")
            {
                var idMatch = Regex.Match(line, @"^\s*game_id:\s*(\S+)\s*$");
                if (idMatch.Success) gameId = idMatch.Groups[1].Value;
            }
        }

        if (gameId is null || dolSha is null)
        {
            throw new InvalidDataException(
                $"{path} does not pin project.game_id and inputs.dol.sha256; the project file is not the shape this tool expects.");
        }

        return new ProjectManifest { Path = path, GameId = gameId, DolSha256 = dolSha };
    }

    /// <summary>Every supported release: one per projects/*/recomp.yml (today only projects/opuc1, ROUPAF).</summary>
    public static IReadOnlyList<ProjectManifest> LoadAll(string workspace)
    {
        var projects = Directory.EnumerateDirectories(System.IO.Path.Combine(workspace, "projects"))
            .Select(directory => System.IO.Path.Combine(directory, "recomp.yml"))
            .Where(File.Exists)
            .Order(StringComparer.Ordinal)
            .Select(Load)
            .ToList();
        if (projects.Count == 0)
            throw new InvalidOperationException($"No projects/*/recomp.yml was found under {workspace}.");
        return projects;
    }

    public static string Describe(IEnumerable<ProjectManifest> projects) =>
        string.Join(", ", projects.Select(project => project.GameId));
}
