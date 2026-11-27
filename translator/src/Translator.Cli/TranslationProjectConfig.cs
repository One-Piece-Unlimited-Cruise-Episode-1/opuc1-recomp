using System.Globalization;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using Translator.Core.Loading;

namespace Translator.Cli.Configuration;

internal sealed class TranslationProjectConfig
{
    private TranslationProjectConfig(
        string sourcePath,
        string workspaceRoot,
        ProjectIdentity identity,
        ProjectMemory memory,
        ProjectInputs inputs,
        ProjectTranslation translation,
        ProjectRuntime runtime,
        ProjectOutput output)
    {
        SourcePath = sourcePath;
        WorkspaceRoot = workspaceRoot;
        Identity = identity;
        Memory = memory;
        Inputs = inputs;
        Translation = translation;
        Runtime = runtime;
        Output = output;
    }

    public string SourcePath { get; }
    public string WorkspaceRoot { get; }
    public ProjectIdentity Identity { get; }
    public ProjectMemory Memory { get; }
    public ProjectInputs Inputs { get; }
    public ProjectTranslation Translation { get; }
    public ProjectRuntime Runtime { get; }
    public ProjectOutput Output { get; }

    public static TranslationProjectConfig Load(string path)
    {
        var sourcePath = Path.GetFullPath(path);
        if (!File.Exists(sourcePath))
        {
            throw new FileNotFoundException("Translation project manifest not found.", sourcePath);
        }

        var dto = ProjectDto.Parse(File.ReadAllText(sourcePath), sourcePath)
            ?? throw new InvalidDataException($"Translation project manifest is empty: {sourcePath}");
        if (dto.SchemaVersion != 1)
        {
            throw new InvalidDataException($"Unsupported translation project schema_version {dto.SchemaVersion}; expected 1.");
        }

        var manifestDirectory = Path.GetDirectoryName(sourcePath)!;
        var workspaceRoot = ResolvePath(manifestDirectory, dto.WorkspaceRoot ?? ".");
        var identity = new ProjectIdentity(
            Require(dto.Project?.Id, "project.id"),
            dto.Project?.DisplayName ?? dto.Project?.Id!,
            dto.Project?.GameId);
        var memory = new ProjectMemory(
            ParseUInt32(dto.Memory?.Base, MemoryLayout.RamBase, "memory.base"),
            ParseInt32(dto.Memory?.Size, MemoryLayout.RamSize, "memory.size"),
            ParseOptionalUInt32(dto.Memory?.SdaBase, "memory.sda_base"),
            ParseOptionalUInt32(dto.Memory?.Sda2Base, "memory.sda2_base"));

        var dolDto = dto.Inputs?.Dol ?? throw new InvalidDataException("Translation project requires inputs.dol.");
        var dol = ResolveInput(workspaceRoot, dolDto, "inputs.dol");

        var entryPoints = (dto.Translation?.EntryPoints ?? [])
            .Select((value, index) => ParseUInt32(value, null, $"translation.entry_points[{index}]"))
            .ToArray();
        string? functionMapPath = null;
        if (dto.Translation?.FunctionMap is { } functionMapDto)
        {
            functionMapPath = ResolvePath(
                workspaceRoot,
                Require(functionMapDto.Path, "translation.function_map.path"));
        }
        var translation = new ProjectTranslation(
            entryPoints,
            functionMapPath,
            dto.Translation?.AllowUnsupportedInstructions ?? false);

        var abiDirectories = (dto.Runtime?.NativeAbiDirectories ?? [])
            .Select(pathValue => ResolvePath(workspaceRoot, pathValue))
            .ToArray();
        var runtime = new ProjectRuntime(
            abiDirectories,
            ResolvePath(workspaceRoot, dto.Runtime?.NativeRegistrationRoot ?? "runtime/src"),
            ResolvePath(workspaceRoot, dto.Runtime?.NativeIndex ?? "runtime/native-index.json"));
        var outputRoot = ResolvePath(workspaceRoot, dto.Output?.Root ?? "generated");
        var output = new ProjectOutput(
            outputRoot,
            ResolveOutput(outputRoot, dto.Output?.Functions, "functions"),
            ResolveOutput(outputRoot, dto.Output?.RuntimeConfig, "RuntimeConfig.h"),
            ResolveOutput(outputRoot, dto.Output?.DataInitializer, "data_sections_init.cpp"));

        var config = new TranslationProjectConfig(
            sourcePath,
            workspaceRoot,
            identity,
            memory,
            new ProjectInputs(dol),
            translation,
            runtime,
            output);
        config.ValidateInputs();
        return config;
    }

