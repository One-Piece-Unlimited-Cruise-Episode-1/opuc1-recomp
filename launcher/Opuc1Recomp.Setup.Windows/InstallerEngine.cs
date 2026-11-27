using Opuc1Recomp.Setup.Common;

namespace Opuc1Recomp.Setup.Windows;

internal sealed class InstallerEngine
{
    private readonly IInstallReporter _reporter;

    public InstallerEngine(IInstallReporter reporter) => _reporter = reporter;

    public async Task InstallAsync(InstallOptions options, CancellationToken cancellationToken = default)
    {
        var installDirectory = Path.GetFullPath(options.InstallDirectory);
        ValidateInstallDirectory(installDirectory);

        InputValidation.ValidateExtension(options.GamePath);
        var parent = Directory.GetParent(installDirectory)?.FullName
                     ?? throw new InvalidOperationException("The installation directory has no parent.");
        Directory.CreateDirectory(parent);

        using var operationLock = InstallOperationLock.Acquire(installDirectory, _reporter);
        var existing = new Installation(installDirectory);

        PortableInstallHealing.HealMovedInstall(existing, _reporter);
        // Captured before publishing anything. An existing installation may be repaired or updated by this
        // invocation, but that must not recreate the user's shortcuts.
        var firstInstall = !existing.IsPresent;
        var previousState = existing.ReadInstallState();
        using var scratch = Directory.Exists(installDirectory)
            ? InstallScratchSpace.CreateInsideInstall(installDirectory, _reporter)
            : InstallScratchSpace.CreateSibling(installDirectory, _reporter);
        var staging = scratch.Root;

        const long payloadStagingAllowance = 2L * 1024 * 1024 * 1024;
        FileSystemUtilities.EnsureFreeSpace(installDirectory, payloadStagingAllowance, "Setup");

        using var payload = PayloadArchive.OpenCurrent();
        var manifest = payload.ReadManifest();
        var toolkit = InstalledLayout.Toolkit(staging);
        var workspace = InstalledLayout.Workspace(staging);

        // The disc decides which release's inputs everything below expects, so it is identified before any reuse
        // decision or toolkit extraction.
        _reporter.Progress(InstallStages.Validate, "Checking the Wii disc image...", 1);
        var nodTool = payload.ExtractDiscTool(Path.Combine(staging, "disc-tool"));
        var header = await InputValidation.ReadDiscHeaderAsync(nodTool, options.GamePath, cancellationToken);
        var release = InputValidation.EnsureCompatibleDisc(header, manifest);

        // An explicit SYSCONF is checked here, before hours of work. The NAND is only written after a successful
        // publication; without any SYSCONF the game still boots (SysconfSeed).
        var sysconfSource = options.SysconfPath is null ? null : Path.GetFullPath(options.SysconfPath);
        if (sysconfSource is not null && !SysconfSeed.IsValidSysconf(sysconfSource))
            throw new InvalidDataException(SysconfSeed.InvalidMessage(sysconfSource));
        if (sysconfSource is null && !SysconfSeed.IsValidSysconf(PlannedSysconfPath(installDirectory, options.Portable)))
            sysconfSource = SysconfSeed.FindDolphinSysconf();

        if (options.Portable)
        {
            var portableRoot = PortableRoot.Create(parent);
            _reporter.Diagnostic($"Installing as a portable installation under {portableRoot}.");
        }

        var candidateToolkitFingerprint = manifest.ToolkitFingerprint;
        var candidateToolkitPackageFingerprint = manifest.ToolkitPackageFingerprint;
        var candidateRuntimeAssetsFingerprint = manifest.RuntimeAssetsFingerprint;
        var installedToolkitFingerprint = existing.IsPresent ? existing.ResolveToolkitFingerprint() : "";
        var installedToolkitPackageFingerprint = existing.IsPresent ? existing.ResolveToolkitPackageFingerprint() : "";
        var reusableGameAssets = FindReusableGameAssets(existing, release);

        var sameToolkit = existing.IsPresent && reusableGameAssets is not null &&
                          installedToolkitFingerprint.Equals(candidateToolkitFingerprint, StringComparison.Ordinal);
        var samePackageContent = existing.IsPresent &&
                                 installedToolkitPackageFingerprint.Equals(candidateToolkitPackageFingerprint,
                                     StringComparison.Ordinal);
        var sameToolkitPackage = sameToolkit && samePackageContent;

        var runtimeAssetsCurrent = sameToolkit && RuntimeAssetsAreCurrent(existing,
            candidateRuntimeAssetsFingerprint, cancellationToken);
        var installedNodTool = Path.Combine(existing.ToolkitDirectory, "nodtool.exe");
        var extractToolkit = MustRefreshToolkit(sameToolkit, samePackageContent, File.Exists(installedNodTool));
        var extractWorkspace = !sameToolkit || !runtimeAssetsCurrent;

        _reporter.Progress(InstallStages.ExtractToolkit, "Unpacking the local recompilation toolkit...", 4);
        if (extractToolkit) payload.ExtractDirectory(InstalledLayout.ToolkitDirectoryName, toolkit);
        if (extractWorkspace)
        {
            payload.ExtractDirectory(InstalledLayout.WorkspaceDirectoryName, workspace);
            WorkspaceTimestamps.MarkChangedFiles(existing.WorkspaceDirectory, workspace,
                _reporter.Diagnostic, cancellationToken);
        }
        payload.ExtractHost(Path.Combine(staging, ProductInfo.SetupCopyName));
        payload.ExtractDirectory("licenses", Path.Combine(staging, "licenses"));
        payload.ExtractEntry(InstalledLayout.PayloadManifestFileName,
            Path.Combine(staging, InstalledLayout.PayloadManifestFileName));

        if (sameToolkit)
        {
            var inputDrift = ProductRepairService.FindDesiredInputDrift(existing, candidateToolkitFingerprint,
                release.DolSha256);
            var reconciliation = await ReconcileAsync(existing, Path.Combine(staging, "repair"), release.DolSha256,
                cancellationToken, inputDrift ? 5 : 11);
            var remainingCancellation = reconciliation.PublicationCommitted
                ? CancellationToken.None
                : cancellationToken;

            if (!runtimeAssetsCurrent)
                runtimeAssetsCurrent = RuntimeAssetsAreCurrent(existing, candidateRuntimeAssetsFingerprint,
                    remainingCancellation);

            remainingCancellation.ThrowIfCancellationRequested();
            EnsureReconciliationCurrent(existing, candidateToolkitFingerprint, release.DolSha256);

            var updatedState = BuildInstallState(existing.ReadInstallState(), installDirectory, manifest, release);
            PrepareMetadata(staging, manifest, updatedState);
            remainingCancellation.ThrowIfCancellationRequested();

            var releaseEntries = new List<InstallTransactionEntry>();
            if (!sameToolkitPackage)
                AddComponent(releaseEntries, staging, installDirectory, InstalledLayout.ToolkitDirectoryName);
            if (!runtimeAssetsCurrent)
                AddRuntimeAssetPublicationEntries(releaseEntries, staging, installDirectory,
                    candidateRuntimeAssetsFingerprint, remainingCancellation);
            Publish(staging, installDirectory, releaseEntries, remainingCancellation, createShortcuts: firstInstall);
            SeedSysconf(installDirectory, sysconfSource);
            return;
        }

        EnsureSufficientRemainingDiskSpace(installDirectory, needsExtractedDisc: reusableGameAssets is null,
            needsLocalBuild: true);

        if (reusableGameAssets is null)
        {
            await ExtractGameAssetsAsync(nodTool, options.GamePath,
                Path.Combine(staging, InstalledLayout.GameAssetsDirectoryName), release, cancellationToken);
        }

        await PublishToolkitAndBuildAsync(existing, staging, workspace, manifest, release, previousState,
            publishGameAssets: reusableGameAssets is null, createShortcuts: firstInstall,
            cancellationToken: cancellationToken);
        SeedSysconf(installDirectory, sysconfSource);
    }

