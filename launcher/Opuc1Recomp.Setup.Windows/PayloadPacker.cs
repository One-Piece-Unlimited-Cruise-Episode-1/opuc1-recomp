using System.Buffers.Binary;
using System.Diagnostics;
using System.Formats.Tar;
using System.Security.Cryptography;
using System.Text;

namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// Release-time side of <see cref="PayloadArchive"/> (<c>--pack-payload</c>, run by Build-Installer.ps1): writes the
/// staged payload tree as tar parts, stores each repeated file once (a hard-link entry names the first copy), and
/// compresses every part to xz. <c>--verify-payload</c> then unpacks the finished setup and compares it with the tree.
/// </summary>
internal static class PayloadPacker
{
    internal const string HeadPart = "head";
    internal const string ToolkitPart = "toolkit";
    internal const string WorkspacePart = "workspace";
    private static readonly DateTimeOffset EntryTimestamp = new(2000, 1, 1, 0, 0, 0, TimeSpan.Zero);
    private const UnixFileMode EntryMode =
        UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead | UnixFileMode.OtherRead;

    internal sealed record PartReport(string Name, int Files, int Links, long TarBytes, long CompressedBytes,
        long LinkedBytes);

    private sealed record PlannedFile(string Name, string FullPath, string Part, long Length, string? LinkTarget);

    /// <summary>
    /// The part a payload entry belongs to. The head carries what the setup reads before it commits to an
    /// installation (manifest, licenses, nodtool and the runtime DLLs it loads); it holds no links, so reading it
    /// never needs another part.
    /// </summary>
    internal static string PartOf(string name)
    {
        if (name.Equals(InstalledLayout.PayloadManifestFileName, StringComparison.Ordinal) ||
            name.StartsWith("licenses/", StringComparison.Ordinal) ||
            name.Equals(InstalledLayout.ToolkitEntryPrefix + "nodtool.exe", StringComparison.Ordinal) ||
            name.StartsWith(InstalledLayout.ToolkitEntryPrefix + "Redist/", StringComparison.Ordinal))
            return HeadPart;
        if (name.StartsWith(InstalledLayout.ToolkitEntryPrefix, StringComparison.Ordinal)) return ToolkitPart;
        if (name.StartsWith(InstalledLayout.WorkspaceEntryPrefix, StringComparison.Ordinal)) return WorkspacePart;
        throw new InvalidDataException($"{name} is outside the payload layout (Toolkit, BuildWorkspace, licenses, " +
                                       $"{InstalledLayout.PayloadManifestFileName}).");
    }

    private static readonly string[] PartOrder = [HeadPart, ToolkitPart, WorkspacePart];

    /// <summary>
    /// The parts that can hold <paramref name="normalizedName"/> (a file, or a directory ending in '/'), or null when
    /// the name is outside the layout <see cref="PartOf"/> knows. Lets the reader skip the large parts: the disc tool
    /// and the manifest come from the head alone.
    /// </summary>
    internal static IReadOnlySet<string>? PartsHolding(string normalizedName)
    {
        try
        {
            var parts = new HashSet<string>(StringComparer.Ordinal)
            {
                PartOf(normalizedName.EndsWith('/') ? normalizedName + "_" : normalizedName)
            };
            // Toolkit/ itself spans the head (nodtool.exe, Redist/) and the toolkit part.
            if (normalizedName.Equals(InstalledLayout.ToolkitEntryPrefix, StringComparison.Ordinal))
                parts.Add(HeadPart);
            return parts;
        }
        catch (InvalidDataException)
        {
            return null;
        }
    }