    /// <summary>The pinned Small Data Area bases (r13/r2), or a diagnostic naming which manifest keys are missing.</summary>
    public (uint Sda1Base, uint Sda2Base) RequireSdaBases()
    {
        if (Memory.SdaBase is { } sda1 && Memory.Sda2Base is { } sda2)
        {
            return (sda1, sda2);
        }

        var missing = Memory.SdaBase is null
            ? Memory.Sda2Base is null ? "memory.sda_base and memory.sda2_base" : "memory.sda_base"
            : "memory.sda2_base";
        throw new InvalidDataException(
            $"Translation project '{Identity.Id}' is missing {missing} in {SourcePath}. " +
            "These are the r13/r2 Small Data Area bases the boot code installs (_SDA_BASE_ and _SDA2_BASE_); " +
            "read them from the lis/ori pair in __init_registers of the pinned DOL and add them as hex strings " +
            "under the manifest's memory: section.");
    }

    private void ValidateInputs()
    {
        ValidateInput(Inputs.Dol, "DOL");
        if (Translation.FunctionMapPath is { } functionMapPath && !File.Exists(functionMapPath))
        {
            throw new FileNotFoundException("Configured function map was not found.", functionMapPath);
        }
        if (Memory.Size <= 0)
        {
            throw new InvalidDataException("memory.size must be positive.");
        }
    }

    private static ProjectBinaryInput ResolveInput(string root, BinaryInputDto input, string field)
    {
        var path = ResolvePath(root, Require(input.Path, $"{field}.path"));
        return new ProjectBinaryInput(path, NormalizeHash(input.Sha256));
    }

