using System;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace DeviceMonitor.BleClient.Services;

/// <summary>
/// BLE central. Scans for the "DeviceMonitor" peripheral, connects, and once per second
/// writes the current 6-byte metrics frame (supplied by a payload provider) to the metrics
/// characteristic. Reconnects automatically if the link drops.
/// </summary>
public sealed class BleMetricsClient
{
    private readonly DispatcherQueue _dispatcher;
    private readonly Func<byte[]?> _payloadProvider;
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private readonly object _connectLock = new();

    private BluetoothLEAdvertisementWatcher? _watcher;
    private BluetoothLEDevice? _device;
    private GattSession? _session;
    private GattDeviceService? _service;
    private GattCharacteristic? _metrics;
    private DispatcherQueueTimer? _timer;

    private bool _running;
    private bool _connecting;
    private bool _connected;

    public event EventHandler<string>? StatusChanged;

    /// <summary>Raised with true right after connecting, false when the link tears down.</summary>
    public event EventHandler<bool>? ConnectedChanged;

    /// <summary>Raised after each metrics frame successfully written to the device.</summary>
    public event EventHandler<byte[]>? FrameSent;

    public BleMetricsClient(DispatcherQueue dispatcher, Func<byte[]?> payloadProvider)
    {
        _dispatcher = dispatcher;
        _payloadProvider = payloadProvider;
    }

    public void Start()
    {
        if (_running)
        {
            return;
        }

        _running = true;
        StartScan();
    }

    public void Stop()
    {
        _running = false;
        StopTimer();
        StopWatcher();
        Cleanup();
        SetStatus("Idle");
    }

    // ---- Scanning ----------------------------------------------------------
    private void StartScan()
    {
        SetStatus("Scanning…");
        _watcher = new BluetoothLEAdvertisementWatcher
        {
            ScanningMode = BluetoothLEScanningMode.Active,
        };
        _watcher.Received += OnAdvertisementReceived;
        _watcher.Start();
    }

    private void StopWatcher()
    {
        if (_watcher is null)
        {
            return;
        }

        _watcher.Received -= OnAdvertisementReceived;
        try
        {
            _watcher.Stop();
        }
        catch
        {
            // Watcher may already be stopped; ignore.
        }

        _watcher = null;
    }

    private async void OnAdvertisementReceived(
        BluetoothLEAdvertisementWatcher sender,
        BluetoothLEAdvertisementReceivedEventArgs args)
    {
        // Name and service UUID can arrive in separate packets — match on either.
        bool nameMatch = string.Equals(
            args.Advertisement.LocalName, BleIds.DeviceName, StringComparison.Ordinal);
        bool uuidMatch = args.Advertisement.ServiceUuids.Contains(BleIds.ServiceUuid);
        if (!nameMatch && !uuidMatch)
        {
            return;
        }

        // Ensure a single connect attempt at a time.
        lock (_connectLock)
        {
            if (!_running || _connecting || _metrics is not null)
            {
                return;
            }

            _connecting = true;
        }

        StopWatcher();
        await ConnectAsync(args.BluetoothAddress);
    }

