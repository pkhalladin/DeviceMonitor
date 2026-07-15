using System;
using System.Linq;
using Microsoft.UI.Xaml;

namespace DeviceMonitor.BleClient;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();

        // The logon task launches with --minimized so it starts hidden in the tray.
        bool minimized = Environment.GetCommandLineArgs()
            .Any(a => string.Equals(a, "--minimized", StringComparison.OrdinalIgnoreCase));
        if (minimized)
        {
            _window.MinimizeToTray();
        }
    }
}