    private static void ValidateInput(ProjectBinaryInput input, string label)
    {
        if (!File.Exists(input.Path))
        {
            throw new FileNotFoundException($"Configured {label} input was not found.", input.Path);
        }
        if (!string.IsNullOrWhiteSpace(input.Sha256))
        {
            var actual = FileSha256(input.Path);
            if (!string.Equals(actual, input.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException($"Configured {label} SHA-256 does not match {input.Path}. Expected {input.Sha256}, got {actual}.");
            }
        }
    }

    private static string FileSha256(string path) =>
        ChecksumUtilities.Sha256HexOfFile(path);

    private static string? NormalizeHash(string? value) =>
        string.IsNullOrWhiteSpace(value) ? null : value.Trim().ToLowerInvariant();

    private static string ResolveOutput(string outputRoot, string? value, string fallback) =>
        ResolvePath(outputRoot, string.IsNullOrWhiteSpace(value) ? fallback : value);

    private static string ResolvePath(string root, string value)
    {
        var expanded = Environment.ExpandEnvironmentVariables(value);
        return Path.GetFullPath(Path.IsPathRooted(expanded) ? expanded : Path.Combine(root, expanded));
    }

    private static string Require(string? value, string field) =>
        !string.IsNullOrWhiteSpace(value) ? value : throw new InvalidDataException($"Translation project requires {field}.");

    private static uint ParseUInt32(string? value, uint? fallback, string field)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return fallback ?? throw new InvalidDataException($"Translation project requires {field}.");
        }
        var text = value.Trim();
        var style = NumberStyles.Integer;
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            text = text[2..];
            style = NumberStyles.HexNumber;
        }
        return uint.TryParse(text, style, CultureInfo.InvariantCulture, out var result)
            ? result
            : throw new InvalidDataException($"Invalid unsigned integer '{value}' for {field}.");
    }

    private static uint? ParseOptionalUInt32(string? value, string field) =>
        string.IsNullOrWhiteSpace(value) ? null : ParseUInt32(value, null, field);

    private static int ParseInt32(string? value, int fallback, string field)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return fallback;
        }
        var parsed = ParseUInt32(value, null, field);
        return parsed <= int.MaxValue
            ? (int)parsed
            : throw new InvalidDataException($"{field} exceeds Int32 range.");
    }

    // The manifest is read through YamlDotNet's representation model and mapped by hand rather than
    // through its reflection-based deserializer, because the translator ships as Native AOT. The
    // mapping keeps that deserializer's contract: snake_case keys, unknown keys rejected, and
    // ~ / null / an empty value read as null.
    private sealed class ProjectDto
    {
        public int SchemaVersion { get; init; }
        public string? WorkspaceRoot { get; init; }
        public IdentityDto? Project { get; init; }
        public MemoryDto? Memory { get; init; }
        public InputsDto? Inputs { get; init; }
        public TranslationDto? Translation { get; init; }
        public RuntimeDto? Runtime { get; init; }
        public OutputDto? Output { get; init; }

        public static ProjectDto? Parse(string text, string sourcePath)
        {
            var stream = new YamlStream();
            try
            {
                using var reader = new StringReader(text);
                stream.Load(reader);
            }
            catch (YamlException ex)
            {
                throw new InvalidDataException($"Translation project manifest is not valid YAML ({sourcePath}): {ex.Message}", ex);
            }
            if (stream.Documents.Count == 0 || YamlSection.IsNull(stream.Documents[0].RootNode))
            {
                return null;
            }
            var root = YamlSection.Of(stream.Documents[0].RootNode, "(root)");
            root.RejectUnknown("schema_version", "workspace_root", "project", "memory", "inputs", "translation", "runtime", "output");
            return new ProjectDto
            {
                SchemaVersion = root.Integer("schema_version") ?? 0,
                WorkspaceRoot = root.Scalar("workspace_root"),
                Project = root.Mapping("project") is { } project ? IdentityDto.From(project) : null,
                Memory = root.Mapping("memory") is { } memory ? MemoryDto.From(memory) : null,
                Inputs = root.Mapping("inputs") is { } inputs ? InputsDto.From(inputs) : null,
                Translation = root.Mapping("translation") is { } translation ? TranslationDto.From(translation) : null,
                Runtime = root.Mapping("runtime") is { } runtime ? RuntimeDto.From(runtime) : null,
                Output = root.Mapping("output") is { } output ? OutputDto.From(output) : null,
            };
        }
    }

    private sealed class IdentityDto
    {
        public string? Id { get; init; }
        public string? DisplayName { get; init; }
        public string? GameId { get; init; }

        public static IdentityDto From(YamlSection section)
        {
            section.RejectUnknown("id", "display_name", "game_id");
            return new IdentityDto
            {
                Id = section.Scalar("id"),
                DisplayName = section.Scalar("display_name"),
                GameId = section.Scalar("game_id"),
            };
        }
    }

    private sealed class MemoryDto
    {
        public string? Base { get; init; }
        public string? Size { get; init; }
        public string? SdaBase { get; init; }
        public string? Sda2Base { get; init; }

        public static MemoryDto From(YamlSection section)
        {
            section.RejectUnknown("base", "size", "sda_base", "sda2_base");
            return new MemoryDto
            {
                Base = section.Scalar("base"),
                Size = section.Scalar("size"),
                SdaBase = section.Scalar("sda_base"),
                Sda2Base = section.Scalar("sda2_base"),
            };
        }
    }

    private sealed class InputsDto
    {
        public BinaryInputDto? Dol { get; init; }

        public static InputsDto From(YamlSection section)
        {
            section.RejectUnknown("dol");
            return new InputsDto { Dol = section.Mapping("dol") is { } dol ? BinaryInputDto.From(dol) : null };
        }
    }

    private sealed class BinaryInputDto
    {
        public string Path { get; init; } = string.Empty;
        public string? Sha256 { get; init; }

        public static BinaryInputDto From(YamlSection section)
        {
            section.RejectUnknown("path", "sha256");
            return new BinaryInputDto
            {
                Path = section.Scalar("path") ?? string.Empty,
                Sha256 = section.Scalar("sha256"),
            };
        }
    }

    // Translation behaviour (state-free ABI, register residency, leaf inlining, flat guest memory) is
    // unconditional; only inputs are declared here. function_map is one: every function start in the
    // image, which the recursive pass seeds from.
    private sealed class TranslationDto
    {
        public List<string>? EntryPoints { get; init; }
        public FunctionMapDto? FunctionMap { get; init; }
        public bool? AllowUnsupportedInstructions { get; init; }

        public static TranslationDto From(YamlSection section)
        {
            section.RejectUnknown("entry_points", "function_map", "allow_unsupported_instructions");
            return new TranslationDto
            {
                EntryPoints = section.Sequence("entry_points"),
                FunctionMap = section.Mapping("function_map") is { } map ? FunctionMapDto.From(map) : null,
                AllowUnsupportedInstructions = section.Boolean("allow_unsupported_instructions"),
            };
        }
    }

    private sealed class FunctionMapDto
    {
        public string? Path { get; init; }

        public static FunctionMapDto From(YamlSection section)
        {
            section.RejectUnknown("path");
            return new FunctionMapDto { Path = section.Scalar("path") };
        }
    }

    private sealed class RuntimeDto
    {
        public List<string>? NativeAbiDirectories { get; init; }
        public string? NativeRegistrationRoot { get; init; }
        public string? NativeIndex { get; init; }

        public static RuntimeDto From(YamlSection section)
        {
            section.RejectUnknown("native_abi_directories", "native_registration_root", "native_index");
            return new RuntimeDto
            {
                NativeAbiDirectories = section.Sequence("native_abi_directories"),
                NativeRegistrationRoot = section.Scalar("native_registration_root"),
                NativeIndex = section.Scalar("native_index"),
            };
        }
    }

    private sealed class OutputDto
    {
        public string? Root { get; init; }
        public string? Functions { get; init; }
        public string? RuntimeConfig { get; init; }
        public string? DataInitializer { get; init; }

        public static OutputDto From(YamlSection section)
        {
            section.RejectUnknown("root", "functions", "runtime_config", "data_initializer");
            return new OutputDto
            {
                Root = section.Scalar("root"),
                Functions = section.Scalar("functions"),
                RuntimeConfig = section.Scalar("runtime_config"),
                DataInitializer = section.Scalar("data_initializer"),
            };
        }
    }

    /// <summary>One YAML mapping of the manifest, named by its dotted key path in diagnostics.</summary>
    private sealed class YamlSection
    {
        private readonly YamlMappingNode _node;
        private readonly string _path;

        private YamlSection(YamlMappingNode node, string path)
        {
            _node = node;
            _path = path;
        }

        public static YamlSection Of(YamlNode node, string path) =>
            node is YamlMappingNode mapping
                ? new YamlSection(mapping, path)
                : throw new InvalidDataException($"Translation project key '{path}' must be a mapping.");

        // A plain (unquoted) empty value, ~ or null is YAML's null; a quoted "" stays an empty string.
        public static bool IsNull(YamlNode node) =>
            node is YamlScalarNode { Style: ScalarStyle.Plain or ScalarStyle.Any } scalar &&
            scalar.Value is null or "" or "~" or "null" or "Null" or "NULL";

        public void RejectUnknown(params string[] keys)
        {
            foreach (var key in _node.Children.Keys)
            {
                var name = (key as YamlScalarNode)?.Value;
                if (name is null || Array.IndexOf(keys, name) < 0)
                {
                    throw new InvalidDataException(
                        $"Unknown key '{name ?? key.ToString()}' in translation project section '{_path}'.");
                }
            }
        }

        private YamlNode? Child(string key) =>
            _node.Children.TryGetValue(new YamlScalarNode(key), out var value) && !IsNull(value) ? value : null;

        private string Field(string key) => _path == "(root)" ? key : _path + "." + key;

        public string? Scalar(string key) => Child(key) switch
        {
            null => null,
            YamlScalarNode scalar => scalar.Value,
            _ => throw new InvalidDataException($"Translation project key '{Field(key)}' must be a scalar."),
        };

        public int? Integer(string key) => Scalar(key) switch
        {
            null => null,
            var text when int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) => value,
            var text => throw new InvalidDataException($"Invalid integer '{text}' for {Field(key)}."),
        };

        public bool? Boolean(string key) => Scalar(key) switch
        {
            null => null,
            var text when bool.TryParse(text, out var value) => value,
            var text => throw new InvalidDataException($"Invalid boolean '{text}' for {Field(key)}."),
        };

        public YamlSection? Mapping(string key) => Child(key) is { } node ? Of(node, Field(key)) : null;

        public List<string>? Sequence(string key) => Child(key) switch
        {
            null => null,
            YamlSequenceNode sequence => sequence.Children
                .Select((item, index) => item is YamlScalarNode { Value: { } value } && !IsNull(item)
                    ? value
                    : throw new InvalidDataException($"Translation project key '{Field(key)}[{index}]' must be a scalar."))
                .ToList(),
            _ => throw new InvalidDataException($"Translation project key '{Field(key)}' must be a sequence."),
        };
    }
}

internal sealed record ProjectIdentity(
    string Id,
    string DisplayName,
    string? GameId);
internal sealed record ProjectMemory(uint Base, int Size, uint? SdaBase = null, uint? Sda2Base = null);
internal sealed record ProjectBinaryInput(string Path, string? Sha256);
internal sealed record ProjectInputs(ProjectBinaryInput Dol);
internal sealed record ProjectTranslation(
    IReadOnlyList<uint> EntryPoints,
    string? FunctionMapPath,
    bool AllowUnsupportedInstructions);
internal sealed record ProjectRuntime(
    IReadOnlyList<string> NativeAbiDirectories,
    string NativeRegistrationRoot,
    string NativeIndex);
internal sealed record ProjectOutput(
    string Root,
    string Functions,
    string RuntimeConfig,
    string DataInitializer);