    // ---- Connecting --------------------------------------------------------
    private async Task ConnectAsync(ulong address)
    {
        SetStatus("Connecting…");
        try
        {
            _device = await BluetoothLEDevice.FromBluetoothAddressAsync(address);
            if (_device is null)
            {
                // Not in the system cache yet — keep scanning for the next packet.
                SetStatus("Not cached, rescanning…");
                await Task.Delay(500);
                if (_running)
                {
                    StartScan();
                }

                return;
            }

            _device.ConnectionStatusChanged += OnConnectionStatusChanged;

            // Discover the service. The uncached call opens the link; right after connect
            // GATT calls can transiently fail or throw (e.g. 0x80070016 ERROR_BAD_COMMAND),
            // so retry. Enumerate ALL services and filter by UUID — more reliable than the
            // by-UUID discovery overload on some Bluetooth stacks.
            GattDeviceService? service = null;
            string lastError = "unknown";
            for (int attempt = 1; attempt <= 6 && _running && service is null; attempt++)
            {
                try
                {
                    GattDeviceServicesResult all =
                        await _device.GetGattServicesAsync(BluetoothCacheMode.Uncached);
                    if (all.Status == GattCommunicationStatus.Success)
                    {
                        foreach (GattDeviceService s in all.Services)
                        {
                            if (s.Uuid == BleIds.ServiceUuid)
                            {
                                service = s;
                                break;
                            }
                        }

                        if (service is null)
                        {
                            lastError = $"Success, {all.Services.Count} svc, target missing";
                        }
                    }
                    else
                    {
                        lastError = all.Status.ToString();
                    }
                }
                catch (Exception ex)
                {
                    lastError = $"0x{ex.HResult:X8}";
                }

                if (service is null)
                {
                    SetStatus($"Discovering service… ({lastError}, try {attempt}/6)");
                    await Task.Delay(700);
                }
            }

            if (service is null)
            {
                SetStatus($"Service not found ({lastError})");
                await Task.Delay(1500);
                RestartScan();
                return;
            }

            _service = service;

            // Same resilience for the characteristic.
            GattCharacteristic? metrics = null;
            string charError = "unknown";
            for (int attempt = 1; attempt <= 6 && _running && metrics is null; attempt++)
            {
                try
                {
                    GattCharacteristicsResult chars =
                        await _service.GetCharacteristicsAsync(BluetoothCacheMode.Uncached);
                    if (chars.Status == GattCommunicationStatus.Success)
                    {
                        foreach (GattCharacteristic c in chars.Characteristics)
                        {
                            if (c.Uuid == BleIds.MetricsUuid)
                            {
                                metrics = c;
                                break;
                            }
                        }

                        if (metrics is null)
                        {
                            charError = $"Success, {chars.Characteristics.Count} chr, target missing";
                        }
                    }
                    else
                    {
                        charError = chars.Status.ToString();
                    }
                }
                catch (Exception ex)
                {
                    charError = $"0x{ex.HResult:X8}";
                }

                if (metrics is null)
                {
                    SetStatus($"Discovering characteristic… ({charError}, try {attempt}/6)");
                    await Task.Delay(700);
                }
            }

            if (metrics is null)
            {
                SetStatus($"Characteristic not found ({charError})");
                await Task.Delay(1500);
                RestartScan();
                return;
            }

            _metrics = metrics;

            // Keep the link alive now that we hold the characteristic handle.
            _session = await GattSession.FromDeviceIdAsync(_device.BluetoothDeviceId);
            _session.MaintainConnection = true;

            SetStatus("Connected");
            _connected = true;
            ConnectedChanged?.Invoke(this, true);
            StartTimer();
        }
        catch (Exception ex)
        {
            SetStatus($"Error 0x{ex.HResult:X8}: {ex.Message}");
            await Task.Delay(1500);
            RestartScan();
        }
        finally
        {
            _connecting = false;
        }
    }

    private void OnConnectionStatusChanged(BluetoothLEDevice sender, object args)
    {
        if (sender.ConnectionStatus != BluetoothConnectionStatus.Disconnected)
        {
            return;
        }

        // Marshal off the event callback before disposing the device.
        _dispatcher.TryEnqueue(() =>
        {
            if (!_running)
            {
                return;
            }

            SetStatus("Disconnected");
            RestartScan();
        });
    }

    private void RestartScan()
    {
        StopTimer();
        Cleanup();
        if (_running)
        {
            StartScan();
        }
    }

    // ---- Sending -----------------------------------------------------------
    private void StartTimer()
    {
        _timer = _dispatcher.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += async (_, _) => await SendTickAsync();
        _timer.Start();
    }

    private void StopTimer()
    {
        _timer?.Stop();
        _timer = null;
    }

    private async Task SendTickAsync()
    {
        if (_metrics is null)
        {
            return;
        }

        byte[]? payload = _payloadProvider();
        if (payload is null || payload.Length == 0)
        {
            return;
        }

        // Skip this tick if the previous write is still in flight.
        if (!_writeGate.Wait(0))
        {
            return;
        }

        try
        {
            IBuffer buffer = payload.AsBuffer();
            GattWriteResult result = await _metrics.WriteValueWithResultAsync(
                buffer, GattWriteOption.WriteWithResponse);

            if (result.Status != GattCommunicationStatus.Success)
            {
                SetStatus($"Write failed: {result.Status}");
            }
            else
            {
                // Only delivered frames count — a dropped write never reaches the device
                // screen, so it must not reach a mirroring chart either.
                FrameSent?.Invoke(this, payload);
            }
        }
        catch (Exception ex)
        {
            SetStatus($"Write error: {ex.Message}");
        }
        finally
        {
            _writeGate.Release();
        }
    }

    // ---- Cleanup -----------------------------------------------------------
    private void Cleanup()
    {
        // Cleanup is the single teardown choke point (RestartScan + Stop), so the
        // connected→disconnected transition is reported here.
        if (_connected)
        {
            _connected = false;
            ConnectedChanged?.Invoke(this, false);
        }

        _metrics = null;

        _service?.Dispose();
        _service = null;

        if (_session is not null)
        {
            _session.MaintainConnection = false;
            _session.Dispose();
            _session = null;
        }

        if (_device is not null)
        {
            _device.ConnectionStatusChanged -= OnConnectionStatusChanged;
            _device.Dispose();
            _device = null;
        }
    }

    private void SetStatus(string status) => StatusChanged?.Invoke(this, status);
}