    private void SeedSysconf(string installDirectory, string? source)
    {
        try
        {
            SysconfSeed.EnsureSeeded(installDirectory, source, _reporter.Diagnostic);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new IOException("The game was installed, but its NAND SYSCONF could not be written: " + ex.Message,
                ex);
        }
    }

    /// <summary>
    /// The SYSCONF an installation at <paramref name="installDirectory"/> will read once published. A portable root
    /// does not exist yet during the preflight, so its NAND is derived from where it will be created.
    /// </summary>
    private static string PlannedSysconfPath(string installDirectory, bool portable)
    {
        var nandRoot = portable
            ? Path.Combine(PortableRoot.UserDataDirectory(Path.GetDirectoryName(installDirectory)!), "NAND")
            : SysconfSeed.ResolveNandRoot(installDirectory);
        return Path.Combine(nandRoot, SysconfSeed.NandRelativePath);
    }

    internal static bool MustRefreshToolkit(bool sameToolkit, bool samePackageContent, bool nodToolPresent) =>
        !sameToolkit || !samePackageContent || !nodToolPresent;

    private static void AddComponent(List<InstallTransactionEntry> entries, string staging,
        string installDirectory, string name) =>
        entries.Add(InstallTransactionEntry.Directory(Path.Combine(staging, name),
            Path.Combine(installDirectory, name)));

