using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// Reconciles the installed game against the installed toolkit and game assets: rebuilds it when either moved, and
/// republishes copied runtime assets when only those drifted.
/// </summary>
internal sealed class ProductRepairService
{
    /// <summary>
    /// Inputs every reconciliation shares. Everything here is optional context the caller already owns under the
    /// operation lock.
    /// </summary>
    internal sealed record ReconcileOptions
    {
        /// <summary>Operation scratch the caller owns, or null to own one for this call.</summary>
        public string? ScratchRoot { get; init; }

        /// <summary>The release's authoritative clean-disc main.dol identity, or null to keep the recorded one.</summary>
        public string? ExpectedDolSha256 { get; init; }

        /// <summary>The first progress percentage of the build window and its end.</summary>
        public int ProgressStart { get; init; } = 5;
        public int ProgressEnd { get; init; } = 92;
    }

    internal sealed record ReconciliationResult(bool PublicationCommitted);

    private readonly Installation _installation;
    private readonly IInstallReporter _reporter;

    public ProductRepairService(Installation installation, IInstallReporter reporter)
    {
        _installation = installation;
        _reporter = reporter;
    }

    /// <summary>
    /// Repairs the game and any copy-only drift the installation can fix from its own authoritative workspace. The
    /// caller must hold <see cref="InstallOperationLock"/>.
    /// </summary>
    public async Task<ReconciliationResult> RepairAsync(ReconcileOptions options,
        CancellationToken cancellationToken = default)
    {
        if (!_installation.IsPresent)
            throw new InvalidOperationException(ProductNames.DisplayName + " is not installed here.");
        if (!_installation.HasToolkit)
            throw new InvalidOperationException(
                "The installed recompilation toolkit is missing. Run the current setup with the disc image first.");
        RunningProductGuard.EnsureProductsNotRunning(_installation.Root);

        var toolkitFingerprint = _installation.ResolveToolkitFingerprint();
        if (string.IsNullOrWhiteSpace(toolkitFingerprint))
            throw new InvalidDataException("The installed recompilation toolkit cannot be identified safely.");

        using var ownedScratch = options.ScratchRoot is null
            ? InstallScratchSpace.CreateInsideInstall(_installation.Root, _reporter)
            : null;
        var scratchRoot = options.ScratchRoot ?? ownedScratch!.Root;
        Directory.CreateDirectory(scratchRoot);

        _reporter.Progress(InstallStages.Validate, "Checking whether the installed game is still current...", 3);
        var gameState = _installation.CheckGameCore(toolkitFingerprint);
        var inputDrift = FindDesiredInputDrift(_installation, toolkitFingerprint, options.ExpectedDolSha256);
        var rebuild = RequiresCompilation(gameState.Status) || inputDrift;

        // Copied support files are republished from the installed workspace, never recompiled. A rebuild already
        // stages fresh copies, so only a game that is not being rebuilt needs it.
        var syncRuntimeAssets = !rebuild && _installation.ValidateCopiedRuntimeAssets(toolkitFingerprint) is not null;

        if (!rebuild && !syncRuntimeAssets)
        {
            _reporter.Progress(InstallStages.Publish, "The installed game is already current.", 99);
            return new ReconciliationResult(PublicationCommitted: false);
        }

        cancellationToken.ThrowIfCancellationRequested();
        string? gameOutput = null;
        if (rebuild)
        {
            EnsureSufficientRepairDiskSpace();
            _reporter.Progress(InstallStages.Build, "Verifying the installed recompilation toolkit...", 4);
            var toolkitComponents = VerifyToolkitForCompilation(toolkitFingerprint, cancellationToken);
            var dolSha = RefreshTranslationAssets(options.ExpectedDolSha256, cancellationToken);
            ProductIcon.Generate(_installation.GameDataDirectory, _installation.WorkspaceIconDirectory);

            // A broken or blocked game may mean the caches themselves are damaged, so nothing recorded on disk is
            // allowed to contribute to the repaired product.
            var forceCleanBuild = gameState.Status is ProductStatus.Broken or ProductStatus.Blocked;
            gameOutput = Path.Combine(scratchRoot, "game-output");
            _reporter.Progress(InstallStages.Build,
                $"Recompiling {ProductNames.GameTitle} with the installed toolkit...", options.ProgressStart);
            var builder = new LocalBuildService(_reporter);
            await BuildWithCleanRetryAsync(forceCleanBuild, clean =>
                builder.BuildAsync(_installation.Root, gameOutput, cancellationToken, toolkitComponents, clean,
                    new BuildProgressWindow(_reporter, InstallStages.Build, options.ProgressStart,
                        options.ProgressEnd),
                    _installation.WorkspaceIconDirectory));
            LocalBuildService.WriteFingerprint(gameOutput, toolkitFingerprint, dolSha);
            PreserveProductConfig(_installation.GameDirectory, gameOutput);
        }

        cancellationToken.ThrowIfCancellationRequested();
        PublishProduct(gameOutput, syncRuntimeAssets, toolkitFingerprint, scratchRoot, options.ExpectedDolSha256,
            cancellationToken);
        _reporter.Progress(InstallStages.Publish, "The game is up to date.", 99);
        return new ReconciliationResult(PublicationCommitted: true);
    }

