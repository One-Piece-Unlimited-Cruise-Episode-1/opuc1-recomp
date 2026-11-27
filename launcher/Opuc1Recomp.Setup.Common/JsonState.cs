using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Opuc1Recomp.Setup.Common;

/// <summary>Reads and atomically writes the small JSON state documents each installer keeps.</summary>
/// <remarks>
/// The setup hosts ship as Native AOT, where reflection-based System.Text.Json is unavailable, so every
/// document type comes from a source-generated context. Each host registers its state types in one
/// context declared with <c>PropertyNameCaseInsensitive = true, WriteIndented = true</c>, the options
/// these documents have always been read and written with.
/// </remarks>
public static class JsonState
{
    public static T? TryRead<T>(string path, JsonSerializerContext context) where T : class
    {
        // Outside the try: a type missing from the context is a programming error, not a damaged
        // document, and must not be swallowed below.
        var typeInfo = TypeInfo<T>(context);
        try
        {
            return File.Exists(path) ? JsonSerializer.Deserialize(File.ReadAllText(path), typeInfo) : null;
        }
        catch
        {
            // A truncated or hand-edited state document must degrade into "unknown", which every
            // caller already treats as "assume stale and rebuild", not into a crash.
            return null;
        }
    }

    public static void Write<T>(string path, T value, JsonSerializerContext context)
    {
        var typeInfo = TypeInfo<T>(context);
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        var tempPath = path + ".tmp-" + Guid.NewGuid().ToString("N");
        File.WriteAllText(tempPath, JsonSerializer.Serialize(value, typeInfo));
        File.Move(tempPath, path, overwrite: true);
    }

    private static JsonTypeInfo<T> TypeInfo<T>(JsonSerializerContext context) =>
        context.GetTypeInfo(typeof(T)) as JsonTypeInfo<T>
        ?? throw new InvalidOperationException(
            $"{typeof(T).Name} is not registered in {context.GetType().Name}; add [JsonSerializable(typeof({typeof(T).Name}))].");
}
