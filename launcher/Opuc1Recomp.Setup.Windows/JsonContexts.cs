using System.Text.Json.Serialization;

namespace Opuc1Recomp.Setup.Windows;

// The setup ships as Native AOT, where reflection-based System.Text.Json is unavailable: every JSON
// document it reads or writes is described by one of these source-generated contexts.

/// <summary>
/// The installation's state documents (<see cref="Common.JsonState"/>) and the payload manifest:
/// read case-insensitively, written indented.
/// </summary>
[JsonSourceGenerationOptions(PropertyNameCaseInsensitive = true, WriteIndented = true)]
[JsonSerializable(typeof(InstallState))]
[JsonSerializable(typeof(ToolkitState))]
[JsonSerializable(typeof(ProductFingerprint))]
[JsonSerializable(typeof(LocalBuildProvenance))]
[JsonSerializable(typeof(InstallTransaction.TransactionJournal))]
[JsonSerializable(typeof(PayloadManifest))]
internal sealed partial class StateJsonContext : JsonSerializerContext;

/// <summary><c>--emit-payload-identities</c>: one line, the record's own property names.</summary>
[JsonSerializable(typeof(ConsoleCommands.PayloadIdentities))]
internal sealed partial class PayloadIdentitiesJsonContext : JsonSerializerContext;

/// <summary>The <c>products</c> record: one line, camelCase.</summary>
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(ConsoleCommands.ProductsRecord))]
internal sealed partial class ProductsJsonContext : JsonSerializerContext;
