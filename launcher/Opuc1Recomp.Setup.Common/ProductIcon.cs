using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace Opuc1Recomp.Setup.Common;

/// <summary>
/// Builds the product icon from the user's own disc, like tools/make-icon.mjs in a developer checkout. The source
/// is <c>files/memcard/banner_ep1.en.tpl</c>, the game's Wii save banner: TPL image 0 is the 192x64 banner, images 1
/// and 2 are the 48x48 RGB5A3 save icon frames. The icon is game art, so it is generated on the user's machine and
/// never shipped in a release payload.
/// </summary>
public static class ProductIcon
{
    public const string BaseName = "opuc1_recomp";
    public static readonly string TplRelativePath = Path.Combine("files", "memcard", "banner_ep1.en.tpl");
    private const uint TplMagic = 0x0020AF30;
    private const uint GxRgb5A3 = 5;
    private static readonly int[] IcoSizes = [16, 24, 32, 48, 64, 128, 256];

    /// <summary>Writes the .ico, the 256 px and 48 px PNGs and the .rc into <paramref name="outputDirectory"/>.</summary>
    /// <returns>The path of the 256 px PNG.</returns>
    public static string Generate(string dataRoot, string outputDirectory, int frame = 1)
    {
        var tplPath = Path.Combine(dataRoot, TplRelativePath);
        if (!File.Exists(tplPath))
            throw new FileNotFoundException("The game's save banner (the product icon source) is missing.", tplPath);
        var (width, height, rgba) = ReadTpl(File.ReadAllBytes(tplPath), frame);

        Directory.CreateDirectory(outputDirectory);
        var entries = IcoSizes
            .Select(size => (size, png: Png(size == width && size == height ? rgba : Resize(rgba, width, height, size, size),
                size, size)))
            .ToList();
        File.WriteAllBytes(Path.Combine(outputDirectory, BaseName + ".ico"), Ico(entries));
        var png256 = Path.Combine(outputDirectory, BaseName + ".png");
        File.WriteAllBytes(png256, entries[^1].png);
        File.WriteAllBytes(Path.Combine(outputDirectory, BaseName + "_48.png"), Png(rgba, width, height));
        File.WriteAllText(Path.Combine(outputDirectory, BaseName + ".rc"), $"1 ICON \"{BaseName}.ico\"\n",
            new UTF8Encoding(false));
        return png256;
    }

    public static (int Width, int Height, byte[] Rgba) ReadTpl(byte[] buffer, int index)
    {
        try
        {
            var data = buffer.AsSpan();
            if (BinaryPrimitives.ReadUInt32BigEndian(data) != TplMagic)
                throw new InvalidDataException("The save banner is not a TPL file.");
            var count = BinaryPrimitives.ReadUInt32BigEndian(data[4..]);
            var table = (int)BinaryPrimitives.ReadUInt32BigEndian(data[8..]);
            if (index < 0 || index >= count)
                throw new InvalidDataException($"The save banner has {count} images, no image {index}.");
            var header = (int)BinaryPrimitives.ReadUInt32BigEndian(data[(table + index * 8)..]);
            var height = BinaryPrimitives.ReadUInt16BigEndian(data[header..]);
            var width = BinaryPrimitives.ReadUInt16BigEndian(data[(header + 2)..]);
            var format = BinaryPrimitives.ReadUInt32BigEndian(data[(header + 4)..]);
            var offset = (int)BinaryPrimitives.ReadUInt32BigEndian(data[(header + 8)..]);
            if (format != GxRgb5A3)
                throw new InvalidDataException($"Save banner image {index} is GX format {format}, expected 5 (RGB5A3).");
            return (width, height, DecodeRgb5A3(data, offset, width, height));
        }
        catch (ArgumentOutOfRangeException)
        {
            throw new InvalidDataException("The save banner TPL is truncated.");
        }
    }

    /// <summary>RGB5A3: 4x4 texel tiles of big-endian 16-bit texels; top bit set is opaque RGB555, clear is A3 RGB444.</summary>
    private static byte[] DecodeRgb5A3(ReadOnlySpan<byte> data, int offset, int width, int height)
    {
        var pixels = new byte[width * height * 4];
        static byte Expand(int value, int bits) => (byte)((value << (8 - bits)) | (value >> (2 * bits - 8)));
        for (var ty = 0; ty < height; ty += 4)
        for (var tx = 0; tx < width; tx += 4)
        for (var y = 0; y < 4; y++)
        for (var x = 0; x < 4; x++, offset += 2)
        {
            int px = tx + x, py = ty + y;
            if (px >= width || py >= height) continue;
            int value = BinaryPrimitives.ReadUInt16BigEndian(data[offset..]);
            var o = (py * width + px) * 4;
            if ((value & 0x8000) != 0)
            {
                pixels[o] = Expand((value >> 10) & 31, 5);
                pixels[o + 1] = Expand((value >> 5) & 31, 5);
                pixels[o + 2] = Expand(value & 31, 5);
                pixels[o + 3] = 255;
            }
            else
            {
                static byte Nibble(int n) => (byte)((n << 4) | n);
                pixels[o + 3] = (byte)Math.Round(((value >> 12) & 7) * 255 / 7.0, MidpointRounding.AwayFromZero);
                pixels[o] = Nibble((value >> 8) & 15);
                pixels[o + 1] = Nibble((value >> 4) & 15);
                pixels[o + 2] = Nibble(value & 15);
            }
        }
        return pixels;
    }