    /// <summary>The statuses that can only be resolved by producing the game again.</summary>
    private static bool RequiresCompilation(ProductStatus status) =>
        status is ProductStatus.ToolkitChanged or ProductStatus.Blocked or ProductStatus.Broken;

    private void PublishProduct(string? gameOutput, bool syncRuntimeAssets, string toolkitFingerprint,
        string scratchRoot, string? expectedDolSha256, CancellationToken cancellationToken)
    {
        var entries = new List<InstallTransactionEntry>();
        if (gameOutput is not null)
            entries.Add(InstallTransactionEntry.Directory(gameOutput, _installation.GameDirectory));
        if (syncRuntimeAssets)
            AddRuntimeAssetPublicationEntries(entries, scratchRoot, toolkitFingerprint, cancellationToken);

        // Copied-runtime repair does not change product or input provenance. Publish only the authoritative support
        // files in that case, leaving install-state byte-for-byte intact.
        if (gameOutput is not null)
        {
            var statePath = Path.Combine(scratchRoot, InstalledLayout.InstallStateFileName);
            JsonState.Write(statePath, BuildUpdatedInstallState(expectedDolSha256));
            entries.Add(InstallTransactionEntry.File(statePath, _installation.InstallStatePath));
        }

        cancellationToken.ThrowIfCancellationRequested();
        // The game may have been started during a long compile; publishing renames its directory.
        RunningProductGuard.EnsureProductsNotRunning(_installation.Root);
        using var transaction = InstallTransaction.Begin(_installation.Root, _reporter, entries.ToArray());
        transaction.Publish();
        transaction.Commit();
    }

    private async Task BuildWithCleanRetryAsync(bool forceCleanBuild, Func<bool, Task> build)
    {
        try
        {
            await build(forceCleanBuild);
        }
        catch (InvalidOperationException ex) when (!forceCleanBuild)
        {
            _reporter.Diagnostic($"Incremental recompilation failed ({ex.Message}); retrying with a clean build.");
            await build(true);
        }
    }

    private void EnsureSufficientRepairDiskSpace()
    {
        const long localBuildAllowance = 14L * 1024 * 1024 * 1024;
        const long safetyAllowance = 2L * 1024 * 1024 * 1024;
        FileSystemUtilities.EnsureFreeSpace(_installation.Root, localBuildAllowance + safetyAllowance,
            "Local recompilation");
    }