    /// <summary>
    /// Packs <paramref name="payloadRoot"/> into <paramref name="outputPath"/> (the bytes that follow the setup host).
    /// <paramref name="compress"/> turns one part's tar file into its xz file.
    /// </summary>
    internal static List<PartReport> Pack(string payloadRoot, string outputPath, string workDirectory,
        Action<string, string> compress)
    {
        payloadRoot = Path.GetFullPath(payloadRoot);
        var files = Plan(payloadRoot);
        Directory.CreateDirectory(workDirectory);
        var reports = new List<PartReport>();
        var compressed = new List<(string Name, string[] TopLevel, string Path)>();
        foreach (var part in PartOrder)
        {
            var members = files.Where(file => file.Part == part).ToList();
            if (members.Count == 0) continue;
            var tarPath = Path.Combine(workDirectory, part + ".tar");
            var xzPath = tarPath + ".xz";
            WriteTar(tarPath, members);
            if (File.Exists(xzPath)) File.Delete(xzPath);
            compress(tarPath, xzPath);
            if (!File.Exists(xzPath)) throw new InvalidOperationException($"Compressing {part} wrote no {xzPath}.");
            var topLevel = members.Select(file => TopLevelName(file.Name)).Distinct(StringComparer.Ordinal)
                .Order(StringComparer.Ordinal).ToArray();
            compressed.Add((part, topLevel, xzPath));
            reports.Add(new PartReport(part, members.Count(file => file.LinkTarget is null),
                members.Count(file => file.LinkTarget is not null), new FileInfo(tarPath).Length,
                new FileInfo(xzPath).Length, members.Where(file => file.LinkTarget is not null).Sum(file => file.Length)));
            File.Delete(tarPath);
        }

        WritePayload(outputPath, compressed);
        foreach (var part in compressed) File.Delete(part.Path);
        return reports;
    }

    /// <summary>
    /// Every file in payload order (part, then path), with the earlier identical file each repeat links to. A link
    /// stays under its target's top-level directory, so extracting that directory always meets the target first.
    /// </summary>
    private static List<PlannedFile> Plan(string payloadRoot)
    {
        var entries = Directory.EnumerateFiles(payloadRoot, "*", SearchOption.AllDirectories)
            .Select(path => (Name: Path.GetRelativePath(payloadRoot, path).Replace('\\', '/'), FullPath: path))
            .Select(entry => (entry.Name, entry.FullPath, Part: PartOf(entry.Name)))
            .OrderBy(entry => Array.IndexOf(PartOrder, entry.Part))
            .ThenBy(entry => entry.Name, StringComparer.Ordinal)
            .ToList();

        var firstByContent = new Dictionary<(string TopLevel, long Length, string Hash), string>();
        var planned = new List<PlannedFile>(entries.Count);
        foreach (var (name, fullPath, part) in entries)
        {
            var length = new FileInfo(fullPath).Length;
            string? linkTarget = null;
            if (length > 0)
            {
                var key = (TopLevelName(name), length, HashFile(fullPath));
                if (part != HeadPart && firstByContent.TryGetValue(key, out var first)) linkTarget = first;
                else firstByContent.TryAdd(key, name);
            }
            planned.Add(new PlannedFile(name, fullPath, part, length, linkTarget));
        }
        return planned;
    }

    private static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    private static void WriteTar(string tarPath, List<PlannedFile> members)
    {
        using var output = new FileStream(tarPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
        using var writer = new TarWriter(output, TarEntryFormat.Gnu, leaveOpen: false);
        foreach (var file in members)
        {
            if (file.LinkTarget is not null)
            {
                writer.WriteEntry(Stamp(new GnuTarEntry(TarEntryType.HardLink, file.Name) { LinkName = file.LinkTarget }));
                continue;
            }
            using var data = File.OpenRead(file.FullPath);
            writer.WriteEntry(Stamp(new GnuTarEntry(TarEntryType.RegularFile, file.Name) { DataStream = data }));
        }
    }

    // Fixed metadata keeps the tar, and therefore the xz part, a function of the file contents alone.
    private static GnuTarEntry Stamp(GnuTarEntry entry)
    {
        entry.ModificationTime = EntryTimestamp;
        entry.AccessTime = EntryTimestamp;
        entry.ChangeTime = EntryTimestamp;
        entry.Mode = EntryMode;
        entry.Uid = 0;
        entry.Gid = 0;
        entry.UserName = "";
        entry.GroupName = "";
        return entry;
    }

    private static void WritePayload(string outputPath, List<(string Name, string[] TopLevel, string Path)> parts)
    {
        byte[] Index(IReadOnlyList<long> offsets)
        {
            using var buffer = new MemoryStream();
            using (var writer = new BinaryWriter(buffer, Encoding.UTF8, leaveOpen: true))
            {
                writer.Write(PayloadArchive.IndexMagic);
                writer.Write(parts.Count);
                for (var i = 0; i < parts.Count; i++)
                {
                    writer.Write(parts[i].Name);
                    writer.Write(parts[i].TopLevel.Length);
                    foreach (var name in parts[i].TopLevel) writer.Write(name);
                    writer.Write(offsets[i]);
                    writer.Write(new FileInfo(parts[i].Path).Length);
                }
            }
            return buffer.ToArray();
        }

        // The index has a fixed size whatever the offsets are, so measure it once and lay the parts out behind it.
        var offsets = new long[parts.Count];
        var position = (long)Index(offsets).Length;
        for (var i = 0; i < parts.Count; i++)
        {
            offsets[i] = position;
            position += new FileInfo(parts[i].Path).Length;
        }

        using var output = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
        output.Write(Index(offsets));
        foreach (var part in parts)
        {
            using var input = File.OpenRead(part.Path);
            input.CopyTo(output);
        }
    }

    /// <summary>Compression with xz: x86 BCJ + LZMA2 preset 9e, CRC64, fixed-size blocks.</summary>
    internal static Action<string, string> XzCompressor(string xzPath) => (tarPath, xzOutput) =>
    {
        var start = new ProcessStartInfo(xzPath)
        {
            UseShellExecute = false,
            RedirectStandardError = true
        };
        // The fixed block size makes the output independent of how many threads this machine gives xz.
        foreach (var argument in new[]
                 {
                     "--x86", "--lzma2=preset=9e", "--check=crc64", "--threads=0", "--block-size=192MiB",
                     "--keep", "--force", "--quiet", tarPath
                 })
            start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new InvalidOperationException($"Could not start {xzPath}.");
        var errors = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0) throw new InvalidOperationException($"xz failed ({process.ExitCode}): {errors}");
        if (!xzOutput.Equals(tarPath + ".xz", StringComparison.OrdinalIgnoreCase))
            File.Move(tarPath + ".xz", xzOutput, overwrite: true);
    };