    /// <summary>
    /// Build-produced workspace directories (not shipped in a release) grafted from the installed workspace into the
    /// staged one, carrying the translation, native build dir and verified inputs across a toolkit update. Each is
    /// re-validated by the build, so stale caches degrade to a clean rebuild, never reuse.
    /// </summary>
    private static readonly string[] WorkspaceCacheDirectories = ["generated", "build", "native-build", "Assets"];

    private async Task PublishToolkitAndBuildAsync(Installation existing, string staging, string stagedWorkspace,
        PayloadManifest manifest, GameRelease release, InstallState? previousState, bool publishGameAssets,
        bool createShortcuts, CancellationToken cancellationToken)
    {
        var installDirectory = existing.Root;

        foreach (var cacheDirectory in WorkspaceCacheDirectories)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var source = Path.Combine(existing.WorkspaceDirectory, cacheDirectory);
            if (!Directory.Exists(source)) continue;
            var destination = Path.Combine(stagedWorkspace, cacheDirectory);
            if (Directory.Exists(destination)) Directory.Delete(destination, recursive: true);
            Directory.Move(source, destination);
        }

        var state = BuildInstallState(previousState, installDirectory, manifest, release);
        PrepareMetadata(staging, manifest, state);

        cancellationToken.ThrowIfCancellationRequested();
        var entries = new List<InstallTransactionEntry>();
        AddComponent(entries, staging, installDirectory, InstalledLayout.ToolkitDirectoryName);
        AddComponent(entries, staging, installDirectory, InstalledLayout.WorkspaceDirectoryName);
        if (publishGameAssets) AddComponent(entries, staging, installDirectory, InstalledLayout.GameAssetsDirectoryName);

        Publish(staging, installDirectory, entries, cancellationToken, progressPercent: 8, completionPercent: 10,
            createShortcuts: createShortcuts);

