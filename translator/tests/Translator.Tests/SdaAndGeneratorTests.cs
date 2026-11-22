using System;
using System.IO;
using Translator.Core;
using Translator.Core.CodeGen;
using Translator.Core.Parsing.Dol;
using Xunit;

namespace Translator.Tests;

public class SdaAndGeneratorTests
{
    [Fact]
    public void RuntimeConfigGeneratorEmitsConfiguredConstants()
    {
        const uint entry = 0x80006000;
        var dol = SyntheticDolFactory.Create(
            entry,
            sections:
            [
                SyntheticDolFactory.Text(0, entry, 0x60000000)
            ]);

        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_runtime_config_tests");
        Directory.CreateDirectory(tempDir);
        var output = Path.Combine(tempDir, "RuntimeConfig.generated.h");

        RuntimeConfigGenerator.GenerateConfigHeader(0x8038F780u, 0x8038E9C8u, output);
        var text = File.ReadAllText(output);

        Assert.Contains("constexpr uint32_t SDA1_BASE = 0x8038F780u;", text);
        Assert.Contains("constexpr uint32_t SDA2_BASE = 0x8038E9C8u;", text);
    }

    [Fact]
    public void RuntimeConfigGeneratorDoesNotRewriteIdenticalContent()
    {
        const uint entry = 0x80006000;
        var dol = SyntheticDolFactory.Create(
            entry,
            sections:
            [
                SyntheticDolFactory.Text(0, entry, 0x60000000)
            ]);

        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_runtime_config_write_tests");
        Directory.CreateDirectory(tempDir);
        var output = Path.Combine(tempDir, "RuntimeConfig.generated.h");

        RuntimeConfigGenerator.GenerateConfigHeader(0x8038F780u, 0x8038E9C8u, output);
        var sentinel = new DateTime(2001, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(output, sentinel);

        RuntimeConfigGenerator.GenerateConfigHeader(0x8038F780u, 0x8038E9C8u, output);

        Assert.Equal(sentinel, File.GetLastWriteTimeUtc(output));
    }

    [Fact]
    public void DataSectionGeneratorEmbedsDolSections()
    {
        var dol = SyntheticDolFactory.Create(
            0x80004000,
            bssAddress: 0x80009000,
            bssSize: 0x40,
            sections:
            [
                SyntheticDolFactory.Text(0, 0x80004000, 0x60000000),
                SyntheticDolFactory.Data(5, 0x80008000, 0xAA, 0xBB, 0xCC)
            ]);

        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_data_section_tests");
        Directory.CreateDirectory(tempDir);
        var output = Path.Combine(tempDir, "data_sections_init_test.cpp");

        DataSectionGenerator.Generate(dol, output);
        var text = File.ReadAllText(output);

        Assert.Contains("kData__data", text);
        Assert.Contains("InitializeDataSections", text);
        Assert.Contains("g_dataInitialized", text);
        Assert.Contains("0x80008000", text);
    }

    [Fact]
    public void DataSectionGeneratorDoesNotEmitRuntimeBssClearAfterInitializedSections()
    {
        var dol = SyntheticDolFactory.Create(
            0x80004000,
            bssAddress: 0x80008000,
            bssSize: 0x100,
            sections:
            [
                SyntheticDolFactory.Data(5, 0x80008020, 0x41, 0x20, 0x00, 0x00)
            ]);

        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_data_section_bss_overlap_tests");
        Directory.CreateDirectory(tempDir);
        var output = Path.Combine(tempDir, "data_sections_init_overlap_test.cpp");

        DataSectionGenerator.Generate(dol, output);
        var text = File.ReadAllText(output);

        Assert.Contains("0x80008020", text);
        Assert.Contains("BSS section @ 0x80008000 (256 bytes) - memory already zero-initialized", text);
        Assert.DoesNotContain("std::memset", text);
    }

    [Fact]
    public void DataSectionGeneratorCanWriteStagedBlobsWithFinalIncbinReferences()
    {
        var dol = SyntheticDolFactory.Create(
            0x80004000,
            sections:
            [
                SyntheticDolFactory.Data(5, 0x80008000, 0xAA, 0xBB, 0xCC)
            ]);
        var tempDir = Path.Combine(Path.GetTempPath(), $"mkw_data_section_staging_{Guid.NewGuid():N}");
        try
        {
            var stagedDirectory = Path.Combine(tempDir, "staging");
            var finalBlobDirectory = Path.Combine(tempDir, "published", "data_sections_init_blobs");
            var output = Path.Combine(stagedDirectory, "data_sections_init.cpp");

            DataSectionGenerator.Generate(
                dol,
                output,
                blobReferenceDirectory: finalBlobDirectory);

            var assembly = File.ReadAllText(Path.Combine(stagedDirectory, "data_sections_init_blobs.S"));
            var expectedReference = Path.GetFullPath(Path.Combine(finalBlobDirectory, "_data.bin")).Replace('\\', '/');
            Assert.Contains($".incbin \"{expectedReference}\"", assembly, StringComparison.Ordinal);
            Assert.True(File.Exists(Path.Combine(stagedDirectory, "data_sections_init_blobs", "_data.bin")));
            Assert.False(File.Exists(Path.Combine(finalBlobDirectory, "_data.bin")));
        }
        finally
        {
            if (Directory.Exists(tempDir))
            {
                Directory.Delete(tempDir, recursive: true);
            }
        }
    }

    [Fact]
    public void DataSectionGeneratorChangesAssemblyWhenSameSizeBlobContentChanges()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), $"mkw_data_section_hash_{Guid.NewGuid():N}");
        try
        {
            var output = Path.Combine(tempDir, "data_sections_init.cpp");
            var first = SyntheticDolFactory.Create(
                0x80004000,
                sections: [SyntheticDolFactory.Data(5, 0x80008000, 1, 2, 3, 4)]);
            DataSectionGenerator.Generate(first, output);
            var assemblyPath = Path.Combine(tempDir, "data_sections_init_blobs.S");
            var before = File.ReadAllText(assemblyPath);

            var second = SyntheticDolFactory.Create(
                0x80004000,
                sections: [SyntheticDolFactory.Data(5, 0x80008000, 1, 2, 3, 5)]);
            DataSectionGenerator.Generate(second, output);
            var after = File.ReadAllText(assemblyPath);

            Assert.NotEqual(before, after);
            Assert.Contains("sha256=", after, StringComparison.Ordinal);
        }
        finally
        {
            if (Directory.Exists(tempDir))
            {
                Directory.Delete(tempDir, recursive: true);
            }
        }
    }
}