    /// <summary>
    /// An xz file holding <paramref name="tarPath"/> in uncompressed LZMA2 chunks. Valid xz that needs no xz binary,
    /// so the self-tests can build and read payloads anywhere.
    /// </summary>
    internal static void WriteStoredXz(string tarPath, string xzOutput)
    {
        var data = File.ReadAllBytes(tarPath);
        using var output = new FileStream(xzOutput, FileMode.Create, FileAccess.Write, FileShare.None);
        Span<byte> word = stackalloc byte[8];

        byte[] streamFlags = [0x00, 0x04];
        output.Write([0xFD, (byte)'7', (byte)'z', (byte)'X', (byte)'Z', 0x00]);
        output.Write(streamFlags);
        BinaryPrimitives.WriteUInt32LittleEndian(word, XzDecoderStream.Crc32.Compute(streamFlags));
        output.Write(word[..4]);

        // Block header: size, flags (one filter), LZMA2 (0x21) with a 1-byte property, padding, CRC32.
        byte[] blockHeader = [0x02, 0x00, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00, 0, 0, 0, 0];
        BinaryPrimitives.WriteUInt32LittleEndian(blockHeader.AsSpan(8), XzDecoderStream.Crc32.Compute(blockHeader.AsSpan(0, 8)));
        output.Write(blockHeader);

        long compressed = 0;
        for (var offset = 0; offset < data.Length; offset += 65536)
        {
            var size = Math.Min(65536, data.Length - offset);
            output.WriteByte(offset == 0 ? (byte)0x01 : (byte)0x02);
            output.WriteByte((byte)((size - 1) >> 8));
            output.WriteByte((byte)(size - 1));
            output.Write(data, offset, size);
            compressed += 3 + size;
        }
        output.WriteByte(0x00);
        compressed++;
        for (var padding = compressed; (padding & 3) != 0; padding++) output.WriteByte(0);

        var crc64 = new XzDecoderStream.Crc64();
        crc64.Append(data);
        BinaryPrimitives.WriteUInt64LittleEndian(word, crc64.Value);
        output.Write(word);

        var index = new List<byte> { 0x00 };
        void Vli(ulong value)
        {
            while (value >= 0x80) { index.Add((byte)(value | 0x80)); value >>= 7; }
            index.Add((byte)value);
        }
        Vli(1);
        Vli((ulong)(blockHeader.Length + compressed + 8));
        Vli((ulong)data.Length);
        while ((index.Count & 3) != 0) index.Add(0);
        var indexBytes = index.ToArray();
        output.Write(indexBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(word, XzDecoderStream.Crc32.Compute(indexBytes));
        output.Write(word[..4]);

        var footer = new byte[12];
        BinaryPrimitives.WriteUInt32LittleEndian(footer.AsSpan(4), (uint)((indexBytes.Length + 4) / 4 - 1));
        streamFlags.CopyTo(footer.AsSpan(8));
        footer[10] = (byte)'Y';
        footer[11] = (byte)'Z';
        BinaryPrimitives.WriteUInt32LittleEndian(footer, XzDecoderStream.Crc32.Compute(footer.AsSpan(4, 6)));
        output.Write(footer);
    }

    /// <summary>
    /// Unpacks every part of the setup at <paramref name="setupPath"/> the way an installation does and compares
    /// the result with <paramref name="payloadRoot"/> file by file. Returns the number of files compared.
    /// </summary>
    internal static int Verify(string setupPath, string payloadRoot, string scratch)
    {
        payloadRoot = Path.GetFullPath(payloadRoot);
        if (Directory.Exists(scratch)) Directory.Delete(scratch, recursive: true);
        Directory.CreateDirectory(scratch);
        try
        {
            using (var payload = PayloadArchive.Open(setupPath))
            {
                payload.ReadManifest();
                payload.ExtractDiscTool(Path.Combine(scratch, "disc-tool"));
                payload.ExtractDirectory(InstalledLayout.ToolkitDirectoryName, Path.Combine(scratch, "tree", InstalledLayout.ToolkitDirectoryName));
                payload.ExtractDirectory(InstalledLayout.WorkspaceDirectoryName, Path.Combine(scratch, "tree", InstalledLayout.WorkspaceDirectoryName));
                payload.ExtractDirectory("licenses", Path.Combine(scratch, "tree", "licenses"));
                payload.ExtractEntry(InstalledLayout.PayloadManifestFileName,
                    Path.Combine(scratch, "tree", InstalledLayout.PayloadManifestFileName));
            }

            var tree = Path.Combine(scratch, "tree");
            var expected = Directory.EnumerateFiles(payloadRoot, "*", SearchOption.AllDirectories)
                .Select(path => Path.GetRelativePath(payloadRoot, path)).Order(StringComparer.OrdinalIgnoreCase).ToList();
            var actual = Directory.EnumerateFiles(tree, "*", SearchOption.AllDirectories)
                .Select(path => Path.GetRelativePath(tree, path)).Order(StringComparer.OrdinalIgnoreCase).ToList();
            var missing = expected.Except(actual, StringComparer.OrdinalIgnoreCase).Take(5).ToList();
            var extra = actual.Except(expected, StringComparer.OrdinalIgnoreCase).Take(5).ToList();
            if (missing.Count > 0 || extra.Count > 0)
                throw new InvalidDataException($"The packed payload does not reproduce the staged tree: missing " +
                                               $"[{string.Join(", ", missing)}], extra [{string.Join(", ", extra)}].");
            foreach (var relative in expected)
            {
                if (!FilesEqual(Path.Combine(payloadRoot, relative), Path.Combine(tree, relative)))
                    throw new InvalidDataException($"The packed payload changes {relative}.");
            }

            var discTool = Path.Combine(scratch, "disc-tool");
            if (!FilesEqual(Path.Combine(payloadRoot, InstalledLayout.ToolkitDirectoryName, "nodtool.exe"),
                    Path.Combine(discTool, "nodtool.exe")))
                throw new InvalidDataException("The packed payload's disc tool differs from the staged nodtool.exe.");
            return expected.Count;
        }
        finally
        {
            if (Directory.Exists(scratch)) Directory.Delete(scratch, recursive: true);
        }
    }

    private static bool FilesEqual(string left, string right)
    {
        var leftInfo = new FileInfo(left);
        var rightInfo = new FileInfo(right);
        if (!leftInfo.Exists || !rightInfo.Exists || leftInfo.Length != rightInfo.Length) return false;
        using var a = new FileStream(left, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        using var b = new FileStream(right, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        var bufferA = new byte[1 << 16];
        var bufferB = new byte[1 << 16];
        while (true)
        {
            var read = a.Read(bufferA);
            if (read == 0) return true;
            b.ReadExactly(bufferB, 0, read);
            if (!bufferA.AsSpan(0, read).SequenceEqual(bufferB.AsSpan(0, read))) return false;
        }
    }

    private static string TopLevelName(string name)
    {
        var slash = name.IndexOf('/');
        return slash < 0 ? name : name[..slash];
    }
}
