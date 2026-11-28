using System.Buffers.Binary;
using System.Security.Cryptography;
using Tomlyn;
using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal static class SelfTests
{
    public static int Run()
    {
        var failures = new List<string>();
        Test("ISO extension", () => TestAcceptedExtension(".iso"), failures);
        Test("GCM extension", () => TestAcceptedExtension(".gcm"), failures);
        Test("GCZ extension", () => TestAcceptedExtension(".gcz"), failures);
        Test("CISO extension", () => TestAcceptedExtension(".ciso"), failures);
        Test("WBFS extension", () => TestAcceptedExtension(".wbfs"), failures);
        Test("WIA extension", () => TestAcceptedExtension(".wia"), failures);
        Test("RVZ extension", () => TestAcceptedExtension(".RVZ"), failures);
        Test("Unsupported extension", () =>
        {
            var path = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".nfs");
            File.WriteAllBytes(path, [0]);
            try
            {
                try { InputValidation.ValidateExtension(path); throw new Exception("NFS was accepted."); }
                catch (InvalidDataException) { }
            }
            finally { File.Delete(path); }
        }, failures);
        Test("Disc compatibility", TestDiscCompatibility, failures);
        Test("Payload manifest releases", TestPayloadManifestReleases, failures);
        Test("Extracted game release validation", TestExtractedGameRelease, failures);
        Test("Payload timestamp normalization", TestPayloadTimestampNormalization, failures);
        Test("Installed setup copy from the host bytes", TestPayloadHostExtraction, failures);
        Test("Payload packing round trip", TestPayloadPackRoundTrip, failures);
        Test("xz decoder", TestXzDecoder, failures);
        Test("Workspace timestamp reconciliation", TestWorkspaceTimestampReconciliation, failures);
        Test("Toolkit accompanies the workspace", TestToolkitRefreshDecision, failures);
        Test("Host CPU requirement", TestHostCpuRequirement, failures);
        Test("Shared runtime configuration", () => TestPathSettingRoundTrip("dvd_root",
            RuntimeConfiguration.SetDvdRoot, RuntimeConfiguration.RemoveDvdRootIfOwned), failures);
        Test("Runtime configuration key matching", TestRuntimeConfigurationKeyMatching, failures);
        Test("Runtime configuration quoted keys", TestRuntimeConfigurationQuotedKeys, failures);
        Test("Install location validation", TestInstallLocationValidation, failures);
        Test("Command line contract", TestCommandLineContract, failures);
        Test("Portable root discovery", TestPortableRootDiscovery, failures);
        Test("Portable install validation failure leaves no marker",
            TestPortableValidationFailureLeavesNoMarker, failures);
        Test("Portable configuration path resolution", TestPortableConfigPathResolution, failures);
        Test("Portable relative path settings", TestPortableRelativePathSettings, failures);
        Test("Portable move healing", TestPortableMoveHealing, failures);
        Test("Install transaction rollback", TestInstallTransactionRollback, failures);
        Test("Install move transient-lock policy", TestInstallMoveTransientLockPolicy, failures);
        Test("Install scratch recovery", TestInstallScratchRecovery, failures);
        Test("Shortcut round trip", TestShortcutRoundTrip, failures);
        Test("Shortcut cleanup is best effort", TestShortcutCleanupIsBestEffort, failures);
        Test("Toolkit fingerprint", TestToolkitFingerprint, failures);
        Test("Game inspection state machine", TestGameStateMachine, failures);
        Test("Products report contract", TestProductsReport, failures);
        Test("Build progress step ids", TestBuildProgressSteps, failures);
        Test("Product icon from the save banner", TestProductIcon, failures);
        Test("SYSCONF seeding", TestSysconfSeed, failures);
        Test("Product names match the runtime", TestProductNames, failures);
        foreach (var failure in failures) Console.Error.WriteLine("FAILED " + failure);
        Console.Out.WriteLine(failures.Count == 0
            ? "All self-tests passed."
            : $"{failures.Count} self-test(s) failed.");
        return failures.Count == 0 ? 0 : 1;
    }

    private static readonly string TestDolSha256 = new('1', 64);

    private static PayloadManifest OpucManifest() => new()
    {
        SchemaVersion = PayloadManifest.CurrentSchemaVersion,
        ToolkitFingerprint = "toolkit", ToolkitPackageFingerprint = "package", RuntimeAssetsFingerprint = "assets",
        SupportedReleases = [new GameRelease { GameId = ProductNames.GameId, DolSha256 = TestDolSha256 }]
    };

    private static void TestDiscCompatibility()
    {
        var manifest = OpucManifest();
        var selected = InputValidation.EnsureCompatibleDisc(new DiscHeader { GameId = "roupaf" }, manifest);
        if (!ReferenceEquals(selected, manifest.SupportedReleases[0]))
            throw new Exception("ROUPAF did not select its own release.");

        // Other regions of the game and unrelated discs are refused with a message naming the supported release.
        foreach (var unsupported in new[] { "ROUEAF", "ROUJAF", "ROUPAG", "RSBE01", "RSPP01" })
        {
            try
            {
                InputValidation.EnsureCompatibleDisc(new DiscHeader { GameId = unsupported }, manifest);
                throw new Exception($"{unsupported} was accepted.");
            }
            catch (InvalidDataException ex) when (ex.Message.Contains("PAL (ROUPAF)", StringComparison.Ordinal) &&
                                                  ex.Message.Contains(unsupported, StringComparison.Ordinal)) { }
        }
    }

    private static void TestPayloadManifestReleases()
    {
        PayloadArchive.ValidateManifest(OpucManifest());

        // The shape Build-Installer.ps1 writes (PowerShell's ConvertTo-Json of the ordered manifest).
        var dol = new string('a', 64);
        var json = $$"""
            {
                "SchemaVersion":  1,
                "ProductVersion":  "0.1.0",
                "SupportedReleases":  [
                    { "GameId":  "ROUPAF", "DolSha256":  "{{dol}}" }
                ],
                "ToolkitReleaseTag":  "",
                "BuildModel":  "local-translation-and-compilation",
                "ToolkitFingerprint":  "toolkit",
                "ToolkitPackageFingerprint":  "package",
                "RuntimeAssetsFingerprint":  "assets",
                "TranslationFingerprint":  "translation",
                "NativeToolchainFingerprint":  "native"
            }
            """;
        using (var stream = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(json)))
        {
            var parsed = PayloadArchive.ParseManifest(stream);
            if (parsed.SupportedReleases.Count != 1 || parsed.SupportedReleases[0].GameId != ProductNames.GameId ||
                parsed.SupportedReleases[0].DolSha256 != dol)
                throw new Exception("The packaged release list was not read back exactly.");
        }

        void Reject(string description, Action<PayloadManifest> corrupt)
        {
            var manifest = OpucManifest();
            corrupt(manifest);
            try
            {
                PayloadArchive.ValidateManifest(manifest);
                throw new Exception($"A manifest with {description} was accepted.");
            }
            catch (InvalidDataException) { }
        }

        Reject("another schema", manifest => manifest.SchemaVersion = 3);
        Reject("no releases", manifest => manifest.SupportedReleases.Clear());
        Reject("an unsupported release", manifest => manifest.SupportedReleases[0].GameId = "RSBE01");
        Reject("a missing input digest", manifest => manifest.SupportedReleases[0].DolSha256 = "");
        Reject("a duplicated release", manifest =>
            manifest.SupportedReleases.Add(new GameRelease { GameId = ProductNames.GameId, DolSha256 = dol }));
    }

    private static void TestExtractedGameRelease()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-release-" + Guid.NewGuid().ToString("N"));
        try
        {
            var data = Path.Combine(root, "GameAssets", "DATA");
            WriteFakeExtractedDisc(data);
            var release = new GameRelease
            {
                GameId = ProductNames.GameId,
                DolSha256 = InputValidation.Sha256File(Path.Combine(data, "sys", "main.dol"))
            };

            InstallerEngine.ValidateExtractedGame(data, release);
            if (Installation.ReadDiscGameId(data) != ProductNames.GameId)
                throw new Exception("The installed game id was not read from the extracted disc header.");

            void Reject(string description, GameRelease expected)
            {
                try
                {
                    InstallerEngine.ValidateExtractedGame(data, expected);
                    throw new Exception($"{description} was accepted.");
                }
                catch (InvalidDataException) { }
            }

            Reject("A modified main.dol", new GameRelease { GameId = ProductNames.GameId, DolSha256 = TestDolSha256 });
            Reject("A disc header of another region",
                new GameRelease { GameId = "ROUEAF", DolSha256 = release.DolSha256 });
            File.Delete(Path.Combine(data, ProductIcon.TplRelativePath));
            Reject("An extraction without the save banner", release);
            WriteFakeExtractedDisc(data);
            File.Delete(Path.Combine(data, "sys", "boot.bin"));
            Reject("An extraction without a disc header", release);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>The files an extracted ROUPAF disc must have, with stand-in contents.</summary>
    private static void WriteFakeExtractedDisc(string data)
    {
        Directory.CreateDirectory(Path.Combine(data, "sys"));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(data, ProductIcon.TplRelativePath))!);
        File.WriteAllText(Path.Combine(data, "sys", "main.dol"), "stand-in dol");
        File.WriteAllText(Path.Combine(data, "sys", "fst.bin"), "fst");
        File.WriteAllText(Path.Combine(data, "sys", "boot.bin"), ProductNames.GameId + new string('\0', 26));
        File.WriteAllBytes(Path.Combine(data, ProductIcon.TplRelativePath), FakeBannerTpl());
    }

    /// <summary>
    /// A save banner with the game's layout (image 0 a 192x64 banner, images 1 and 2 48x48 icon frames, all RGB5A3),
    /// filled with a synthetic pattern. No game art is involved.
    /// </summary>
    private static byte[] FakeBannerTpl()
    {
        (int W, int H)[] images = [(192, 64), (48, 48), (48, 48)];
        const int table = 12;
        var headers = table + images.Length * 8;
        var dataStart = headers + images.Length * 36;
        var size = dataStart + images.Sum(image => image.W * image.H * 2);
        var tpl = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(tpl, 0x0020AF30);
        BinaryPrimitives.WriteUInt32BigEndian(tpl.AsSpan(4), (uint)images.Length);
        BinaryPrimitives.WriteUInt32BigEndian(tpl.AsSpan(8), table);
        var data = dataStart;
        for (var i = 0; i < images.Length; i++)
        {
            var header = headers + i * 36;
            BinaryPrimitives.WriteUInt32BigEndian(tpl.AsSpan(table + i * 8), (uint)header);
            BinaryPrimitives.WriteUInt16BigEndian(tpl.AsSpan(header), (ushort)images[i].H);
            BinaryPrimitives.WriteUInt16BigEndian(tpl.AsSpan(header + 2), (ushort)images[i].W);
            BinaryPrimitives.WriteUInt32BigEndian(tpl.AsSpan(header + 4), 5);
            BinaryPrimitives.WriteUInt32BigEndian(tpl.AsSpan(header + 8), (uint)data);
            for (var t = 0; t < images[i].W * images[i].H; t++)
            {
                // Alternate an opaque RGB555 texel and a translucent A3RGB444 texel.
                var texel = (t & 1) == 0 ? (ushort)(0x8000 | ((t % 32) << 10) | 0x1F) : (ushort)((3 << 12) | 0x0F0);
                BinaryPrimitives.WriteUInt16BigEndian(tpl.AsSpan(data + t * 2), texel);
            }
            data += images[i].W * images[i].H * 2;
        }
        return tpl;
    }

    private static void TestAcceptedExtension(string extension)
    {
        var path = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + extension);
        File.WriteAllBytes(path, [0]);
        try { InputValidation.ValidateExtension(path); } finally { File.Delete(path); }
    }

    private static void TestPayloadTimestampNormalization()
    {
        var path = Path.Combine(Path.GetTempPath(), "opuc1-timestamp-" + Guid.NewGuid().ToString("N"));
        try
        {
            File.WriteAllText(path, "timestamp");
            File.SetLastWriteTimeUtc(path, DateTime.UtcNow.AddDays(1));
            PayloadArchive.NormalizeExtractedTimestamp(path);
            if (File.GetLastWriteTimeUtc(path).Year != 2000)
                throw new Exception("Extracted payload timestamps were not normalized.");
        }
        finally
        {
            File.Delete(path);
        }
    }

    /// <summary>
    /// The installed setup copy is the release file's leading host bytes (the payload carries no second
    /// copy): exactly the bytes before the payload, not one more or fewer.
    /// </summary>
    private static void TestPayloadHostExtraction()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-host-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(root);
            var tree = Path.Combine(root, "tree");
            WriteTestFile(tree, "licenses/x.txt", "x");
            var host = new byte[70_001];
            new Random(1).NextBytes(host);
            var setup = BuildTestSetup(root, tree, host);

            var extracted = Path.Combine(root, "installed", ProductInfo.SetupCopyName);
            using (var payload = PayloadArchive.Open(setup))
                payload.ExtractHost(extracted);
            if (!File.ReadAllBytes(extracted).AsSpan().SequenceEqual(host))
                throw new Exception("The installed setup copy is not the release file's host bytes.");
            if (File.GetLastWriteTimeUtc(extracted).Year != 2000)
                throw new Exception("The installed setup copy's timestamp was not normalized.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void WriteTestFile(string tree, string relative, string content)
    {
        var path = Path.Combine(tree, relative.Replace('/', Path.DirectorySeparatorChar));
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, content);
    }

    /// <summary>A setup file laid out like a release (host, packed payload, footer), packed without an xz binary.</summary>
    private static string BuildTestSetup(string root, string tree, byte[] host)
    {
        var payloadPath = Path.Combine(root, "payload.bin");
        var reports = PayloadPacker.Pack(tree, payloadPath, Path.Combine(root, "parts"), PayloadPacker.WriteStoredXz);
        _lastTestPackReports = reports;
        var setup = Path.Combine(root, "setup.exe");
        using (var output = File.Create(setup))
        {
            output.Write(host);
            using (var payload = File.OpenRead(payloadPath)) payload.CopyTo(output);
            output.Write(PayloadArchive.FooterMagic);
            output.Write(BitConverter.GetBytes((long)host.Length));
            output.Write(BitConverter.GetBytes(new FileInfo(payloadPath).Length));
        }
        return setup;
    }

    private static List<PayloadPacker.PartReport> _lastTestPackReports = [];

    /// <summary>
    /// A packed payload unpacks to exactly the staged tree, repeated files included: a copy of the head's runtime
    /// DLL elsewhere in the toolkit and a repeated workspace file are stored once and come back as full files.
    /// </summary>
    private static void TestPayloadPackRoundTrip()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-pack-" + Guid.NewGuid().ToString("N"));
        try
        {
            var tree = Path.Combine(root, "tree");
            var manifest = System.Text.Json.JsonSerializer.Serialize(OpucManifest(), StateJsonContext.Default.PayloadManifest);
            WriteTestFile(tree, InstalledLayout.PayloadManifestFileName, manifest);
            WriteTestFile(tree, "licenses/LICENSE.txt", "license text");
            WriteTestFile(tree, "Toolkit/nodtool.exe", "nodtool bytes");
            WriteTestFile(tree, "Toolkit/Redist/vcruntime140.dll", "runtime dll");
            WriteTestFile(tree, "Toolkit/CMake/bin/vcruntime140.dll", "runtime dll");
            WriteTestFile(tree, "Toolkit/Ninja/vcruntime140.dll", "runtime dll");
            WriteTestFile(tree, "Toolkit/llvm-mingw/bin/clang.exe", new string('c', 300_000));
            WriteTestFile(tree, "Toolkit/empty.txt", "");
            WriteTestFile(tree, "BuildWorkspace/runtime/src/a.cpp", "int a();");
            WriteTestFile(tree, "BuildWorkspace/Dependencies/x/a.cpp", "int a();");
            WriteTestFile(tree, "BuildWorkspace/Dependencies/x/runtime.dll", "runtime dll");
            var setup = BuildTestSetup(root, tree, [1, 2, 3, 4]);

            var links = _lastTestPackReports.Sum(part => part.Links);
            // Toolkit: CMake and Ninja copies of the head's DLL. Workspace: the second a.cpp. The workspace DLL is not
            // linked to the toolkit's: a link never crosses top-level directories.
            if (links != 3) throw new Exception($"Expected 3 repeated files stored once, found {links}.");
            if (_lastTestPackReports.Single(part => part.Name == PayloadPacker.HeadPart).Links != 0)
                throw new Exception("The head part holds a link.");

            var count = PayloadPacker.Verify(setup, tree, Path.Combine(root, "verify"));
            if (count != 11) throw new Exception($"Verification compared {count} files, expected 11.");

            using var payload = PayloadArchive.Open(setup);
            if (payload.ReadManifest().ToolkitFingerprint != "toolkit") throw new Exception("The manifest did not round-trip.");
            var single = Path.Combine(root, "single", "vcruntime140.dll");
            payload.ExtractEntry("Toolkit/Ninja/vcruntime140.dll", single);
            if (File.ReadAllText(single) != "runtime dll") throw new Exception("A linked entry did not extract on its own.");
            var tools = Path.Combine(root, "tools");
            payload.ExtractDirectory("Toolkit/CMake", tools);
            if (File.ReadAllText(Path.Combine(tools, "bin", "vcruntime140.dll")) != "runtime dll")
                throw new Exception("A link whose target lies outside the extracted directory was not resolved.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The decoder reads what xz itself writes (x86 BCJ + LZMA2 preset 9e, CRC64, made with xz 5.8.3) and rejects
    /// the same file with one byte changed.
    /// </summary>
    private static void TestXzDecoder()
    {
        var compressed = Convert.FromBase64String(XzFixture);
        string Decode(byte[] bytes)
        {
            using var xz = new XzDecoderStream(new MemoryStream(bytes));
            using var output = new MemoryStream();
            xz.CopyTo(output);
            return Convert.ToHexStringLower(SHA256.HashData(output.ToArray()));
        }

        var hash = Decode(compressed);
        if (hash != "c36eab388c943c45dd4b40088112e2015100290e83f10388192db179a63a23db")
            throw new Exception($"The xz fixture decoded to {hash}.");
        foreach (var position in new[] { 40, compressed.Length / 2, compressed.Length - 30 })
        {
            var damaged = (byte[])compressed.Clone();
            damaged[position] ^= 0x20;
            try
            {
                Decode(damaged);
                throw new Exception($"A damaged xz file (byte {position}) was accepted.");
            }
            catch (InvalidDataException) { }
        }
        try
        {
            Decode(compressed[..^20]);
            throw new Exception("A truncated xz file was accepted.");
        }
        catch (InvalidDataException) { }
    }

    private const string XzFixture =
        "/Td6WFoAAATm1rRGBMHPD4B6BAAhARwAAAAAAF1APSTgPP8Hx10AJ5QG4NF6t1BLyn4FpkD5jQMRP88DSb/HUE75r9UK6DXUdN44lhk4xqpFYm3K" +
        "YYgOOwQgmSdZ1YTPO8MJCmrrb67qaLykWpQWRXJNnRsy3yTDuNM6l4gKALNObLB8z7WGPYakm56eaqHQASSKxEJT3aCumQswFq3rHW0bW5eUIXcU" +
        "SAM/3G8lL59uqzu7mXTWG+QJlBp5MODgqfrAT99GPWqaa9P0xSBFhNdBuutjspIq8+hTmJsduDoTsMRMJYM7MeFlRYauHQwvNSlQ2cx82BJ/y6Lk" +
        "E9QPtwk77zvFeBO9XrqM01i4w+FEMxVcFSKpVvUrL1K/Gj7GSNwwVezs9ym9qiVbcehpLlhcPFBSwwXRvGV+PQaY48esKmQmmdY/Ymn4HAoSIqgB" +
        "qvvIUYsrgmi84ipTWRm6V6cZQAnmiVyns5AS7EjpDPNoYs0E48hkT9ZFo6Nzy60gTzcYGfWgsYzhaHTTNPZLVbmXrQXYr7YS1WyiWC3mtI3gpQlw" +
        "tBL8d0/OnMQTtUp6GeipGQNXzyLuWJyHSMkJ/AviaopZxhMcjfa05xLieJnDJZMAZXY2OQbKlMrAIkjG5DgJ0BKLZpYEtWnw3HP397ea3H4jsCTC" +
        "UxAcy0bPrC7X8R7glMDXymsXDbmDol6ZdiR3+ItCawekY8wz83AZVRhtvOzYrXb4UyeU5k9tbQoLPIvy7UDGzwMO6Ro3EakYQIQKgKd6DHlLd/iP" +
        "3Ha6kdfByWkQ/JY3TEPLfa3YLZUrhyMP5zHTacaYC94QSXKa3GJsFtcwqusBxWK7NI3A2F3SiFegaZ4EJlCMruceWNFiBxHzp56hT0SbdJiu49pb" +
        "02BBz5xThITglOWZHAMU5A+EYXTZEdaI0J6WJsqYpw8RAreaNXIr6PpLjlNk0OuJcD524prMHVFV+8StDR27ojGSUs6tHwDLQgFZSHzsZ8hgktMb" +
        "rGNeVoER0vWQbA88hYsZFe+V2f8ZpEp8lJG/X5t+G0lcjsYz7rElqDg7lZuGi4G7ZsdJToM5GBSPP9tvMAldiTebOuHeEADj1DQE7gYS57QjSB7s" +
        "v1NjMp1cfHn1drAImsfk03JzQ0RlUc6OK+A9J5QE6nN0EXLSCOHXRz4frskQQNwpwCC4dzdO2YdJQl75NxEyRIRUc1oRlPNBVVf/hl08nwYehQ23" +
        "nQRQcWx3wZ3SDDtc4awFtft+kMo6CngeQQ/WQkjVW96xwxBwbyfOPoYfD/loQY73RVLC2nCOv/wjtRyCFfh3gu4C1aaMmB8NOZm8J3ApQupIiehW" +
        "6TaJQm1oo7uLFnKBAtcU2nTUeGSX6Lg20dVXvSIC/34vEnbchu3Z3ZchCClfCerODOrnlRhtJhYf5MkTwunzvrfZAUC0ln1C+2Cd8xwJq2t5KjVD" +
        "MHkuy7/wiLCJfFvXsqWgpeChvHtgUgy0oA1C5s6Cop6cPoxZHBCjZqMPb9JUwhHDd++lcs2GEPtASWda1phxMkQqLciAf+trpWPbp1fzsYEadEwX" +
        "Q/S5UidsE+vdZ8DaSsYihyoa4g3uEPCIiTzzw4WiawTQSEsiNz4mzoYS0yMn+2eNQhh19vZmf1KaoxiiMSUlicZOdVNBdEyzQHUoSzVTt+mNR3DI" +
        "Fcreoz65fA7qLDLeAb5HKkd3a+ppQcIv6VEF5iBzmbE2bBLC6uEkcWMYfZDmbfk8TfDixVjiWtJ/fLrVKmFeT1j8XORIx9Q7YCBSTs5yevFRjRXX" +
        "PuGgyKrWdwWbtcMnG3TfqwNd6XMNQW+6g7PafLPpW11qSz7S6dSNY96rxDd07nqsWb2pMkovvFFsKihNjMK3LbUfIxy5F+wyCvuv5s8f0GxbzFoi" +
        "UAQATyYvuTDjg/6zn5VaISREUZUd6KMemVaBZ7PHh580IjAa+/uljiTHUAv94rK3s+diFKXOv/dy8XcboQghZZvXxHsqrIQcXpQGARE+ANRtvVEQ" +
        "+t4bIlKRoqxxx8LbfmNLz6771oHtnKjlOmoTQxYnNBHKKcvh/mjw3IF/YRQickS+3D/clQMitzCvp0XHks2HKmSHWKhXsB9CGV28Ote0ArRvLnP+" +
        "8RNsU+oWQB3EY4yrV7aQ2lePNRW2D4QEhXqVIrJxggb8OBmJeQmFyc0mTacPf09s8ZndcVPsDZs7SHmZ6wioE+gBhU1aJ4WQ72zvnAO1Y3JquSOV" +
        "fIyn/bOIMbwF5c/W7T3a69VWcOKbWeuMUME9Re0rfmw0lekgtHXcJzz2v4z6bJ2tkhbWGW2/zEhaaXA2v3zY4g3bqt52B85ORBytWaDnVMlBiBYf" +
        "+jCG4/ZckuuUbOvgCvjMM30kPp78VbW3vKqHkSGOaDyp3qQu3wG+QB65LN5/foXtUu899WXrZ4N+TzS1PbxjiKV5RQhs7khwrkeOr0ol4IStfdAQ" +
        "ojIhelOsur6A8Eh4Dz5MnSGcE/OKBl5pPguzYqHhtJw5g/n13wSxr2v9FGdbeookmKCqFkp3gPHPYHTHxumS/KL7RI3y49YbN+l31pjUbWX07TJM" +
        "k3W2H1fUzmtXeb+bP0IGNZtKaGLVzPrThfnpfEFoZ7seie2ynohBFyHG0+ic9dmGtmD8H8L3rvI7r/lWRU5q1Sb9IPVjs0VQTL96oFrxC/pnv6eW" +
        "dYMkmzByVrswBn1/t74AAES4csZHWVmEAAHrD4B6AADw87mJscRn+wIAAAAABFla";

    private static void TestWorkspaceTimestampReconciliation()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-wsstamp-" + Guid.NewGuid().ToString("N"));
        try
        {
            var installed = Path.Combine(root, "Install", "BuildWorkspace");
            var staged = Path.Combine(root, "staging", "BuildWorkspace");
            Directory.CreateDirectory(Path.Combine(installed, "runtime", "src"));
            Directory.CreateDirectory(Path.Combine(staged, "runtime", "src"));
            File.WriteAllText(Path.Combine(installed, "runtime", "src", "same.cpp"), "int same();");
            File.WriteAllText(Path.Combine(staged, "runtime", "src", "same.cpp"), "int same();");
            var installedSameStamp = new DateTime(2026, 1, 2, 3, 4, 5, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(Path.Combine(installed, "runtime", "src", "same.cpp"),
                installedSameStamp);
            File.WriteAllText(Path.Combine(installed, "runtime", "src", "edited.cpp"), "int old();");
            File.WriteAllText(Path.Combine(staged, "runtime", "src", "edited.cpp"), "int new_();");
            File.WriteAllText(Path.Combine(staged, "runtime", "src", "added.cpp"), "int added();");
            foreach (var file in Directory.EnumerateFiles(staged, "*", SearchOption.AllDirectories))
                PayloadArchive.NormalizeExtractedTimestamp(file);

            WorkspaceTimestamps.MarkChangedFiles(installed, staged, _ => { });

            if (File.GetLastWriteTimeUtc(Path.Combine(staged, "runtime", "src", "same.cpp")) !=
                installedSameStamp)
                throw new Exception("An unchanged workspace file did not inherit the installed " +
                                    "timestamp; clang would reject the previous build's PCHs.");
            if (File.GetLastWriteTimeUtc(Path.Combine(staged, "runtime", "src", "edited.cpp")).Year == 2000)
                throw new Exception("A changed workspace file kept its normalized timestamp; " +
                                    "Ninja would never recompile it.");
            if (File.GetLastWriteTimeUtc(Path.Combine(staged, "runtime", "src", "added.cpp")).Year == 2000)
                throw new Exception("A new workspace file kept its normalized timestamp.");

            // A fresh installation has no workspace to compare against; the pass must be a no-op.
            WorkspaceTimestamps.MarkChangedFiles(Path.Combine(root, "absent"), staged, _ => { });
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }


    private static void TestToolkitRefreshDecision()
    {
        if (!InstallerEngine.MustRefreshToolkit(sameToolkit: false, samePackageContent: true,
                nodToolPresent: true))
            throw new Exception("A republished workspace kept the installed toolkit; the shipped " +
                                "translator and project file could come from different releases.");
        if (!InstallerEngine.MustRefreshToolkit(sameToolkit: true, samePackageContent: false,
                nodToolPresent: true))
            throw new Exception("Changed toolkit package content was not extracted.");
        if (!InstallerEngine.MustRefreshToolkit(sameToolkit: true, samePackageContent: true,
                nodToolPresent: false))
            throw new Exception("A missing nodtool.exe did not force toolkit extraction.");
        if (InstallerEngine.MustRefreshToolkit(sameToolkit: true, samePackageContent: true,
                nodToolPresent: true))
            throw new Exception("An unchanged toolkit was needlessly re-extracted.");
    }

    /// <summary>
    /// Every <c>[paths]</c> key follows the same contract: writing preserves other settings, reads
    /// back resolved, and uninstall removes it only while it still points at the recorded location.
    /// </summary>
    private static void TestPathSettingRoundTrip(string key, Action<string, string> set,
        Action<string, string> removeIfOwned)
    {
        var root = Path.Combine(Path.GetTempPath(), $"opuc1-{key}-" + Guid.NewGuid().ToString("N"));
        var config = Path.Combine(root, "Config.toml");
        var owned = Path.Combine(root, "Owned Location");
        try
        {
            Directory.CreateDirectory(owned);
            File.WriteAllText(config,
                "[video]\nwidescreen = true\n\n[paths]\nunrelated_root = \"D:\\\\Elsewhere\"\n");
            set(config, owned);
            var updated = File.ReadAllText(config);
            if (!updated.Contains("widescreen = true", StringComparison.Ordinal) ||
                !updated.Contains("unrelated_root = \"D:\\\\Elsewhere\"", StringComparison.Ordinal) ||
                !updated.Contains(key + " = ", StringComparison.Ordinal))
                throw new Exception($"Writing {key} discarded existing runtime settings.");

            var read = RuntimeConfiguration.GetResolvedPath(config, key);
            if (read is null || !read.Equals(Path.GetFullPath(owned), StringComparison.OrdinalIgnoreCase))
                throw new Exception($"The configured {key} did not round-trip.");

            removeIfOwned(config, Path.Combine(root, "Someone Else"));
            if (RuntimeConfiguration.GetPath(config, key) is null)
                throw new Exception($"A {key} owned by someone else was removed.");
            removeIfOwned(config, owned);
            if (RuntimeConfiguration.GetPath(config, key) is not null)
                throw new Exception($"Uninstall did not remove its owned {key} setting.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// TOML keys are case-sensitive and the runtime compares them exactly, so the host must match
    /// case too, or it silently writes a key the runtime never reads.
    /// </summary>
    private static void TestRuntimeConfigurationKeyMatching()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-tomlcase-" + Guid.NewGuid().ToString("N"));
        var config = Path.Combine(root, RuntimeConfiguration.ConfigFileName);
        var owned = Path.Combine(root, "Owned Location");
        try
        {
            Directory.CreateDirectory(owned);
            File.WriteAllText(config, "[Paths]\r\nDvd_Root = \"D:\\\\Wrong\"\r\n");
            RuntimeConfiguration.SetDvdRoot(config, owned);
            if (!File.ReadAllText(config).Contains("Dvd_Root = \"D:\\\\Wrong\"", StringComparison.Ordinal))
                throw new Exception("A differently-cased key the runtime ignores was rewritten.");
            var written = RuntimeConfiguration.GetResolvedPath(config, "dvd_root");
            if (written is null || !written.Equals(Path.GetFullPath(owned), StringComparison.OrdinalIgnoreCase))
                throw new Exception("dvd_root was not written as the exactly-cased key the runtime reads.");

            // An escaped quote does not end the value, so the '#' after it is data rather than the
            // start of a comment - exactly what RemoveComment in runtime_config.h decides.
            File.WriteAllText(config, "[paths]\r\ndvd_root = \"a\\\"#b\"\r\n");
            if (RuntimeConfiguration.GetPath(config, "dvd_root") != "a\"#b")
                throw new Exception("A '#' inside an escaped quoted value was treated as a comment.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// Quoted and bare spellings of a key or table are the same TOML key, so rewriting replaces the
    /// entry in place, keeps everything else byte-identical, and never touches an unparseable file.
    /// </summary>
    private static void TestRuntimeConfigurationQuotedKeys()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-tomlquoted-" + Guid.NewGuid().ToString("N"));
        var config = Path.Combine(root, RuntimeConfiguration.ConfigFileName);
        var owned = Path.Combine(root, "Owned Location");
        var fullOwned = Path.GetFullPath(owned);
        try
        {
            Directory.CreateDirectory(owned);
            foreach (var header in new[] { "[\"paths\"]", "[ 'paths' ]", "[paths]" })
            foreach (var key in new[] { "\"dvd_root\"", "'dvd_root'", "\"dvd\\u005Froot\"", "dvd_root" })
            {
                File.WriteAllText(config,
                    $"# top\r\n[video]\r\nwidescreen = true # keep\r\n\r\n{header}\r\n  {key}=\"D:\\\\Old\" # note\r\nother = 1\r\n");
                RuntimeConfiguration.SetDvdRoot(config, owned);
                var text = File.ReadAllText(config);
                var expected = $"# top\r\n[video]\r\nwidescreen = true # keep\r\n\r\n{header}\r\n  {key}=" +
                               $"\"{fullOwned.Replace("\\", "\\\\")}\" # note\r\nother = 1\r\n";
                if (text != expected)
                    throw new Exception($"{header} {key} was not replaced in place:\r\n{text}");
                if (RuntimeConfiguration.GetResolvedPath(config, "dvd_root") != fullOwned)
                    throw new Exception($"The replaced {key} could not be read back.");
            }

            File.WriteAllText(config, "[paths]\r\nother = 1\r\n\r\n# next\r\n[video]\r\nx = 2");
            RuntimeConfiguration.SetDvdRoot(config, owned);
            var appended = File.ReadAllText(config);
            if (!appended.StartsWith("[paths]\r\nother = 1\r\ndvd_root = ", StringComparison.Ordinal) ||
                !appended.EndsWith("\r\n\r\n# next\r\n[video]\r\nx = 2", StringComparison.Ordinal))
                throw new Exception("A missing key was not appended to its section:\r\n" + appended);

            File.WriteAllText(config, "dvd_root = \"D:\\\\Root\"\r\n[video]\r\nx = 2\r\n");
            RuntimeConfiguration.SetDvdRoot(config, owned);
            var created = File.ReadAllText(config);
            if (!created.StartsWith("dvd_root = \"D:\\\\Root\"\r\n[video]\r\nx = 2\r\n\r\n[paths]\r\n", StringComparison.Ordinal) ||
                RuntimeConfiguration.GetResolvedPath(config, "dvd_root") != fullOwned)
                throw new Exception("A missing section was not created without touching a same-named root key:\r\n" + created);

            File.Delete(config);
            RuntimeConfiguration.SetDvdRoot(config, owned);
            if (RuntimeConfiguration.GetResolvedPath(config, "dvd_root") != fullOwned)
                throw new Exception("A missing config file was not created.");

            const string broken = "[paths]\r\ndvd_root = 1\r\ndvd_root = 2\r\n";
            File.WriteAllText(config, broken);
            try
            {
                RuntimeConfiguration.SetDvdRoot(config, owned);
                throw new Exception("An unparseable config was rewritten.");
            }
            catch (TomlException) { }
            if (File.ReadAllText(config) != broken)
                throw new Exception("An unparseable config was modified.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// One location rule set covers both an installation directory and a portable root, so the
    /// rejections a portable install performs are the ones an ordinary install performs too.
    /// </summary>
    private static void TestInstallLocationValidation()
    {
        void Rejects(string description, string path)
        {
            try
            {
                FileSystemUtilities.EnsureUsableLocation(path, "The installation folder");
                throw new Exception(description + " was accepted as an install location.");
            }
            catch (InvalidOperationException) { }
        }

        FileSystemUtilities.EnsureUsableLocation("C:\\Games\\OpucRecomp\\Install", "The installation folder");
        Rejects("A network path", "\\\\server\\share\\OpucRecomp");
        Rejects("An over-long path", "C:\\" + new string('w', 200));
        Rejects("A drive root", "C:\\");
        Rejects("A folder inside Windows",
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "OpucRecomp"));
        Rejects("The user profile itself",
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        // The two entry points must not have separate opinions about any of the above.
        foreach (var rejected in new[] { "\\\\server\\share\\OpucRecomp", "C:\\" })
        {
            try
            {
                PortableRoot.EnsureUsableRoot(rejected);
                throw new Exception($"{rejected} was accepted as a portable root.");
            }
            catch (InvalidOperationException) { }
        }
    }

    private static void TestCommandLineContract()
    {
        void Rejects(string description, params string[] args)
        {
            try
            {
                CommandLine.Parse(args);
                throw new Exception(description + " was accepted.");
            }
            catch (ArgumentException) { }
        }

        var version = CommandLine.Parse(["--version"]);
        if (version.Mode != AppMode.Version) throw new Exception("--version was not recognised.");
        if (CommandLine.Parse([]).Mode != AppMode.Guided)
            throw new Exception("No arguments did not select the guided setup.");
        if (CommandLine.Parse(["--help"]).Mode != AppMode.Help) throw new Exception("--help was not recognised.");

        var silent = CommandLine.Parse([
            "--silent", "--game", "game.iso", "--install-dir", "C:\\Games\\OPUC", "--sysconf", "D:\\SYSCONF",
            "--progress-json"
        ]);
        if (silent.Mode != AppMode.SilentInstall || !silent.ProgressJson || silent.GamePath != "game.iso" ||
            silent.SysconfPath != "D:\\SYSCONF" || silent.InstallDirectory != "C:\\Games\\OPUC")
            throw new Exception("The silent install contract did not parse.");
        if (CommandLine.Parse(["--silent", "--game", "game.iso"]).InstallDirectory != ProductInfo.DefaultInstallDirectory)
            throw new Exception("A silent install without --install-dir did not use the default folder.");

        var launch = CommandLine.Parse(["--launch"]);
        if (launch.Mode != AppMode.Launch || launch.InstallDirectory is not null)
            throw new Exception("The launch contract did not parse.");
        if (CommandLine.Parse(["--launch", "--install-dir", "C:\\Games\\OPUC"]).InstallDirectory != "C:\\Games\\OPUC")
            throw new Exception("--launch did not take an install folder.");
        Rejects("runtime argument forwarding", "--launch", "--", "--windowed");

        var check = CommandLine.Parse(["--check-products", "--install-dir", "C:\\Games\\OPUC", "--progress-json"]);
        if (check.Mode != AppMode.CheckProducts || !check.ProgressJson)
            throw new Exception("The product check contract did not parse.");

        var repair = CommandLine.Parse([
            "--repair-products", "--install-dir", "C:\\Games\\OPUC", "--sysconf", "S", "--progress-json"
        ]);
        if (repair.Mode != AppMode.RepairProducts || repair.SysconfPath != "S")
            throw new Exception("The repair contract did not parse.");

        Rejects("Repair without an installation", "--repair-products", "--progress-json");
        Rejects("--progress-json outside an install", "--launch", "--progress-json");
        Rejects("--sysconf with a product check", "--check-products", "--sysconf", "S");
        Rejects("--game with a launch", "--launch", "--game", "g.iso");
        Rejects("An install without a disc image", "--silent");
        Rejects("A value-less option", "--silent", "--game");
        Rejects("An unknown option", "--silent", "--game", "g.iso", "--unknown-option", "b");
        Rejects("A per-profile launch verb", "--launch-base");

        var portable = CommandLine.Parse([
            "--silent", "--game", "game.iso", "--install-dir", "D:\\Games\\Recomp\\Install",
            "--portable", "--progress-json"
        ]);
        if (portable.Mode != AppMode.SilentInstall || !portable.Portable)
            throw new Exception("The portable install contract did not parse.");

        Rejects("--portable without an install directory", "--silent", "--game", "g.iso", "--portable");
        Rejects("--portable outside an install", "--check-products", "--install-dir", "fixed", "--portable");
        Rejects("A bare setting assignment", "--check-products", "video.show_fps=true");
    }


    /// <summary>
    /// The marker walk-up is the one thing both languages must agree on. The depth bound keeps an
    /// unrelated marker high up a drive from capturing an ordinary installation.
    /// </summary>
    private static void TestPortableRootDiscovery()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-portable-" + Guid.NewGuid().ToString("N"));
        try
        {
            var install = Path.Combine(root, "Install");
            var deep = Path.Combine(install, "Base", "config", "a", "b", "c");
            Directory.CreateDirectory(deep);

            if (PortableRoot.TryFind(install) is not null)
                throw new Exception("A directory without the marker was treated as portable.");

            File.WriteAllText(Path.Combine(root, PortableRoot.MarkerFileName), "portable");
            if (PortableRoot.TryFind(root) != root)
                throw new Exception("The marker directory itself was not recognised as the root.");
            if (PortableRoot.TryFind(install) != root)
                throw new Exception("The install directory did not resolve to its portable root.");
            if (PortableRoot.TryFind(Path.Combine(install, "Base", "config", "a")) != root)
                throw new Exception("A path within the depth bound did not resolve to its portable root.");
            if (PortableRoot.TryFind(deep) is not null)
                throw new Exception("The portable search walked further up than its depth bound.");

            if (!PortableRoot.Contains(root, Path.Combine(install, "GameAssets", "DATA")) ||
                PortableRoot.Contains(root, Path.Combine(Path.GetTempPath(), "elsewhere")))
                throw new Exception("Portable containment did not classify paths correctly.");

            try
            {
                PortableRoot.EnsureUsableRoot(Path.GetPathRoot(root)!);
                throw new Exception("A drive root was accepted as a portable root.");
            }
            catch (InvalidOperationException) { }
            try
            {
                PortableRoot.EnsureUsableRoot(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
                throw new Exception("A well-known system folder was accepted as a portable root.");
            }
            catch (InvalidOperationException) { }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A rejected game image must not turn the destination's parent into a portable root. Otherwise
    /// the marker walk-up would redirect unrelated later installs into this root's UserData.
    /// </summary>
    private static void TestPortableValidationFailureLeavesNoMarker()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-portable-rejected-" + Guid.NewGuid().ToString("N"));
        var game = Path.Combine(root, "not-a-disc-image.wad");
        try
        {
            Directory.CreateDirectory(root);
            File.WriteAllBytes(game, [0]);
            var options = new InstallOptions
            {
                GamePath = game,
                InstallDirectory = Path.Combine(root, PortableRoot.InstallDirectoryName),
                Portable = true
            };

            try
            {
                new InstallerEngine(new DelegatingInstallReporter(_ => { }))
                    .InstallAsync(options).GetAwaiter().GetResult();
                throw new Exception("An unsupported game image was accepted.");
            }
            catch (InvalidDataException) { }

            if (File.Exists(Path.Combine(root, PortableRoot.MarkerFileName)))
                throw new Exception("A failed portable install left its marker behind.");
            if (Directory.Exists(PortableRoot.UserDataDirectory(root)))
                throw new Exception("A failed portable install left its UserData directory behind.");
            if (PortableRoot.TryFind(options.InstallDirectory) is not null)
                throw new Exception("A failed portable install captured later installs under its parent.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// Shortcuts are written through source-generated COM (Native AOT). A vtable slot declared out of
    /// order would store the wrong field or crash, so a written .lnk is read back.
    /// </summary>
    private static void TestShortcutRoundTrip()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-shortcut-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(root);
            var target = Path.Combine(root, "Install Dir", ProductInfo.SetupCopyName);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.WriteAllText(target, "");
            var shortcut = Path.Combine(root, "game.lnk");

            ShellIntegration.CreateShortcut(shortcut, target, "--launch", Path.GetDirectoryName(target)!, target, "Play");
            var (storedTarget, storedArguments) = ShellIntegration.ReadShortcut(shortcut);
            if (!string.Equals(storedTarget, target, StringComparison.OrdinalIgnoreCase))
                throw new Exception($"The shortcut targets '{storedTarget}', expected '{target}'.");
            if (storedArguments != "--launch")
                throw new Exception($"The shortcut passes '{storedArguments}', expected '--launch'.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestShortcutCleanupIsBestEffort()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-shortcuts-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(root);
            // File.Delete cannot delete a directory. This deterministically forces the first cleanup
            // to fail while the remaining independently guarded targets must still be attempted.
            var firstShortcut = Path.Combine(root, "first.lnk");
            Directory.CreateDirectory(firstShortcut);
            var secondShortcut = Path.Combine(root, "second.lnk");
            File.WriteAllText(secondShortcut, "shortcut");

            try
            {
                ShellIntegration.RemoveShortcuts([firstShortcut, secondShortcut]);
                throw new Exception("A shortcut deletion failure was not reported.");
            }
            catch (AggregateException) { }

            if (File.Exists(secondShortcut))
                throw new Exception("One failed shortcut deletion prevented the next file deletion.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The configuration location is a property of the installation, not of this process: an install
    /// under a portable root uses that root's UserData, everything else the shared per-user file.
    /// </summary>
    private static void TestPortableConfigPathResolution()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-portable-config-" + Guid.NewGuid().ToString("N"));
        try
        {
            var install = Path.Combine(root, "Install");
            Directory.CreateDirectory(install);
            if (!RuntimeConfiguration.ResolveConfigPath(install)
                    .Equals(RuntimeConfiguration.ApplicationDataConfigPath, StringComparison.OrdinalIgnoreCase))
                throw new Exception("A non-portable installation did not use per-user application data.");

            var created = PortableRoot.Create(root);
            if (!Directory.Exists(PortableRoot.UserDataDirectory(created)) ||
                !File.Exists(Path.Combine(created, PortableRoot.MarkerFileName)))
                throw new Exception("Creating a portable root did not produce its marker and user data.");

            var expected = Path.Combine(root, PortableRoot.UserDataDirectoryName, "Config.toml");
            if (!RuntimeConfiguration.ResolveConfigPath(install)
                    .Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new Exception("A portable installation did not use its own UserData configuration.");
            if (!new Installation(install).ConfigPath.Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new Exception("The installation did not report its portable configuration path.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A portable installation stores every path inside its root relative to the configuration
    /// directory, so the whole tree survives being moved. Ownership decisions must therefore compare
    /// resolved paths, not stored text.
    /// </summary>
    private static void TestPortableRelativePathSettings()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-portable-paths-" + Guid.NewGuid().ToString("N"));
        try
        {
            PortableRoot.Create(root);
            var install = Path.Combine(root, "Install");
            var config = Path.Combine(root, PortableRoot.UserDataDirectoryName, "Config.toml");
            var dvdRoot = Path.Combine(install, "GameAssets", "DATA");
            var outsideDvd = Path.Combine(Path.GetTempPath(), "opuc1-outside-" + Guid.NewGuid().ToString("N"));

            RuntimeConfiguration.SetDvdRoot(config, dvdRoot);
            var stored = RuntimeConfiguration.GetPath(config, "dvd_root");
            if (stored is null || Path.IsPathRooted(stored))
                throw new Exception("A path inside the portable root was stored absolute.");
            if (stored.Contains('\\', StringComparison.Ordinal))
                throw new Exception("A relative path was not stored with the canonical forward slashes.");
            if (!RuntimeConfiguration.GetResolvedPath(config, "dvd_root")!
                    .Equals(Path.GetFullPath(dvdRoot), StringComparison.OrdinalIgnoreCase))
                throw new Exception("A relative dvd_root did not resolve against the configuration directory.");

            RuntimeConfiguration.SetDvdRoot(config, outsideDvd);
            if (!Path.IsPathRooted(RuntimeConfiguration.GetPath(config, "dvd_root")!))
                throw new Exception("A dvd_root outside the portable root was stored relative.");
            RuntimeConfiguration.SetDvdRoot(config, dvdRoot);

            RuntimeConfiguration.RemoveDvdRootIfOwned(config, Path.Combine(install, "Elsewhere"));
            if (RuntimeConfiguration.GetPath(config, "dvd_root") is null)
                throw new Exception("A relative dvd_root owned by someone else was removed.");
            RuntimeConfiguration.RemoveDvdRootIfOwned(config, dvdRoot);
            if (RuntimeConfiguration.GetPath(config, "dvd_root") is not null)
                throw new Exception("A relative dvd_root this installation owns was not removed.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A portable root can be moved. Healing adopts the directory the installation was actually found
    /// in and discards the location-bound native build cache - and must never touch a non-portable
    /// installation, where the same disagreement is a real problem to report.
    /// </summary>
    private static void TestPortableMoveHealing()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-move-" + Guid.NewGuid().ToString("N"));
        try
        {
            var install = Path.Combine(root, "Install");
            var nativeBuild = Path.Combine(install, "BuildWorkspace", "native-build");
            Directory.CreateDirectory(nativeBuild);
            File.WriteAllText(Path.Combine(nativeBuild, "CMakeCache.txt"),
                "CMAKE_HOME_DIRECTORY:INTERNAL=D:/Old/Install/BuildWorkspace/runtime");
            var statePath = Path.Combine(install, "install-state.json");
            JsonState.Write(statePath, new InstallState { InstallDir = "D:\\Old\\Install" }, StateJsonContext.Default);

            var plain = new Installation(install);
            if (PortableInstallHealing.HealMovedInstall(plain))
                throw new Exception("A non-portable installation was healed.");
            if (!Directory.Exists(nativeBuild))
                throw new Exception("A non-portable installation lost its native build directory.");

            PortableRoot.Create(root);
            var portable = new Installation(install);
            if (!PortableInstallHealing.HealMovedInstall(portable))
                throw new Exception("A moved portable installation was not healed.");
            var healed = portable.ReadInstallState();
            if (healed is null || !healed.InstallDir.Equals(portable.Root, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Healing did not adopt the actual installation directory.");
            if (Directory.Exists(nativeBuild))
                throw new Exception("Healing kept the location-bound native build cache.");

            if (PortableInstallHealing.HealMovedInstall(portable))
                throw new Exception("An installation already at its recorded location was healed again.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestInstallMoveTransientLockPolicy()
    {
        if (!InstallTransaction.IsTransientLock(new IOException("in use", unchecked((int)0x80070020))))
            throw new Exception("A sharing violation would not be retried.");
        if (!InstallTransaction.IsTransientLock(new IOException("locked", unchecked((int)0x80070021))))
            throw new Exception("A lock violation would not be retried.");
        if (!InstallTransaction.IsTransientLock(new UnauthorizedAccessException("denied")))
            throw new Exception("A transient access-denied would not be retried.");
        if (InstallTransaction.IsTransientLock(new IOException("disk full", unchecked((int)0x80070070))))
            throw new Exception("A permanent I/O failure would be retried.");
        if (InstallTransaction.IsTransientLock(new InvalidDataException("bad")))
            throw new Exception("A non-I/O failure would be retried.");
    }

    private static void TestHostCpuRequirement()
    {
        // Leaf 1 ecx, leaf 7 ebx, leaf 0x80000001 ecx of a v2-only and a v3 processor.
        const uint v2Leaf1 = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23);
        const uint v3Leaf1 = v2Leaf1 | (1u << 12) | (1u << 22) | (1u << 27) | (1u << 28) | (1u << 29);
        const uint v3Leaf7 = (1u << 3) | (1u << 5) | (1u << 8);
        Func<uint, uint[]> Cpu(uint maxBasic, uint leaf1, uint leaf7, uint extended) => leaf => leaf switch
        {
            0 => [maxBasic, 0, 0, 0],
            1 => [0, 0, leaf1, 0],
            7 => [0, leaf7, 0, 0],
            0x80000000u => [0x80000001u, 0, 0, 0],
            0x80000001u => [0, 0, extended, 0],
            _ => [0, 0, 0, 0]
        };
        var nehalem = Cpu(11, v2Leaf1, 0, 1u);
        var haswell = Cpu(13, v3Leaf1, v3Leaf7, 1u | (1u << 5));

        void Expect(IReadOnlyList<string> actual, string expected, string what)
        {
            if (string.Join(", ", actual) != expected)
                throw new Exception($"{what}: expected [{expected}], got [{string.Join(", ", actual)}].");
        }

        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V2, nehalem, false), "",
            "A v2 processor was rejected for the v2 build");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, nehalem, false),
            "FMA, MOVBE, OSXSAVE, AVX, F16C, BMI1, AVX2, BMI2, LZCNT", "A v2 processor for the v3 build");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, haswell, true), "",
            "A v3 processor was rejected");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V2, haswell, false), "",
            "A v3 processor was rejected for the v2 build");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, haswell, false),
            "operating system support for AVX register state (XCR0 YMM bits)", "Disabled AVX state");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, Cpu(6, v3Leaf1, v3Leaf7, 1u | (1u << 5)), true),
            "BMI1, AVX2, BMI2", "Unavailable leaf 7");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V2, Cpu(11, v2Leaf1, 0, 0), false),
            "LAHF-SAHF", "Missing LAHF/SAHF");

        if (HostCpuRequirement.SelectBuildProfile(nehalem, false) != X86CpuProfile.V2)
            throw new Exception("A v2 processor was not given the v2 build.");
        if (HostCpuRequirement.SelectBuildProfile(haswell, true) != X86CpuProfile.V3)
            throw new Exception("A v3 processor was not given the v3 build.");
        if (HostCpuRequirement.SelectBuildProfile(haswell, false) != X86CpuProfile.V2)
            throw new Exception("A v3 processor without AVX state was given the v3 build.");

        var message = HostCpuRequirement.Describe(X86CpuProfile.V3, ["AVX2"]);
        if (!message.Contains("x86-64-v3") || !message.Contains("Missing on this machine: AVX2"))
            throw new Exception("The unsupported-processor message lost its detail.");
    }

    private static void TestInstallTransactionRollback()
    {
        var parent = Path.Combine(Path.GetTempPath(),
            "opuc1-transaction-self-test-" + Guid.NewGuid().ToString("N"));
        var installRoot = Path.Combine(parent, "install");
        var liveComponent = Path.Combine(installRoot, "Component");
        var preparedComponent = Path.Combine(parent, "prepared-component");
        var preparedFile = Path.Combine(parent, "prepared-added.json");
        var addedFile = Path.Combine(installRoot, "added.json");
        try
        {
            Directory.CreateDirectory(Path.Combine(liveComponent, "nested"));
            File.WriteAllText(Path.Combine(liveComponent, "nested", "original.txt"), "original-bytes");
            File.WriteAllText(Path.Combine(installRoot, "untouched.txt"), "untouched-bytes");
            Directory.CreateDirectory(preparedComponent);
            File.WriteAllText(Path.Combine(preparedComponent, "replacement.txt"), "replacement-bytes");
            File.WriteAllText(preparedFile, "new-live-file");

            using (var transaction = InstallTransaction.Begin(installRoot, null,
                       InstallTransactionEntry.Directory(preparedComponent, liveComponent),
                       InstallTransactionEntry.File(preparedFile, addedFile)))
            {
                transaction.Publish();
                if (!File.Exists(Path.Combine(liveComponent, "replacement.txt")) ||
                    File.Exists(Path.Combine(liveComponent, "nested", "original.txt")) ||
                    File.ReadAllText(addedFile) != "new-live-file")
                    throw new Exception("The transaction did not publish its prepared replacements.");
                // Deliberately omit Commit. Dispose must restore the exact pre-transaction tree.
            }

            var original = Path.Combine(liveComponent, "nested", "original.txt");
            if (!File.Exists(original) || File.ReadAllText(original) != "original-bytes" ||
                File.Exists(Path.Combine(liveComponent, "replacement.txt")) || File.Exists(addedFile) ||
                Directory.Exists(preparedComponent) || File.Exists(preparedFile) ||
                File.ReadAllText(Path.Combine(installRoot, "untouched.txt")) != "untouched-bytes")
                throw new Exception("Disposing an uncommitted transaction did not restore the exact original tree.");
            if (File.Exists(InstallOperationPaths.GetJournalPath(installRoot)))
                throw new Exception("A rolled-back transaction left its journal behind.");
        }
        finally
        {
            if (Directory.Exists(parent)) Directory.Delete(parent, recursive: true);
        }
    }

    private static void TestInstallScratchRecovery()
    {
        var parent = Path.Combine(Path.GetTempPath(),
            "opuc1-scratch-self-test-" + Guid.NewGuid().ToString("N"));
        var installRoot = Path.Combine(parent, "install");
        var otherInstallRoot = Path.Combine(parent, "other-install");
        InstallScratchSpace? scratch = null;
        InstallScratchSpace? otherScratch = null;
        try
        {
            Directory.CreateDirectory(installRoot);
            Directory.CreateDirectory(otherInstallRoot);
            scratch = InstallScratchSpace.CreateSibling(installRoot);
            File.WriteAllText(Path.Combine(scratch.Root, "owned.txt"), "owned");

            // Scratch is keyed by the installation's scope id, so a neighbouring installation's
            // scratch lives beside it under a different prefix and must survive this recovery.
            otherScratch = InstallScratchSpace.CreateSibling(otherInstallRoot);
            var sentinel = Path.Combine(otherScratch.Root, "preserve.txt");
            File.WriteAllText(sentinel, "preserve");

            using (InstallOperationLock.Acquire(installRoot))
            {
                if (Directory.Exists(scratch.Root))
                    throw new Exception("Lock acquisition did not recover this installation's scratch space.");
                if (!File.Exists(sentinel) || File.ReadAllText(sentinel) != "preserve")
                    throw new Exception("Scratch recovery removed another installation's scratch space.");
            }
        }
        finally
        {
            scratch?.Dispose();
            otherScratch?.Dispose();
            if (Directory.Exists(parent)) Directory.Delete(parent, recursive: true);
        }
    }

    private static void TestToolkitFingerprint()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-toolkit-" + Guid.NewGuid().ToString("N"));
        try
        {
            CreateFakeToolkit(root);
            var first = ToolkitFingerprint.Compute(root);
            if (first != ToolkitFingerprint.Compute(root))
                throw new Exception("The toolkit fingerprint is not stable.");

            // A file that has nothing to do with generated code must not invalidate every install.
            File.WriteAllText(Path.Combine(root, "Toolkit", "nodtool.exe"), "irrelevant");
            if (ToolkitFingerprint.Compute(root) != first)
                throw new Exception("An unrelated toolkit file changed the fingerprint.");

            File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"), "translator-v2");
            var afterTranslator = ToolkitFingerprint.Compute(root);
            if (afterTranslator == first)
                throw new Exception("A new translator did not change the fingerprint.");

            File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp"), "int main2();");
            var afterRuntimeSource = ToolkitFingerprint.Compute(root);
            if (afterRuntimeSource == afterTranslator)
                throw new Exception("A runtime source change did not change the fingerprint.");

            var patch = Path.Combine(root, "BuildWorkspace", "third_party", "aurora", "cmake", "patches",
                "sdl3.patch");
            Directory.CreateDirectory(Path.GetDirectoryName(patch)!);
            File.WriteAllText(patch, "patch-v1");
            var afterPatch = ToolkitFingerprint.Compute(root);
            if (afterPatch == afterRuntimeSource)
                throw new Exception("An Aurora patch consumed by the provider did not change compile identity.");

            var installation = new Installation(root);
            installation.WriteToolkitState(afterPatch, ToolkitFingerprint.ComputePackage(root),
                "opuc1-recomp-v1", ToolkitFingerprint.ComputeRuntimeAssets(root));
            var repair = new ProductRepairService(installation, new DelegatingInstallReporter(_ => { }));
            repair.VerifyToolkitForCompilation(afterPatch);

            void ExpectModifiedToolkitRejected(string description)
            {
                try
                {
                    repair.VerifyToolkitForCompilation(afterPatch);
                    throw new Exception(description);
                }
                catch (InvalidDataException) { }
            }

            File.WriteAllText(patch, "patch-v2");
            ExpectModifiedToolkitRejected("A modified mandatory Aurora patch was accepted for compilation.");
            File.Delete(patch);
            ExpectModifiedToolkitRejected("A deleted mandatory Aurora patch was accepted for compilation.");
            File.WriteAllText(patch, "patch-v1");
            repair.VerifyToolkitForCompilation(afterPatch);

            var runtimeAssets = ToolkitFingerprint.ComputeRuntimeAssets(root);
            var dspRom = Path.Combine(root, "BuildWorkspace", "runtime", "assets", "dsp", "dsp_coef.bin");
            File.WriteAllText(dspRom, "dsp-v2");
            if (ToolkitFingerprint.Compute(root) != afterPatch)
                throw new Exception("A copied-only runtime asset needlessly changed compile identity.");
            var changedRuntimeAssets = ToolkitFingerprint.ComputeRuntimeAssets(root);
            if (changedRuntimeAssets == runtimeAssets)
                throw new Exception("A copied runtime asset did not change its publication identity.");

            // Cache-reuse subsets: translator or native registration changes invalidate translation
            // but not toolchain, since CMake/Ninja rebuild exactly the affected objects; a compiler
            // change invalidates only toolchain; a native_prebuilt provenance rewrite invalidates
            // neither. Without a current native index, runtime/src itself decides translation.
            var components = ToolkitFingerprint.ComputeComponents(root);
            if (components.Compile != afterPatch)
                throw new Exception("ComputeComponents disagrees with the compile identity.");
            File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"),
                "translator-v3");
            var afterNewTranslator = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewTranslator.Translation == components.Translation)
                throw new Exception("A new translator did not change the translation identity.");
            if (afterNewTranslator.NativeToolchain != components.NativeToolchain)
                throw new Exception("A new translator needlessly changed the native toolchain identity.");
            var runtimeMain = Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp");
            File.WriteAllText(runtimeMain, "int main3();");
            var afterUnindexedRuntime = ToolkitFingerprint.ComputeComponents(root);
            if (afterUnindexedRuntime.Translation == afterNewTranslator.Translation)
                throw new Exception("A runtime source change without a native index did not change the translation identity.");

            File.WriteAllText(runtimeMain, "int main();");
            if (WriteFakeNativeIndex(root, "\"a\"") !=
                "50b9779591f4b205751361c8899accd362f96ea5dfd32febed1bedca534c523a")
                throw new Exception("The native index source digest no longer matches the translator's.");
            var indexed = ToolkitFingerprint.ComputeComponents(root);
            File.WriteAllText(runtimeMain, "int main3();");
            WriteFakeNativeIndex(root, "\"a\"");
            var afterNewRuntime = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewRuntime.Translation != indexed.Translation)
                throw new Exception("A runtime source change with unchanged registrations changed the translation identity.");
            if (afterNewRuntime.NativeToolchain != indexed.NativeToolchain)
                throw new Exception("A runtime source change needlessly changed the native toolchain identity.");
            if (afterNewRuntime.Compile == indexed.Compile)
                throw new Exception("A runtime source change did not change the compile identity.");
            File.WriteAllText(runtimeMain, "int main4();");
            if (ToolkitFingerprint.ComputeComponents(root).Translation == afterNewRuntime.Translation)
                throw new Exception("A stale native index was trusted for the translation identity.");
            File.WriteAllText(runtimeMain, "int main3();");
            WriteFakeNativeIndex(root, "\"a\", \"b\"");
            afterNewRuntime = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewRuntime.Translation == indexed.Translation)
                throw new Exception("A native registration change did not change the translation identity.");
            if (afterNewRuntime.NativeToolchain != indexed.NativeToolchain)
                throw new Exception("A native registration change needlessly changed the native toolchain identity.");
            File.WriteAllText(Path.Combine(root, "Toolkit", "llvm-mingw", "bin", "clang-22.exe"),
                "clang-v2");
            var afterNewCompiler = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewCompiler.NativeToolchain == afterNewRuntime.NativeToolchain)
                throw new Exception("A new compiler did not change the native toolchain identity.");
            if (afterNewCompiler.Translation != afterNewRuntime.Translation)
                throw new Exception("A new compiler needlessly changed the translation identity.");
            File.WriteAllText(Path.Combine(root, "BuildWorkspace", "Dependencies", "native_prebuilt",
                "provenance.json"), "{\"BuiltUtc\":\"rewritten\"}");
            if (ToolkitFingerprint.Compute(root) != afterNewCompiler.Compile)
                throw new Exception(
                    "A native_prebuilt provenance rewrite needlessly changed the compile identity.");

            var product = Path.Combine(root, "product");
            Directory.CreateDirectory(product);
            File.Copy(dspRom, Path.Combine(product, "dsp_coef.bin"));
            if (!ToolkitFingerprint.ProductRuntimeAssetsMatch(product, changedRuntimeAssets))
                throw new Exception("Copied product assets did not match their source identity.");
            File.WriteAllText(Path.Combine(product, "dsp_coef.bin"), "stale");
            if (ToolkitFingerprint.ProductRuntimeAssetsMatch(product, changedRuntimeAssets))
                throw new Exception("An altered copied runtime asset was not detected.");
            File.Delete(Path.Combine(product, "dsp_coef.bin"));
            if (ToolkitFingerprint.ProductRuntimeAssetsMatch(product, changedRuntimeAssets))
                throw new Exception("A missing copied runtime asset was not detected.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>Writes a native index current for the fake runtime/src; returns its source digest.</summary>
    private static string WriteFakeNativeIndex(string root, string registrations)
    {
        var source = Path.Combine(root, "BuildWorkspace", "runtime", "src");
        var manifest = new System.Text.StringBuilder();
        foreach (var (relative, path) in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)
                     .Where(path => path.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) ||
                                    path.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
                                    path.EndsWith(".hpp", StringComparison.OrdinalIgnoreCase))
                     .Select(path => (Path.GetRelativePath(source, path).Replace('\\', '/'), path))
                     .OrderBy(item => item.Item1, StringComparer.Ordinal))
            manifest.Append(InputValidation.Sha256File(path)).Append("  ").Append(relative).Append('\n');
        var digest = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(manifest.ToString())))
            .ToLowerInvariant();
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "native-index.json"),
            "{\n  \"format\": \"opuc1-runtime-native-index\",\n  \"sourceDigest\": \"" + digest +
            "\",\n  \"registrations\": [" + registrations + "]\n}\n");
        return digest;
    }

    private static void CreateFakeToolkit(string root)
    {
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "Translator"));
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "CMake", "bin"));
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "Ninja"));
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "llvm-mingw", "bin"));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "projects", "opuc1"));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "runtime", "src"));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "dsp"));
        var aurora = Path.Combine(root, "BuildWorkspace", "third_party", "aurora");
        Directory.CreateDirectory(aurora);
        File.WriteAllText(Path.Combine(aurora, "README.md"), "aurora source");
        foreach (var dependency in InstalledLayout.DependencyNames)
        {
            var directory = Path.Combine(root, "BuildWorkspace", "Dependencies", dependency);
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, ".opuc1-layout"), dependency);
        }
        File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"), "translator-v1");
        File.WriteAllText(Path.Combine(root, "Toolkit", "CMake", "bin", "cmake.exe"), "cmake");
        File.WriteAllText(Path.Combine(root, "Toolkit", "Ninja", "ninja.exe"), "ninja");
        foreach (var executable in new[]
                 {
                     "clang-22.exe", "ld.lld.exe", "x86_64-w64-mingw32-clang.exe",
                     "x86_64-w64-mingw32-clang++.exe", "x86_64-w64-mingw32-windres.exe"
                 })
            File.WriteAllText(Path.Combine(root, "Toolkit", "llvm-mingw", "bin", executable), executable);
        File.WriteAllText(Path.Combine(root, "Toolkit", "nodtool.exe"), "tool");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "LocalBuild.ps1"), "# build");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "NativeBuildFlags.ps1"), "# flags");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", InstalledLayout.ProjectRelativePath),
            "project: {}");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "CMakeLists.txt"), "cmake_minimum_required(VERSION 3.20)");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp"), "int main();");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "dsp", "dsp_coef.bin"),
            "dsp");
    }

    private static void CopyFakeRuntimeAssets(string root, string productDirectory)
    {
        var source = Path.Combine(root, "BuildWorkspace", "runtime", "assets");
        File.Copy(Path.Combine(source, "dsp", "dsp_coef.bin"),
            Path.Combine(productDirectory, "dsp_coef.bin"), overwrite: true);
    }

    private static bool TryCreateFileLink(string linkPath, string targetPath)
    {
        try
        {
            File.CreateSymbolicLink(linkPath, targetPath);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or NotSupportedException)
        {
            return false;
        }
    }

    private static bool TryCreateDirectoryLink(string linkPath, string targetPath)
    {
        try
        {
            Directory.CreateSymbolicLink(linkPath, targetPath);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or NotSupportedException)
        {
            // Developer Mode / symlink permission is not guaranteed on a user's Windows system.
            // The identity check is still covered whenever the platform can create a link.
            return false;
        }
    }


    /// <summary>
    /// Callers match the products line's keys case-sensitively and branch on the typed status alone, so the exact
    /// shape is pinned here rather than left to a serializer default.
    /// </summary>
    private static void TestProductsReport()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-products-" + Guid.NewGuid().ToString("N"));
        try
        {
            // A path nothing was installed to must answer "absent", not fail and not ask for a rebuild.
            var missing = ConsoleCommands.InspectProducts(new Installation(root));
            if (missing.RebuildRequired || missing.Game.Reason != "absent")
                throw new Exception("A missing installation directory was not reported as absent.");
            if (Directory.Exists(root))
                throw new Exception("Inspecting a missing installation created the directory.");

            Directory.CreateDirectory(root);
            var empty = ConsoleCommands.InspectProducts(new Installation(root));
            if (empty.RebuildRequired || empty.Game.Reason != "absent")
                throw new Exception("An empty installation directory was not reported as absent.");
            if (File.Exists(Path.Combine(root, ToolkitState.FileName)))
                throw new Exception("Inspecting an empty installation wrote installation state.");

            var json = ConsoleCommands.SerializeProductsReport(empty);
            foreach (var key in new[]
                     {
                         "\"type\":\"products\"", "\"setupVersion\":", "\"installDir\":",
                         "\"rebuildRequired\":false", "\"game\":{\"status\":\"absent\",\"detail\":"
                     })
                if (!json.Contains(key, StringComparison.Ordinal))
                    throw new Exception($"The products line is missing {key}: {json}");
            foreach (var removed in new[] { "\"base\"", "\"health\"", "\"action\"" })
                if (json.Contains(removed, StringComparison.Ordinal))
                    throw new Exception($"The products line still carries {removed}: {json}");
            foreach (var wrongCase in new[] { "\"Game\"", "\"SetupVersion\"", "\"InstallDir\"",
                         "\"RebuildRequired\"", "\"Status\"", "\"Detail\"" })
                if (json.Contains(wrongCase, StringComparison.Ordinal))
                    throw new Exception($"The products line used PascalCase for {wrongCase}: {json}");

            var declared = new[] { "absent", "current", "toolkit-changed", "blocked", "broken" };
            foreach (var status in Enum.GetValues<ProductStatus>())
            {
                var reason = new ProductState(status, "").Reason;
                if (!declared.Contains(reason, StringComparer.Ordinal))
                    throw new Exception($"{status} reports the undeclared status {reason}.");
            }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The launch gate: a complete installation is current, and every way its provenance chain can break is reported
    /// as something other than current.
    /// </summary>
    private static void TestGameStateMachine()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-state-" + Guid.NewGuid().ToString("N"));
        try
        {
            CreateFakeToolkit(root);
            var installation = new Installation(root);
            WriteFakeExtractedDisc(installation.GameDataDirectory);
            var dolSha = InputValidation.Sha256File(Path.Combine(installation.GameDataDirectory, "sys", "main.dol"));
            var toolkit = ToolkitFingerprint.Compute(root);
            installation.WriteToolkitState(toolkit, ToolkitFingerprint.ComputePackage(root), "test",
                ToolkitFingerprint.ComputeRuntimeAssets(root));
            JsonState.Write(installation.InstallStatePath, new InstallState
            {
                InstallDir = root, GameId = ProductNames.GameId, DolSha256 = dolSha
            }, StateJsonContext.Default);

            Directory.CreateDirectory(installation.GameDirectory);
            File.WriteAllBytes(installation.GameExecutable, new byte[128 * 1024]);
            CopyFakeRuntimeAssets(root, installation.GameDirectory);
            JsonState.Write(Path.Combine(installation.GameDirectory, LocalBuildProvenance.FileName),
                new LocalBuildProvenance { SchemaVersion = 1, Profile = Installation.GameProfile, DolSha256 = dolSha }, StateJsonContext.Default);
            LocalBuildService.WriteFingerprint(installation.GameDirectory, toolkit, dolSha);

            void Expect(ProductStatus expected, string description)
            {
                var state = installation.CheckGame(installation.ResolveToolkitFingerprint());
                if (state.Status != expected)
                    throw new Exception($"{description}: expected {expected}, got {state.Status} ({state.Detail}).");
            }

            Expect(ProductStatus.Current, "A complete installation");
            if (!installation.ProductUsesGameInputs(toolkit, dolSha) ||
                installation.ProductUsesGameInputs(toolkit, TestDolSha256))
                throw new Exception("The product's recorded game inputs were not compared exactly.");

            File.WriteAllText(Path.Combine(installation.GameDirectory, "dsp_coef.bin"), "stale");
            Expect(ProductStatus.Broken, "A stale copied runtime asset");
            CopyFakeRuntimeAssets(root, installation.GameDirectory);
            Expect(ProductStatus.Current, "Restored runtime assets");

            var executable = File.ReadAllBytes(installation.GameExecutable);
            executable[0] ^= 0xFF;
            File.WriteAllBytes(installation.GameExecutable, executable);
            Expect(ProductStatus.Blocked, "A replaced game executable");
            executable[0] ^= 0xFF;
            File.WriteAllBytes(installation.GameExecutable, executable);
            Expect(ProductStatus.Current, "The original game executable");

            var dol = Path.Combine(installation.GameDataDirectory, "sys", "main.dol");
            File.WriteAllText(dol, "patched dol");
            Expect(ProductStatus.Blocked, "A modified installed main.dol");
            File.WriteAllText(dol, "stand-in dol");
            Expect(ProductStatus.Current, "The original installed main.dol");

            File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"), "translator-v2");
            var replaced = ToolkitFingerprint.Compute(root);
            installation.WriteToolkitState(replaced, ToolkitFingerprint.ComputePackage(root), "test",
                ToolkitFingerprint.ComputeRuntimeAssets(root));
            Expect(ProductStatus.ToolkitChanged, "A replaced translator");
            if (!ProductRepairService.FindDesiredInputDrift(installation, replaced, dolSha))
                throw new Exception("A product from another toolkit did not count as input drift.");

            File.Delete(installation.ToolkitStatePath);
            Expect(ProductStatus.Blocked, "A missing toolkit-state.json");
            File.Delete(installation.GameExecutable);
            Expect(ProductStatus.Broken, "An installation whose game executable is gone");
            File.Delete(installation.InstallStatePath);
            Expect(ProductStatus.Absent, "A folder with neither install state nor game");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>The build script's step lines drive progress; raw tool output and unknown ids stay diagnostics.</summary>
    private static void TestBuildProgressSteps()
    {
        var progress = new List<(string Stage, int Percent)>();
        var diagnostics = new List<string>();
        var reporter = new RecordingReporter(progress, diagnostics);
        var window = new BuildProgressWindow(reporter, InstallStages.Build, 10, 90);
        foreach (var id in new[]
                 {
                     BuildStepIds.Translate, BuildStepIds.GenerateDataInit, BuildStepIds.EmitBuildShards,
                     BuildStepIds.ConfigureNative, BuildStepIds.Compile
                 })
            window.Observe($"OPUCBUILD:STEP:{id} text for the log");
        window.Observe("[50/100] Building CXX object x.o");
        window.Observe("OPUCBUILD:STEP:unknown-step something");
        window.Observe("plain compiler output");
        if (progress.Count != 6 || progress.Any(entry => entry.Stage != InstallStages.Build))
            throw new Exception($"Expected 6 progress events, got {progress.Count}.");
        for (var i = 1; i < progress.Count; i++)
            if (progress[i].Percent <= progress[i - 1].Percent)
                throw new Exception("Build progress did not advance monotonically.");
        if (progress[^1].Percent >= 90 || progress[0].Percent <= 10)
            throw new Exception("Build progress left its window.");
        if (!diagnostics.Contains("plain compiler output") ||
            !diagnostics.Contains("OPUCBUILD:STEP:unknown-step something"))
            throw new Exception("Unrecognised build output was not kept as a diagnostic.");

        // A reused translation jumps straight past the translation share.
        progress.Clear();
        new BuildProgressWindow(reporter, InstallStages.Build, 0, 100)
            .Observe("OPUCBUILD:STEP:" + BuildStepIds.ReuseTranslation + " reuse");
        if (progress.Count != 1 || progress[0].Percent < 30)
            throw new Exception("A reused translation did not skip the translation share.");
    }

    private sealed class RecordingReporter(List<(string, int)> progress, List<string> diagnostics) : IInstallReporter
    {
        public void Progress(string stage, string message, int percent) => progress.Add((stage, percent));
        public void Diagnostic(string line) => diagnostics.Add(line);
    }

    /// <summary>The icon writer produces the files the resource compiler and the desktop entries consume.</summary>
    private static void TestProductIcon()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-icon-" + Guid.NewGuid().ToString("N"));
        try
        {
            var data = Path.Combine(root, "DATA");
            WriteFakeExtractedDisc(data);
            var output = Path.Combine(root, "icon");
            var png = ProductIcon.Generate(data, output);
            if (!png.Equals(Path.Combine(output, ProductIcon.BaseName + ".png"), StringComparison.Ordinal))
                throw new Exception("The returned PNG is not the 256 px icon.");

            static void ExpectPng(string path, int size)
            {
                var bytes = File.ReadAllBytes(path);
                if (!bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }))
                    throw new Exception($"{Path.GetFileName(path)} is not a PNG.");
                if (BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(16)) != size ||
                    BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(20)) != size)
                    throw new Exception($"{Path.GetFileName(path)} is not {size}x{size}.");
            }
            ExpectPng(png, 256);
            ExpectPng(Path.Combine(output, ProductIcon.BaseName + "_48.png"), 48);

            var ico = File.ReadAllBytes(Path.Combine(output, ProductIcon.BaseName + ".ico"));
            if (BinaryPrimitives.ReadUInt16LittleEndian(ico.AsSpan(2)) != 1 ||
                BinaryPrimitives.ReadUInt16LittleEndian(ico.AsSpan(4)) != 7)
                throw new Exception("The .ico does not hold the seven icon sizes.");
            if (File.ReadAllText(Path.Combine(output, ProductIcon.BaseName + ".rc")).Trim() !=
                $"1 ICON \"{ProductIcon.BaseName}.ico\"")
                throw new Exception("The resource script does not name the icon.");

            // The decoder honours both RGB5A3 texel forms.
            var (width, height, rgba) = ProductIcon.ReadTpl(FakeBannerTpl(), 1);
            if (width != 48 || height != 48) throw new Exception("Image 1 is not the 48x48 icon frame.");
            if (rgba[3] != 255 || rgba[2] != 255)
                throw new Exception("An opaque RGB555 texel decoded wrongly.");
            if (rgba[4 + 3] != (byte)Math.Round(3 * 255 / 7.0, MidpointRounding.AwayFromZero) || rgba[4 + 1] != 0xFF)
                throw new Exception("A translucent A3RGB444 texel decoded wrongly.");

            try
            {
                ProductIcon.ReadTpl(new byte[16], 1);
                throw new Exception("A file without the TPL magic was accepted.");
            }
            catch (InvalidDataException) { }
            try
            {
                ProductIcon.ReadTpl(FakeBannerTpl(), 5);
                throw new Exception("A missing TPL image was accepted.");
            }
            catch (InvalidDataException) { }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The SYSCONF lands where the runtime opens its NAND, an existing one is never replaced, and anything that is
    /// not a SYSCONF is refused.
    /// </summary>
    private static void TestSysconfSeed()
    {
        var root = Path.Combine(Path.GetTempPath(), "opuc1-sysconf-" + Guid.NewGuid().ToString("N"));
        try
        {
            PortableRoot.Create(root);
            var install = Path.Combine(root, PortableRoot.InstallDirectoryName);
            Directory.CreateDirectory(install);
            var expectedNand = Path.Combine(root, PortableRoot.UserDataDirectoryName, "NAND");
            if (!SysconfSeed.ResolveNandRoot(install).Equals(expectedNand, StringComparison.OrdinalIgnoreCase))
                throw new Exception("A portable installation's NAND is not UserData\\NAND.");

            var source = Path.Combine(root, "SYSCONF.source");
            var sysconf = new byte[SysconfSeed.SysconfLength];
            "SCv0"u8.CopyTo(sysconf);
            sysconf[100] = 0x42;
            File.WriteAllBytes(source, sysconf);
            var invalid = Path.Combine(root, "not-a-sysconf");
            File.WriteAllBytes(invalid, new byte[SysconfSeed.SysconfLength]);
            if (SysconfSeed.IsValidSysconf(invalid) || !SysconfSeed.IsValidSysconf(source))
                throw new Exception("The SYSCONF shape check is wrong.");
            try
            {
                SysconfSeed.EnsureSeeded(install, invalid, _ => { });
                throw new Exception("A file without the SCv0 magic was seeded.");
            }
            catch (InvalidDataException) { }

            var seeded = SysconfSeed.EnsureSeeded(install, source, _ => { }) ?? throw new Exception("An explicit SYSCONF was not seeded.");
            if (!seeded.Equals(Path.Combine(expectedNand, SysconfSeed.NandRelativePath),
                    StringComparison.OrdinalIgnoreCase) ||
                !File.ReadAllBytes(seeded).SequenceEqual(sysconf))
                throw new Exception("The SYSCONF was not copied into the NAND.");

            // The game writes its own settings into the NAND copy; a second install must keep them.
            var modified = (byte[])sysconf.Clone();
            modified[100] = 0x43;
            File.WriteAllBytes(seeded, modified);
            SysconfSeed.EnsureSeeded(install, source, _ => { });
            if (File.ReadAllBytes(seeded)[100] != 0x43)
                throw new Exception("An existing NAND SYSCONF was overwritten.");

            // Without a source anywhere the SYSCONF is optional: a note, no file, no failure.
            var otherInstall = Path.Combine(root, "other", PortableRoot.InstallDirectoryName);
            PortableRoot.Create(Path.Combine(root, "other"));
            Directory.CreateDirectory(otherInstall);
            var fakeDolphin = Path.Combine(root, "fake-dolphin");
            SysconfSeed.DolphinUserDirectoriesOverride = [fakeDolphin];
            try
            {
                var notes = new List<string>();
                if (SysconfSeed.EnsureSeeded(otherInstall, null, notes.Add) is not null ||
                    !notes.Contains(SysconfSeed.NotFoundNote) ||
                    File.Exists(Path.Combine(SysconfSeed.ResolveNandRoot(otherInstall), SysconfSeed.NandRelativePath)))
                    throw new Exception("A missing SYSCONF was not treated as optional.");

                // A Dolphin user folder that has one is found and copied.
                var dolphinSysconf = Path.Combine(fakeDolphin, "Wii", SysconfSeed.NandRelativePath);
                Directory.CreateDirectory(Path.GetDirectoryName(dolphinSysconf)!);
                File.WriteAllBytes(dolphinSysconf, sysconf);
                if (SysconfSeed.FindDolphinSysconf() != dolphinSysconf ||
                    SysconfSeed.EnsureSeeded(otherInstall, null, _ => { }) is not { } fromDolphin ||
                    !File.ReadAllBytes(fromDolphin).SequenceEqual(sysconf))
                    throw new Exception("The SYSCONF in a Dolphin user folder was not found and seeded.");
            }
            finally
            {
                SysconfSeed.DolphinUserDirectoriesOverride = null;
            }

            // A configured nand_root wins over the managed NAND directory.
            var custom = Path.Combine(root, "custom-nand");
            Directory.CreateDirectory(custom);
            var configPath = RuntimeConfiguration.ResolveConfigPath(install);
            File.WriteAllText(configPath, $"[paths]\nnand_root = \"{custom.Replace("\\", "/")}\"\n");
            if (!SysconfSeed.ResolveNandRoot(install).Equals(custom, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Config.toml nand_root was not honoured.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>The names the runtime hard-codes (runtime/include/runtime_config.h) and the setup must agree on.</summary>
    private static void TestProductNames()
    {
        if (ProductNames.FileSystemName != "One Piece Unlimited Recompilation Episode 1")
            throw new Exception("The file-system name differs from the runtime's kApplicationDirectoryName.");
        if (!RuntimeConfiguration.ApplicationDataConfigPath.EndsWith(
                Path.Combine(ProductNames.FileSystemName, RuntimeConfiguration.ConfigFileName), StringComparison.Ordinal))
            throw new Exception("The per-user Config.toml is not in the runtime's application data directory.");
        if (PortableRoot.MarkerFileName != "portable.txt" || PortableRoot.UserDataDirectoryName != "UserData" ||
            PortableRoot.MaximumSearchDepth != 4)
            throw new Exception("The portable layout differs from the runtime's.");
        if (ProductInfo.SetupCopyName != "opuc1-recomp-setup.exe" ||
            !Path.GetFileName(new Installation("C:\\x").GameExecutable).Equals("opuc1-recomp.exe", StringComparison.Ordinal))
            throw new Exception("The executable names differ from the build outputs.");
    }

    private static void Test(string name, Action action, List<string> failures)
    {
        try { action(); }
        catch (Exception ex) { failures.Add($"{name}: {ex.Message}"); }
    }
}
