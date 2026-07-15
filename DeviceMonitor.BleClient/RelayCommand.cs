using System;
using System.Windows.Input;

namespace DeviceMonitor.BleClient;

/// <summary>Minimal always-executable ICommand used to wire tray-icon clicks.</summary>
internal sealed class RelayCommand : ICommand
{
    private readonly Action _execute;

    public RelayCommand(Action execute) => _execute = execute;

    public event EventHandler? CanExecuteChanged { add { } remove { } }

    public bool CanExecute(object? parameter) => true;

    public void Execute(object? parameter) => _execute();
}
