namespace Opuc1Recomp.Setup.Windows;

using System.Reflection;
using Opuc1Recomp.Setup.Common;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        var progressJson = CommandLine.WantsProgressJson(args);

        try
        {
            PlatformChecks.EnsureSupportedHost();

            CommandLine command;
            try
            {
                command = CommandLine.Parse(args);
            }
            catch (ArgumentException ex)
            {
                // A command line was supplied, so a caller is driving this process. Rejecting it with a prompt
                // would hang that caller.
                if (progressJson) new NdjsonInstallReporter(ProductInfo.Version).Failure(ex.Message);
                else Console.Error.WriteLine(ex.Message);
                return 1;
            }

            switch (command.Mode)
            {
                case AppMode.Guided:
                    return GuidedSetup.Run();
                case AppMode.Version:
                    return ConsoleCommands.Version();
                case AppMode.Help:
                    return ConsoleCommands.Help();
                case AppMode.SelfTest:
                    return SelfTests.Run();
                case AppMode.VerifyInputs:
                    return ConsoleCommands.VerifyInputs(command);
                case AppMode.EmitPayloadIdentities:
                    return ConsoleCommands.EmitPayloadIdentities(command);
                case AppMode.CheckProducts:
                {
                    using var cancellationSignal = CancellationSignal.ObserveEnvironment();
                    return ConsoleCommands.CheckProducts(command, cancellationSignal.Token);
                }
                case AppMode.Launch:
                    return GameLaunchService.Launch(command.InstallDirectory is null
                        ? null
                        : Path.GetFullPath(command.InstallDirectory));
                case AppMode.SilentInstall or AppMode.RepairProducts:
                {
                    // CPUs without x86-64-v3 get an x86-64-v2 build.
                    HostCpuRequirement.EnsureSupported(X86CpuProfile.V2);
                    using var cancellationSignal = CancellationSignal.ObserveEnvironment();
                    return command.Mode == AppMode.SilentInstall
                        ? ConsoleCommands.Install(command, cancellationSignal.Token).GetAwaiter().GetResult()
                        : ConsoleCommands.RepairProducts(command, cancellationSignal.Token).GetAwaiter().GetResult();
                }
                case AppMode.Uninstall or AppMode.SilentUninstall:
                    UninstallService.StartWorker(command.InstallDirectory!,
                        quiet: command.Mode == AppMode.SilentUninstall || command.Quiet);
                    return 0;
                case AppMode.UninstallWorker:
                    return UninstallService.RunWorker(command.InstallDirectory!, command.Quiet);
                default:
                    return ConsoleCommands.Help();
            }
        }
        catch (Exception ex)
        {
            // A caller reading the NDJSON protocol must always receive a terminal result line, even when the failure
            // happened before the installer was reached. ConsoleCommands already reports its own failures, so this
            // only fires for errors raised outside it.
            if (progressJson)
            {
                new NdjsonInstallReporter(ProductInfo.Version).Failure(ex.Message);
                Console.Error.WriteLine(ex);
            }
            else
            {
                Console.Error.WriteLine(ex.Message);
                Console.Error.WriteLine();
                Console.Error.WriteLine(ex);
            }
            return 1;
        }
    }
}

internal static class PlatformChecks
{
    public static void EnsureSupportedHost()
    {
        if (!OperatingSystem.IsWindows() || !Environment.Is64BitOperatingSystem)
            throw new PlatformNotSupportedException(ProductNames.DisplayName + " requires 64-bit Windows.");
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 18362))
            throw new PlatformNotSupportedException(
                ProductNames.DisplayName + " requires Windows 10 version 1903 or newer.");
    }
}

internal static class ProductInfo
{
    public const string Name = ProductNames.DisplayName;
    public static readonly string Version =
        typeof(ProductInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()!
            .InformationalVersion;

    /// <summary>
    /// The setup executable is copied into the installation under this name. It is the launcher the Start menu
    /// shortcut runs and the uninstaller Windows calls, so the name is part of the installed layout.
    /// </summary>
    public const string SetupCopyName = ProductNames.SetupBaseName + ".exe";

    public const string UninstallKey =
        @"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + ProductNames.TechnicalId;

    public static string DefaultInstallDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Programs", ProductNames.FileSystemName);
}
