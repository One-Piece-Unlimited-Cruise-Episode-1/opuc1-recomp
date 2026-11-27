using System.Text.Json.Serialization;

namespace Opuc1Recomp.Setup.Linux;

/// <summary>
/// install-state.json (<see cref="JsonState"/>): read case-insensitively, written indented. Source-generated
/// because the setup ships as Native AOT, where reflection-based System.Text.Json is unavailable.
/// </summary>
[JsonSourceGenerationOptions(PropertyNameCaseInsensitive = true, WriteIndented = true)]
[JsonSerializable(typeof(InstallState))]
internal sealed partial class StateJsonContext : JsonSerializerContext;