    internal ToolkitFingerprintComponents VerifyToolkitForCompilation(string authoritativeFingerprint,
        CancellationToken cancellationToken = default)
    {
        var state = _installation.ReadToolkitState();
        if (state is not { SchemaVersion: 2 } ||
            string.IsNullOrWhiteSpace(state.ToolkitFingerprint) ||
            !state.ToolkitFingerprint.Equals(authoritativeFingerprint, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The installed toolkit provenance is missing or does not match this repair operation.");
        }

        var components = ToolkitFingerprint.ComputeComponents(_installation.Root, cancellationToken);
        if (!components.Compile.Equals(authoritativeFingerprint, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The installed recompilation toolkit was modified or is incomplete. Run the current setup with the " +
                "disc image before compiling the game.");
        }
        return components;
    }

    private void AddRuntimeAssetPublicationEntries(List<InstallTransactionEntry> entries, string scratchRoot,
        string toolkitFingerprint, CancellationToken cancellationToken)
    {
        var state = _installation.ReadToolkitState();
        if (state is not { SchemaVersion: 2 } ||
            !state.ToolkitFingerprint.Equals(toolkitFingerprint, StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(state.RuntimeAssetsFingerprint))
        {
            throw new InvalidDataException(
                "The installed runtime-asset provenance is missing or does not belong to the current toolkit.");
        }

        var expectedFingerprint = state.RuntimeAssetsFingerprint;
        var sourceFingerprint = ToolkitFingerprint.ComputeRuntimeAssets(_installation.Root, cancellationToken);
        if (!sourceFingerprint.Equals(expectedFingerprint, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The installed BuildWorkspace runtime assets do not match their authoritative toolkit provenance.");
        }

        RuntimeAssetPublication.AddEntries(entries,
            Path.Combine(_installation.WorkspaceDirectory, "runtime", "assets"),
            Path.Combine(scratchRoot, "runtime-asset-repair"),
            [(InstalledLayout.GameDirectoryName, _installation.GameDirectory)], expectedFingerprint,
            "repair", _reporter.Diagnostic, cancellationToken);

        cancellationToken.ThrowIfCancellationRequested();
        var sourceAfter = ToolkitFingerprint.ComputeRuntimeAssets(_installation.Root, cancellationToken);
        if (!sourceAfter.Equals(sourceFingerprint, StringComparison.Ordinal))
            throw new IOException("The installed BuildWorkspace runtime assets changed while repair was being prepared.");
    }

    private InstallState BuildUpdatedInstallState(string? expectedDolSha256)
    {
        var state = _installation.ReadInstallState() ?? new InstallState { InstallDir = _installation.Root };
        state.InstallDir = _installation.Root;
        state.SchemaVersion = 1;
        if (string.IsNullOrWhiteSpace(state.SetupVersion)) state.SetupVersion = ProductInfo.Version;
        if (string.IsNullOrWhiteSpace(state.ProductVersion)) state.ProductVersion = state.SetupVersion;
        if (string.IsNullOrWhiteSpace(state.GameId)) state.GameId = ProductNames.GameId;
        if (!string.IsNullOrWhiteSpace(expectedDolSha256)) state.DolSha256 = expectedDolSha256;
        return state;
    }

    internal static bool FindDesiredInputDrift(Installation installation, string toolkitFingerprint,
        string? expectedDolSha256) =>
        !string.IsNullOrWhiteSpace(expectedDolSha256) &&
        File.Exists(installation.GameExecutable) &&
        !installation.ProductUsesGameInputs(toolkitFingerprint, expectedDolSha256);

    /// <summary>Copies the installed main.dol into the workspace the translator reads, after checking its identity.</summary>
    /// <returns>The SHA-256 of the translated main.dol.</returns>
    private string RefreshTranslationAssets(string? expectedDolSha256, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(expectedDolSha256))
        {
            var state = _installation.ReadInstallState();
            expectedDolSha256 = state is { SchemaVersion: 1 } ? state.DolSha256 : null;
        }
        if (string.IsNullOrWhiteSpace(expectedDolSha256))
            throw new InvalidDataException(
                "The installed game-input provenance is missing or unsupported. Run the current setup with the disc image.");

        Directory.CreateDirectory(_installation.WorkspaceAssetsDirectory);
        var source = Path.Combine(_installation.GameDataDirectory, "sys", "main.dol");
        cancellationToken.ThrowIfCancellationRequested();
        if (!File.Exists(source))
            throw new InvalidDataException(
                $"The installed {ProductNames.GameTitle} main.dol is missing. Run setup again with the disc image.");
        if (!InputValidation.Sha256File(source).Equals(expectedDolSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(
                $"The installed {ProductNames.GameTitle} main.dol does not match its recorded clean-disc identity. " +
                "Run the current setup with the disc image.");

        var workspaceInput = Path.Combine(_installation.WorkspaceAssetsDirectory, "main.dol");
        if (!File.Exists(workspaceInput) ||
            !InputValidation.Sha256File(workspaceInput).Equals(expectedDolSha256, StringComparison.OrdinalIgnoreCase))
            File.Copy(source, workspaceInput, overwrite: true);
        return expectedDolSha256.ToLowerInvariant();
    }

    private static void PreserveProductConfig(string currentProduct, string preparedProduct)
    {
        var config = Path.Combine(currentProduct, "config");
        if (Directory.Exists(config))
            FileSystemUtilities.CopyDirectory(config, Path.Combine(preparedProduct, "config"));
    }
}