        _reporter.Progress(InstallStages.Build, "Building the game with the published toolkit...", 11);
        await ReconcileAsync(existing, repairScratch: null, release.DolSha256, cancellationToken, 11);
        EnsureReconciliationCurrent(existing, manifest.ToolkitFingerprint, release.DolSha256);
        _reporter.Progress(InstallStages.Publish, "Installation complete.", 99);
    }

    private Task<ProductRepairService.ReconciliationResult> ReconcileAsync(Installation existing,
        string? repairScratch, string expectedDolSha256, CancellationToken cancellationToken, int progressStart)
    {
        var repair = new ProductRepairService(existing, _reporter);
        return repair.RepairAsync(new ProductRepairService.ReconcileOptions
        {
            ScratchRoot = repairScratch,
            ExpectedDolSha256 = expectedDolSha256,
            ProgressStart = progressStart,
            ProgressEnd = 92
        }, cancellationToken);
    }

    private static void EnsureReconciliationCurrent(Installation installation, string toolkitFingerprint,
        string expectedDolSha256)
    {
        var gameState = installation.CheckGame(toolkitFingerprint);
        if (gameState.Status == ProductStatus.Current &&
            installation.ProductUsesGameInputs(toolkitFingerprint, expectedDolSha256))
            return;

        throw new InvalidOperationException(
            "Building the game did not produce a current installation with the release's game inputs; release " +
            "metadata was not advanced." + (string.IsNullOrWhiteSpace(gameState.Detail) ? "" : " " + gameState.Detail));
    }

    /// <summary>
    /// Publishes the caller's component entries together with the release metadata every install and update
    /// advances, then applies the runtime configuration this installation owns.
    /// </summary>
    private void Publish(string staging, string installDirectory, List<InstallTransactionEntry> entries,
        CancellationToken cancellationToken, int progressPercent = 95, int completionPercent = 99,
        bool createShortcuts = false)
    {
        entries.Add(InstallTransactionEntry.Directory(Path.Combine(staging, "licenses"),
            Path.Combine(installDirectory, "licenses")));
        entries.Add(InstallTransactionEntry.File(Path.Combine(staging, ProductInfo.SetupCopyName),
            Path.Combine(installDirectory, ProductInfo.SetupCopyName)));
        entries.Add(InstallTransactionEntry.File(Path.Combine(staging, InstalledLayout.PayloadManifestFileName),
            Path.Combine(installDirectory, InstalledLayout.PayloadManifestFileName)));
        entries.Add(InstallTransactionEntry.File(Path.Combine(staging, ToolkitState.FileName),
            Path.Combine(installDirectory, ToolkitState.FileName)));
        entries.Add(InstallTransactionEntry.File(Path.Combine(staging, InstalledLayout.InstallStateFileName),
            Path.Combine(installDirectory, InstalledLayout.InstallStateFileName)));

        cancellationToken.ThrowIfCancellationRequested();
        RunningProductGuard.EnsureProductsNotRunning(installDirectory);
        _reporter.Progress(InstallStages.Publish, "Publishing the installation...", progressPercent);
        var configPath = RuntimeConfiguration.ResolveConfigPath(installDirectory);
        var configSnapshot = RuntimeConfiguration.Capture(configPath);
        using var transaction = InstallTransaction.Begin(installDirectory, _reporter, entries.ToArray());
        transaction.Publish();
        transaction.RecordRuntimeConfigurationMutation(configSnapshot);
        RuntimeConfiguration.SetDvdRoot(configPath,
            Path.Combine(installDirectory, InstalledLayout.GameAssetsDirectoryName, "DATA"));
        transaction.Commit();

        try
        {
            // A portable installation is owned by the folder it lives in, not by this machine. A machine-wide
            // uninstall entry for it would outlive the folder and would overwrite the entry of a normal installation
            // on the same account.
            if (PortableRoot.TryFind(installDirectory) is not null)
            {
                _reporter.Diagnostic(
                    "Portable installation: no Windows uninstall entry was registered. Remove the folder, or run the " +
                    "copied setup with --uninstall, to uninstall it.");
            }
            else
            {
                ShellIntegration.RegisterUninstaller(installDirectory);
            }
            if (createShortcuts)
                ShellIntegration.CreateShortcuts(installDirectory);
        }
        catch (Exception ex)
        {
            _reporter.Diagnostic("The installation succeeded, but Windows shell integration failed: " + ex.Message);
        }

        _reporter.Progress(InstallStages.Publish, "Installation published.", completionPercent);
    }

    private static void PrepareMetadata(string staging, PayloadManifest manifest, InstallState state)
    {
        JsonState.Write(Path.Combine(staging, ToolkitState.FileName), new ToolkitState
        {
            ToolkitFingerprint = manifest.ToolkitFingerprint,
            ToolkitPackageFingerprint = manifest.ToolkitPackageFingerprint,
            RuntimeAssetsFingerprint = manifest.RuntimeAssetsFingerprint,
            TranslationFingerprint = manifest.TranslationFingerprint,
            NativeToolchainFingerprint = manifest.NativeToolchainFingerprint,
            ToolkitReleaseTag = manifest.ToolkitReleaseTag
        }, StateJsonContext.Default);
        JsonState.Write(Path.Combine(staging, InstalledLayout.InstallStateFileName), state, StateJsonContext.Default);
    }

    private static InstallState BuildInstallState(InstallState? previousState, string installDirectory,
        PayloadManifest manifest, GameRelease release) =>
        new()
        {
            SchemaVersion = 1,
            SetupVersion = ProductInfo.Version,
            ProductVersion = manifest.ProductVersion,
            InstallDir = installDirectory,
            InstalledUtc = previousState?.InstalledUtc ?? DateTime.UtcNow,
            ToolkitReleaseTag = manifest.ToolkitReleaseTag,
            GameId = release.GameId,
            DolSha256 = release.DolSha256
        };

    private bool RuntimeAssetsAreCurrent(Installation installation, string expectedFingerprint,
        CancellationToken cancellationToken)
    {
        var installedSource = ToolkitFingerprint.TryComputeRuntimeAssets(installation.Root, cancellationToken,
            _reporter.Diagnostic);
        if (!expectedFingerprint.Equals(installedSource, StringComparison.Ordinal)) return false;
        return !File.Exists(installation.GameExecutable) ||
               ToolkitFingerprint.ProductRuntimeAssetsMatch(installation.GameDirectory, expectedFingerprint,
                   cancellationToken, _reporter.Diagnostic);
    }

    private void AddRuntimeAssetPublicationEntries(List<InstallTransactionEntry> entries, string staging,
        string installDirectory, string expectedFingerprint, CancellationToken cancellationToken)
    {
        // The identity was computed from this exact operation-owned payload extraction, so the staged assets are
        // published as they are; only repair has an installed source to re-verify.
        var sourceAssets = Path.Combine(InstalledLayout.Workspace(staging), "runtime", "assets");
        RuntimeAssetPublication.AddEntries(entries, sourceAssets,
            Path.Combine(staging, "runtime-asset-publication"),
            [(InstalledLayout.GameDirectoryName, Path.Combine(installDirectory, InstalledLayout.GameDirectoryName))],
            expectedFingerprint, "publication", _reporter.Diagnostic, cancellationToken);

        entries.Add(InstallTransactionEntry.Directory(sourceAssets,
            Path.Combine(InstalledLayout.Workspace(installDirectory), "runtime", "assets")));
    }

    private string? FindReusableGameAssets(Installation existing, GameRelease release)
    {
        var dataRoot = existing.GameDataDirectory;
        // No fst.bin simply means nothing was extracted here before; anything past that gate is an existing
        // extraction that fails reuse, which the log must say before the expensive re-extraction quietly repairs it.
        if (!File.Exists(Path.Combine(dataRoot, "sys", "fst.bin"))) return null;
        try
        {
            ValidateExtractedGame(dataRoot, release);
            return dataRoot;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            _reporter.Diagnostic("The installed game assets cannot be reused; the disc will be extracted again: " +
                                 ex.Message);
            return null;
        }
    }

    private async Task ExtractGameAssetsAsync(string nodTool, string gamePath, string destination,
        GameRelease release, CancellationToken cancellationToken)
    {
        _reporter.Progress(InstallStages.ExtractDisc,
            "Extracting the game disc. This is the longest preparation step...", 6);
        // Extracted straight into a "DATA" subfolder so the on-disk layout matches what Installation.GameDataDirectory
        // and the runtime's dvd_root expect - nodtool extracts sys/ and files/ directly into the directory it is
        // given.
        var dataRoot = Path.Combine(destination, "DATA");
        var extraction = await ProcessRunner.RunAsync(nodTool,
            ["extract", Path.GetFullPath(gamePath), dataRoot, "-q"],
            line => { if (!string.IsNullOrWhiteSpace(line)) _reporter.Diagnostic(line); },
            cancellationToken);
        if (extraction.ExitCode != 0)
            throw new InvalidDataException("Game extraction failed. " + extraction.CombinedOutput.Trim());
        ValidateExtractedGame(dataRoot, release);
    }

    internal static void ValidateExtractedGame(string dataRoot, GameRelease release)
    {
        var dol = Path.Combine(dataRoot, "sys", "main.dol");
        var fst = Path.Combine(dataRoot, "sys", "fst.bin");
        var boot = Path.Combine(dataRoot, "sys", "boot.bin");
        var banner = Path.Combine(dataRoot, ProductIcon.TplRelativePath);
        var missing = new[] { dol, fst, boot, banner }.Where(path => !File.Exists(path))
            .Select(path => Path.GetRelativePath(dataRoot, path)).ToList();
        if (missing.Count > 0)
            throw new InvalidDataException(
                $"The disc extraction is missing required {ProductNames.GameTitle} files: " +
                string.Join(", ", missing) + ".");

        var mismatches = new List<string>();
        var discGameId = Installation.ReadDiscGameId(dataRoot);
        if (!discGameId.Equals(release.GameId, StringComparison.Ordinal))
            mismatches.Add($"sys/boot.bin names {discGameId}");
        var actual = InputValidation.Sha256File(dol);
        if (!actual.Equals(release.DolSha256, StringComparison.OrdinalIgnoreCase))
            mismatches.Add($"main.dol is {new FileInfo(dol).Length:N0} bytes with SHA-256 {actual} " +
                           $"(expected {release.DolSha256})");

        if (mismatches.Count > 0)
            throw new InvalidDataException(
                $"The disc is {release.GameId} but its game data does not match the supported clean revision: " +
                string.Join("; ", mismatches) + ". " +
                "Patched or otherwise modified game code cannot be installed safely.");
    }

    private static void ValidateInstallDirectory(string path) =>
        FileSystemUtilities.EnsureUsableLocation(path, "The installation folder");

    private static void EnsureSufficientRemainingDiskSpace(string installDirectory, bool needsExtractedDisc,
        bool needsLocalBuild)
    {
        const long extractedDiscAllowance = 5L * 1024 * 1024 * 1024;
        const long localBuildAllowance = 14L * 1024 * 1024 * 1024;
        const long safetyAllowance = 2L * 1024 * 1024 * 1024;
        // This preflight runs after the release payload has already been staged. Count only work that can still
        // allocate space from this point.
        var required = checked((needsExtractedDisc ? extractedDiscAllowance : 0) +
                               (needsLocalBuild ? localBuildAllowance : 0) + safetyAllowance);
        FileSystemUtilities.EnsureFreeSpace(installDirectory, required, "Setup");
    }
}