    /// <summary>Premultiplied-alpha resample: bilinear when upscaling, box filter when downscaling.</summary>
    private static byte[] Resize(byte[] source, int sw, int sh, int dw, int dh)
    {
        var pre = new float[sw * sh * 4];
        for (var i = 0; i < sw * sh; i++)
        {
            var a = source[i * 4 + 3] / 255f;
            pre[i * 4] = source[i * 4] * a;
            pre[i * 4 + 1] = source[i * 4 + 1] * a;
            pre[i * 4 + 2] = source[i * 4 + 2] * a;
            pre[i * 4 + 3] = a;
        }
        var result = new byte[dw * dh * 4];
        double sx = (double)sw / dw, sy = (double)sh / dh;
        Span<double> acc = stackalloc double[4];
        for (var y = 0; y < dh; y++)
        for (var x = 0; x < dw; x++)
        {
            acc.Clear();
            if (dw >= sw)
            {
                var fx = Math.Min(sw - 1, Math.Max(0, (x + 0.5) * sx - 0.5));
                var fy = Math.Min(sh - 1, Math.Max(0, (y + 0.5) * sy - 0.5));
                int x0 = (int)Math.Floor(fx), y0 = (int)Math.Floor(fy);
                int x1 = Math.Min(sw - 1, x0 + 1), y1 = Math.Min(sh - 1, y0 + 1);
                double ax = fx - x0, ay = fy - y0;
                foreach (var (xx, yy, weight) in new[]
                         {
                             (x0, y0, (1 - ax) * (1 - ay)), (x1, y0, ax * (1 - ay)),
                             (x0, y1, (1 - ax) * ay), (x1, y1, ax * ay)
                         })
                    for (var c = 0; c < 4; c++) acc[c] += pre[(yy * sw + xx) * 4 + c] * weight;
            }
            else
            {
                var n = 0;
                for (var yy = (int)Math.Floor(y * sy); yy < (int)Math.Ceiling((y + 1) * sy); yy++)
                for (var xx = (int)Math.Floor(x * sx); xx < (int)Math.Ceiling((x + 1) * sx); xx++, n++)
                    for (var c = 0; c < 4; c++) acc[c] += pre[(yy * sw + xx) * 4 + c];
                for (var c = 0; c < 4; c++) acc[c] /= n;
            }
            var o = (y * dw + x) * 4;
            var alpha = acc[3];
            result[o + 3] = (byte)Math.Round(alpha * 255, MidpointRounding.AwayFromZero);
            for (var c = 0; c < 3; c++)
                result[o + c] = alpha > 0
                    ? (byte)Math.Min(255, Math.Round(acc[c] / alpha, MidpointRounding.AwayFromZero))
                    : (byte)0;
        }
        return result;
    }

    internal static byte[] Png(byte[] rgba, int width, int height)
    {
        using var output = new MemoryStream();
        output.Write([137, 80, 78, 71, 13, 10, 26, 10]);
        var header = new byte[13];
        BinaryPrimitives.WriteUInt32BigEndian(header, (uint)width);
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(4), (uint)height);
        header[8] = 8;
        header[9] = 6;
        WriteChunk(output, "IHDR", header);

        var raw = new byte[(width * 4 + 1) * height];
        for (var y = 0; y < height; y++)
            Buffer.BlockCopy(rgba, y * width * 4, raw, y * (width * 4 + 1) + 1, width * 4);
        using (var compressed = new MemoryStream())
        {
            using (var zlib = new ZLibStream(compressed, CompressionLevel.SmallestSize, leaveOpen: true))
                zlib.Write(raw);
            WriteChunk(output, "IDAT", compressed.ToArray());
        }
        WriteChunk(output, "IEND", []);
        return output.ToArray();
    }

    private static void WriteChunk(Stream output, string type, byte[] data)
    {
        Span<byte> word = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(word, (uint)data.Length);
        output.Write(word);
        var typed = new byte[4 + data.Length];
        Encoding.ASCII.GetBytes(type, typed);
        data.CopyTo(typed, 4);
        output.Write(typed);
        BinaryPrimitives.WriteUInt32BigEndian(word, Crc32(typed));
        output.Write(word);
    }

    private static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n =>
    {
        var c = (uint)n;
        for (var k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    private static uint Crc32(byte[] bytes)
    {
        var c = 0xFFFFFFFFu;
        foreach (var b in bytes) c = CrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    private static byte[] Ico(IReadOnlyList<(int size, byte[] png)> images)
    {
        var header = new byte[6 + 16 * images.Count];
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(4), (ushort)images.Count);
        var offset = header.Length;
        for (var i = 0; i < images.Count; i++)
        {
            var entry = header.AsSpan(6 + i * 16);
            var (size, png) = images[i];
            entry[0] = (byte)(size >= 256 ? 0 : size);
            entry[1] = (byte)(size >= 256 ? 0 : size);
            BinaryPrimitives.WriteUInt16LittleEndian(entry[4..], 1);
            BinaryPrimitives.WriteUInt16LittleEndian(entry[6..], 32);
            BinaryPrimitives.WriteUInt32LittleEndian(entry[8..], (uint)png.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(entry[12..], (uint)offset);
            offset += png.Length;
        }
        using var output = new MemoryStream();
        output.Write(header);
        foreach (var (_, png) in images) output.Write(png);
        return output.ToArray();
    }
}
