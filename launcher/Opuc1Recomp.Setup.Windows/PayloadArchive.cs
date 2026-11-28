using Opuc1Recomp.Setup.Common;
using System.Formats.Tar;
using System.Text;
using System.Text.Json;

namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// The installation payload appended to the setup executable (Build-Installer.ps1, PayloadPacker):
/// <code>
/// [setup host][OPUCPKG2 part index][part 0 .xz]...[part n .xz]["OPUCPAY2"][int64 offset][int64 length]
/// </code>
/// Each part is one xz stream (x86 BCJ + LZMA2, CRC64) of a pax tar. Parts are split by what the setup unpacks
/// separately (the small head with the manifest, licenses and disc tool; the toolkit; the build workspace), so
/// reading the manifest or the disc tool never decodes the large parts. A file identical to an earlier one under
/// the same top-level directory is stored once and appears again as a tar hard-link entry, which extraction turns
/// back into a full copy.
/// </summary>
internal sealed class PayloadArchive : IDisposable
{
    internal static readonly byte[] FooterMagic = Encoding.ASCII.GetBytes("OPUCPAY2");
    internal static readonly byte[] IndexMagic = Encoding.ASCII.GetBytes("OPUCPKG2");
    private static readonly DateTime NormalizedPayloadTimestampUtc =
        new(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    internal const int FooterSize = 24;
    private const int MaxParts = 64;
    private readonly FileStream _executable;
    private readonly long _hostLength;
    private readonly IReadOnlyList<PayloadPart> _parts;

    internal sealed record PayloadPart(string Name, IReadOnlyList<string> TopLevelNames, long Offset, long Length);

    private PayloadArchive(FileStream executable, long hostLength, IReadOnlyList<PayloadPart> parts)
    {
        _executable = executable;
        _hostLength = hostLength;
        _parts = parts;
    }

    internal IReadOnlyList<PayloadPart> Parts => _parts;

    public static PayloadArchive OpenCurrent() =>
        Open(Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate the setup executable."));

    /// <summary>Opens the payload appended to the setup executable at <paramref name="path"/>.</summary>
    internal static PayloadArchive Open(string path)
    {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            if (stream.Length < FooterSize)
                throw new InvalidDataException("This setup executable does not contain an installation payload.");

            stream.Position = stream.Length - FooterSize;
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
            var magic = reader.ReadBytes(FooterMagic.Length);
            var offset = reader.ReadInt64();
            var length = reader.ReadInt64();
            if (!magic.SequenceEqual(FooterMagic) || offset < 0 || length <= 0 || offset + length != stream.Length - FooterSize)
                throw new InvalidDataException("The installation payload is missing or damaged.");

            stream.Position = offset;
            var parts = ReadIndex(reader, length);
            return new PayloadArchive(stream, offset, parts);
        }
        catch (EndOfStreamException)
        {
            stream.Dispose();
            throw new InvalidDataException("The installation payload index is truncated.");
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    private static List<PayloadPart> ReadIndex(BinaryReader reader, long payloadLength)
    {
        if (!reader.ReadBytes(IndexMagic.Length).SequenceEqual(IndexMagic))
            throw new InvalidDataException("The installation payload has an unknown format.");
        var count = reader.ReadInt32();
        if (count is <= 0 or > MaxParts) throw new InvalidDataException("The installation payload index is damaged.");
        var parts = new List<PayloadPart>(count);
        for (var i = 0; i < count; i++)
        {
            var name = reader.ReadString();
            var topLevelCount = reader.ReadInt32();
            if (topLevelCount is <= 0 or > 256) throw new InvalidDataException("The installation payload index is damaged.");
            var topLevel = new List<string>(topLevelCount);
            for (var j = 0; j < topLevelCount; j++) topLevel.Add(reader.ReadString());
            var offset = reader.ReadInt64();
            var length = reader.ReadInt64();
            if (offset <= 0 || length <= 0 || offset + length > payloadLength)
                throw new InvalidDataException("The installation payload index points outside the payload.");
            parts.Add(new PayloadPart(name, topLevel, offset, length));
        }
        return parts;
    }

    public PayloadManifest ReadManifest()
    {
        using var buffer = new MemoryStream();
        if (!TryCopyEntry(InstalledLayout.PayloadManifestFileName, buffer))
            throw new InvalidDataException("The payload manifest is missing.");
        buffer.Position = 0;
        return ParseManifest(buffer);
    }

    internal static PayloadManifest ParseManifest(Stream stream)
    {
        var manifest = JsonSerializer.Deserialize(stream, StateJsonContext.Default.PayloadManifest)
            ?? throw new InvalidDataException("The payload manifest is invalid.");
        ValidateManifest(manifest);
        return manifest;
    }

    internal static void ValidateManifest(PayloadManifest manifest)
    {
        if (manifest.SchemaVersion != PayloadManifest.CurrentSchemaVersion)
            throw new InvalidDataException($"Unsupported payload schema {manifest.SchemaVersion}.");
        // The payload identities are the release-computed replacement for hashing the whole
        // toolkit on the user's machine; a payload without them was packaged incorrectly.
        if (string.IsNullOrWhiteSpace(manifest.ToolkitFingerprint) ||
            string.IsNullOrWhiteSpace(manifest.ToolkitPackageFingerprint) ||
            string.IsNullOrWhiteSpace(manifest.RuntimeAssetsFingerprint))
            throw new InvalidDataException("The payload manifest is missing its content identities.");
        if (manifest.SupportedReleases is not { Count: > 0 })
            throw new InvalidDataException("The payload manifest names no supported game release.");
        foreach (var release in manifest.SupportedReleases)
        {
            if (!release.GameId.Equals(ProductNames.GameId, StringComparison.Ordinal) ||
                !IsSha256(release.DolSha256))
                throw new InvalidDataException(
                    $"The payload manifest describes an unsupported or incomplete release '{release.GameId}'.");
        }
        if (manifest.SupportedReleases.DistinctBy(release => release.GameId).Count() !=
            manifest.SupportedReleases.Count)
            throw new InvalidDataException("The payload manifest lists a game release twice.");
    }

    private static bool IsSha256(string value) =>
        value.Length == 64 && value.All(char.IsAsciiHexDigit);

    /// <summary>Extracts nodtool and the runtime DLLs it loads; returns the nodtool path.</summary>
    public string ExtractDiscTool(string directory)
    {
        var tool = Path.Combine(directory, "nodtool.exe");
        ExtractEntry(InstalledLayout.ToolkitEntryPrefix + "nodtool.exe", tool);
        ExtractDirectory(InstalledLayout.ToolkitEntryPrefix + "Redist", directory);
        return tool;
    }

    /// <summary>
    /// Writes the bare setup host: this executable up to the payload. The release file is exactly that
    /// host with the payload and footer appended (Build-Installer.ps1), so the installed copy is the same
    /// program without carrying a second copy of it inside the payload.
    /// </summary>
    public void ExtractHost(string destination)
    {
        if (_hostLength <= 0)
            throw new InvalidDataException("The installation payload does not follow a setup host.");
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        using (var host = new SliceStream(_executable, 0, _hostLength))
        using (var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
            host.CopyTo(output);
        NormalizeExtractedTimestamp(destination);
    }

    public void ExtractEntry(string entryName, string destination)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        bool found;
        using (var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
            found = TryCopyEntry(entryName, output);
        if (!found)
        {
            File.Delete(destination);
            throw new InvalidDataException($"Payload entry is missing: {entryName}");
        }
        NormalizeExtractedTimestamp(destination);
    }

    /// <summary>Copies one file's content (following hard-link entries) into <paramref name="output"/>.</summary>
    private bool TryCopyEntry(string entryName, Stream output)
    {
        var wanted = NormalizeEntryName(entryName);
        for (var hops = 0; hops < 8; hops++)
        {
            string? linkTarget = null;
            var found = false;
            ForEachEntry(wanted, entry =>
            {
                if (!NormalizeEntryName(entry.Name).Equals(wanted, StringComparison.Ordinal)) return true;
                found = true;
                if (entry.EntryType == TarEntryType.HardLink) linkTarget = NormalizeEntryName(entry.LinkName);
                else entry.DataStream?.CopyTo(output);
                return false;
            });
            if (!found) return false;
            if (linkTarget is null) return true;
            wanted = linkTarget;
        }
        throw new InvalidDataException($"Payload entry {entryName} is a chain of links.");
    }

    public void ExtractDirectory(string prefix, string destination)
    {
        prefix = NormalizeEntryName(prefix).TrimEnd('/') + "/";
        var destinationRoot = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var extracted = new Dictionary<string, string>(StringComparer.Ordinal);
        var unresolved = new List<(string Target, string OutputPath)>();
        ForEachEntry(prefix, entry =>
        {
            var normalizedEntry = NormalizeEntryName(entry.Name);
            if (!normalizedEntry.StartsWith(prefix, StringComparison.Ordinal) || normalizedEntry.EndsWith('/') ||
                entry.EntryType == TarEntryType.Directory)
                return true;
            var relative = normalizedEntry[prefix.Length..].Replace('/', Path.DirectorySeparatorChar);
            var outputPath = Path.GetFullPath(Path.Combine(destinationRoot, relative));
            if (!outputPath.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Unsafe payload path: {entry.Name}");
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            switch (entry.EntryType)
            {
                case TarEntryType.RegularFile or TarEntryType.V7RegularFile:
                    using (var output = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None))
                        entry.DataStream?.CopyTo(output);
                    NormalizeExtractedTimestamp(outputPath);
                    break;
                case TarEntryType.HardLink:
                    var target = NormalizeEntryName(entry.LinkName);
                    if (extracted.TryGetValue(target, out var targetPath))
                    {
                        File.Copy(targetPath, outputPath, overwrite: true);
                        NormalizeExtractedTimestamp(outputPath);
                    }
                    else unresolved.Add((target, outputPath));
                    break;
                default:
                    throw new InvalidDataException($"Payload entry {entry.Name} has unsupported type {entry.EntryType}.");
            }
            extracted[normalizedEntry] = outputPath;
            return true;
        });
        // A copy of a file outside the requested directory (or in a later part) is fetched by name.
        foreach (var (target, outputPath) in unresolved) ExtractEntry(target, outputPath);
    }

    /// <summary>
    /// Visits the tar entries of every part that can hold <paramref name="normalizedName"/> (a file, or a directory
    /// ending in '/'), in payload order, until <paramref name="visit"/> returns false. A part is always decoded to
    /// its end, so its xz integrity checks run even when the wanted entry comes early.
    /// </summary>
    private void ForEachEntry(string normalizedName, Func<TarEntry, bool> visit)
    {
        var topLevelName = TopLevelName(normalizedName);
        var holding = PayloadPacker.PartsHolding(normalizedName);
        foreach (var part in _parts)
        {
            if (!part.TopLevelNames.Contains(topLevelName, StringComparer.Ordinal)) continue;
            if (holding is not null && !holding.Contains(part.Name)) continue;
            using var slice = new SliceStream(_executable, _hostLength + part.Offset, part.Length);
            using var buffered = new BufferedStream(slice, 1 << 16);
            using var xz = new XzDecoderStream(buffered, leaveOpen: true);
            var keepGoing = true;
            using (var tar = new TarReader(xz, leaveOpen: true))
            {
                while (tar.GetNextEntry(copyData: false) is { } entry)
                {
                    if (keepGoing) keepGoing = visit(entry);
                }
            }
            xz.CopyTo(Stream.Null);
            if (!keepGoing) return;
        }
    }

    private static string TopLevelName(string normalizedName)
    {
        var slash = normalizedName.IndexOf('/');
        return slash < 0 ? normalizedName : normalizedName[..slash];
    }

    internal static void NormalizeExtractedTimestamp(string path) =>
        File.SetLastWriteTimeUtc(path, NormalizedPayloadTimestampUtc);

    internal static string NormalizeEntryName(string value) => value.Replace('\\', '/').TrimStart('/');

    public void Dispose() => _executable.Dispose();

    private sealed class SliceStream : Stream
    {
        private readonly Stream _inner;
        private readonly long _offset;
        private readonly long _length;
        private long _position;

        public SliceStream(Stream inner, long offset, long length)
        {
            _inner = inner;
            _offset = offset;
            _length = length;
        }

        public override bool CanRead => true;
        public override bool CanSeek => true;
        public override bool CanWrite => false;
        public override long Length => _length;
        public override long Position { get => _position; set => Seek(value, SeekOrigin.Begin); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count)
        {
            if (_position >= _length) return 0;
            count = (int)Math.Min(count, _length - _position);
            lock (_inner)
            {
                _inner.Position = _offset + _position;
                var read = _inner.Read(buffer, offset, count);
                _position += read;
                return read;
            }
        }
        public override long Seek(long offset, SeekOrigin origin)
        {
            var next = origin switch
            {
                SeekOrigin.Begin => offset,
                SeekOrigin.Current => _position + offset,
                SeekOrigin.End => _length + offset,
                _ => throw new ArgumentOutOfRangeException(nameof(origin))
            };
            if (next < 0 || next > _length) throw new IOException("Attempted to seek outside the payload.");
            return _position = next;
        }
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
