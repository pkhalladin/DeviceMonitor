using System;
using System.Diagnostics;

namespace DeviceMonitor.BleClient.Services;

/// <summary>
/// Manages "start with Windows" via a Task Scheduler logon task with highest privileges.
/// A plain HKCU\...\Run entry can't silently elevate a requireAdministrator app (it would
/// prompt for UAC every logon), so a scheduled task is used instead. Requires the current
/// process to be elevated to create/delete the task — which this app already is.
/// </summary>
public static class AutoStartService
{
    private const string TaskName = "DeviceMonitor";

    public static bool IsEnabled()
    {
        try
        {
            return RunSchtasks("/Query", "/TN", TaskName) == 0;
        }
        catch
        {
            return false;
        }
    }

    public static void Enable()
    {
        string exe = Environment.ProcessPath
            ?? throw new InvalidOperationException("Executable path unavailable.");

        // The task starts the app minimized to the tray at logon, running elevated silently.
        RunSchtasks(
            "/Create", "/TN", TaskName,
            "/TR", $"\"{exe}\" --minimized",
            "/SC", "ONLOGON",
            "/RL", "HIGHEST",
            "/F");
    }

    public static void Disable() => RunSchtasks("/Delete", "/TN", TaskName, "/F");

    private static int RunSchtasks(params string[] args)
    {
        var psi = new ProcessStartInfo("schtasks.exe")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        foreach (string a in args)
        {
            psi.ArgumentList.Add(a);
        }

        using Process process = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to start schtasks.exe.");
        process.WaitForExit();
        return process.ExitCode;
    }
}
