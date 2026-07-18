using System;
using System.Runtime.InteropServices;
using H.NotifyIcon;
using H.NotifyIcon.Core;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.Graphics;
using Windows.UI;
using DeviceMonitor.BleClient.Services;
using SD = System.Drawing;

namespace DeviceMonitor.BleClient;

public sealed partial class MainWindow : Window
{
    private readonly DispatcherQueue _dispatcher;
    private readonly HardwareMonitorService _hardware;
    private readonly BleMetricsClient _client;

    // Per-column value colors (match the device screen); gray is used for "--" (no reading).
    private static readonly SolidColorBrush LoadBrush = new(Color.FromArgb(0xFF, 0xFF, 0xFF, 0xFF)); // white
    private static readonly SolidColorBrush TempBrush = new(Color.FromArgb(0xFF, 0xFF, 0xC1, 0x07)); // amber (yellow on the device — pulled toward orange here to stand apart from white)
    private static readonly SolidColorBrush MemBrush  = new(Color.FromArgb(0xFF, 0xD9, 0x66, 0xFF)); // violet (brighter than the device's — the dark hue is too dim on a desktop screen)
    private static readonly SolidColorBrush NaBrush   = new(Color.FromArgb(0xFF, 0x80, 0x80, 0x80)); // gray

    private static readonly SolidColorBrush AxisBrush = new(Color.FromArgb(0xFF, 0x3A, 0x3A, 0x3A)); // dark gray

    private bool _trayCreated;
    private bool _exiting;

    // ---- Tray icons (two permanent icons: [0] CPU, [1] GPU; see UpdateTrayIcons) ----
    // Latest sample in BLE frame order: cpuLoad, cpuTemp, ramUsed, gpuLoad, gpuTemp, vramUsed.
    private readonly byte[] _trayValues = { Na, Na, Na, Na, Na, Na };
    private readonly TaskbarIcon?[] _trays = new TaskbarIcon?[2];
    // Icon.FromHandle does not own the HICON, so each handle is kept alongside its Icon
    // and both are released explicitly on every swap (a leak here grows ~2 GDI objects/s).
    private readonly SD.Icon?[] _trayIcons = new SD.Icon?[2];
    private readonly nint[] _trayHicons = new nint[2];

    // Each icon shows one metric (0 load, 1 temp, 2 mem) as a big number. Every
    // _trayTimer tick each device independently switches to the metric that moved
    // the most since the previous tick (largest |current - previous|); ties keep
    // the incumbent so a quiet system does not flicker between colors.
    private readonly int[] _trayMetrics = new int[2];
    // Values as of the previous tick — the comparison base for the deltas.
    private readonly byte[] _trayPrevValues = { Na, Na, Na, Na, Na, Na };
    private DispatcherQueueTimer? _trayTimer;

    // Metric palette as System.Drawing colors (same values as the brushes above).
    private static readonly SD.Color[] TrayMetricColors =
    {
        SD.Color.FromArgb(0xFF, 0xFF, 0xFF), // load
        SD.Color.FromArgb(0xFF, 0xC1, 0x07), // temp
        SD.Color.FromArgb(0xD9, 0x66, 0xFF), // mem
    };
    private static readonly SD.Color TrayNaColor = SD.Color.FromArgb(0x80, 0x80, 0x80);

    // ---- Title status (emoji in the window/taskbar title; see ApplyTitleStatus) ----
    private BleStatusKind _titleKind = BleStatusKind.Idle;
    private string? _titleDetail;
    private bool _titleFrame;                 // alternates the two-frame title animations
    private DispatcherQueueTimer? _titleTimer;

    // ---- Chart views (mirror the device: view 0 = table, 1 = CPU chart, 2 = GPU chart) ----
    private const int HistLen = 100;   // rolling history depth: 100 samples = ~100 s (1 sample/s)
    private const byte Na = 0xFF;      // sentinel for "no reading"
    private const int ViewCount = 3;

    // Line color per chart column (LOAD white, TEMP yellow, MEM violet — as on the table).
    private static readonly SolidColorBrush[] MetricBrushes = { LoadBrush, TempBrush, MemBrush };

    // Rolling per-metric samples (ring buffer). Metric order matches the BLE frame:
    // 0 cpuLoad, 1 cpuTemp, 2 ramUsed, 3 gpuLoad, 4 gpuTemp, 5 vramUsed.
    // Touched only on the dispatcher thread (OnSample enqueues before writing).
    private readonly byte[][] _hist =
        { new byte[HistLen], new byte[HistLen], new byte[HistLen],
          new byte[HistLen], new byte[HistLen], new byte[HistLen] };
    private int _histCount;
    private int _histHead;
    private int _view;    // 0 = table, 1 = CPU chart, 2 = GPU chart
    private bool _mirror; // hybrid chart source: true = connected, history fed from delivered
                          // BLE frames (exact mirror of the device chart, rebuilt from zero
                          // at connect); false = disconnected, fed from local samples

    public MainWindow()
    {
        InitializeComponent();
        _dispatcher = DispatcherQueue.GetForCurrentThread();

        _hardware = new HardwareMonitorService();
        _hardware.Updated += OnSample;
        _hardware.Error += OnErrorMessage;

        _client = new BleMetricsClient(_dispatcher, () => _hardware.CurrentPayload);
        _client.StatusChanged += OnBleStatus;
        _client.ConnectedChanged += OnConnectedChanged;
        _client.FrameSent += OnFrameSent;

        // Modern title bar: the content extends under the caption area, AppTitleBar is
        // the drag region, and the system caption buttons are recolored for the dark UI.
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        ConfigureTitleBarButtons();

        // Fixed-size window: no resizing, no maximize, no minimize — closing (X) hides
        // to the tray instead.
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsResizable = false;
            presenter.IsMaximizable = false;
            presenter.IsMinimizable = false;
        }

        // With ExtendsContentIntoTitleBar the double-click-to-maximize gesture on the
        // drag region bypasses IsMaximizable, so the message is swallowed in HookWndProc.
        InstallWndProcHook();

        AppWindow.Resize(new SizeInt32(440, 300));
        AppWindow.Closing += OnAppWindowClosing;
        AppWindow.Changed += OnAppWindowChanged;
        Activated += OnActivated;
        Closed += OnClosed;

        // Background monitoring + BLE connect both run for the whole session.
        _hardware.Start();
        _client.Start();
    }

    /// <summary>Hide to the tray at startup (used when launched by the logon task).</summary>
    public void MinimizeToTray() => AppWindow.Hide();

    // ---- WndProc hook (blocks double-click maximize on the custom title bar) ----
    private const int GWLP_WNDPROC = -4;
    private const uint WM_NCLBUTTONDBLCLK = 0x00A3;
    private const nint HTCAPTION = 2;

    private delegate nint WndProc(nint hWnd, uint msg, nint wParam, nint lParam);

    private WndProc? _wndProcHook; // field keeps the delegate alive for the native side
    private nint _originalWndProc;

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr(nint hWnd, int index, nint newLong);

    [DllImport("user32.dll", EntryPoint = "CallWindowProcW")]
    private static extern nint CallWindowProc(nint prevWndFunc, nint hWnd, uint msg, nint wParam, nint lParam);

    private void InstallWndProcHook()
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _wndProcHook = HookWndProc;
        _originalWndProc = SetWindowLongPtr(
            hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(_wndProcHook));
    }

    private nint HookWndProc(nint hWnd, uint msg, nint wParam, nint lParam)
    {
        if (msg == WM_NCLBUTTONDBLCLK && wParam == HTCAPTION)
        {
            return 0; // ignore double-click on the title bar
        }

        return CallWindowProc(_originalWndProc, hWnd, msg, wParam, lParam);
    }

    private void ConfigureTitleBarButtons()
    {
        var titleBar = AppWindow.TitleBar;
        var transparent = Color.FromArgb(0x00, 0x00, 0x00, 0x00);
        var white = Color.FromArgb(0xFF, 0xFF, 0xFF, 0xFF);

        titleBar.ButtonBackgroundColor = transparent;
        titleBar.ButtonInactiveBackgroundColor = transparent;
        titleBar.ButtonForegroundColor = white;
        titleBar.ButtonInactiveForegroundColor = Color.FromArgb(0xFF, 0x80, 0x80, 0x80);
        titleBar.ButtonHoverBackgroundColor = Color.FromArgb(0xFF, 0x2A, 0x2A, 0x2A);
        titleBar.ButtonHoverForegroundColor = white;
        titleBar.ButtonPressedBackgroundColor = Color.FromArgb(0xFF, 0x40, 0x40, 0x40);
        titleBar.ButtonPressedForegroundColor = white;
    }

    // ---- Tray icon ---------------------------------------------------------
    private void OnActivated(object sender, WindowActivatedEventArgs e)
    {
        if (_trayCreated)
        {
            return;
        }

        _trayCreated = true;
        CreateTrayIcon();
    }

    private void CreateTrayIcon()
    {
        // Two icons, created GPU first then CPU — on this shell that puts CPU on the
        // left. Each needs its own fixed Guid (the default Id is derived from the
        // process path, so two icons would collide). New icons may land in the
        // overflow until the user pins them.
        for (var i = 1; i >= 0; i--)
        {
            var device = i == 0 ? "CPU" : "GPU";
            _trays[i] = new TaskbarIcon
            {
                Id = TrayIcon.CreateUniqueGuidFromString("DeviceMonitor.BleClient." + device),
                ToolTipText = device,
                ContextFlyout = BuildTrayMenu(),
                LeftClickCommand = new RelayCommand(ShowMainWindow),
                DoubleClickCommand = new RelayCommand(ShowMainWindow),
                NoLeftClickDelay = true,
            };
        }

        UpdateTrayIcons(); // both start as a gray "--" until the first sample lands
        for (var i = 1; i >= 0; i--)
        {
            _trays[i]!.ForceCreate(); // GPU registers first — see the ordering note above
        }

        _trayTimer = CreateTrayTimer();
        _trayTimer.Start();
    }

    // Redraws both tray icons with the currently cycled metric and swaps them in.
    // The previous Icon/HICON pair is released only after the swap (the shell keeps
    // its own copy of the icon).
    private void UpdateTrayIcons()
    {
        if (_exiting)
        {
            return;
        }

        for (var i = 0; i < 2; i++)
        {
            if (_trays[i] is not { } tray)
            {
                continue;
            }

            var (icon, hicon) = CreateValueIcon(
                _trayValues[i * 3 + _trayMetrics[i]], TrayMetricColors[_trayMetrics[i]]);
            tray.Icon = icon;
            DestroyTrayIconImage(i);
            _trayIcons[i] = icon;
            _trayHicons[i] = hicon;
        }
    }

    // Each tick re-picks, per device, the metric with the largest change since the
    // previous tick and redraws both icons with the values as of the tick (frozen
    // for the whole second).
    private DispatcherQueueTimer CreateTrayTimer()
    {
        var timer = _dispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromSeconds(1);
        timer.Tick += (_, _) =>
        {
            if (_exiting)
            {
                return;
            }

            for (var i = 0; i < 2; i++)
            {
                // Start from the incumbent so a tie does not switch the metric.
                var bestMetric = _trayMetrics[i];
                var bestDelta = TrayDelta(i, bestMetric);
                for (var m = 0; m < 3; m++)
                {
                    var delta = TrayDelta(i, m);
                    if (delta > bestDelta)
                    {
                        bestDelta = delta;
                        bestMetric = m;
                    }
                }

                _trayMetrics[i] = bestMetric;
            }

            _trayValues.CopyTo(_trayPrevValues, 0);
            UpdateTrayIcons();
        };
        return timer;
    }

    // Change of a metric since the previous tick: -1 = no current reading (never
    // selected), 255 = a reading just (re)appeared, otherwise |current - previous|.
    private int TrayDelta(int device, int metric)
    {
        var cur = _trayValues[device * 3 + metric];
        var prev = _trayPrevValues[device * 3 + metric];
        if (cur == Na)
        {
            return -1;
        }

        return prev == Na ? 255 : Math.Abs(cur - prev);
    }

    private void DestroyTrayIconImage(int i)
    {
        _trayIcons[i]?.Dispose();
        _trayIcons[i] = null;
        if (_trayHicons[i] != 0)
        {
            DestroyIcon(_trayHicons[i]);
            _trayHicons[i] = 0;
        }
    }

    private void DestroyTrayIconImages()
    {
        for (var i = 0; i < 2; i++)
        {
            DestroyTrayIconImage(i);
        }
    }

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(nint hIcon);

    // Tray icon image: the cycled metric's value as one big number in the metric color
    // (LOAD white / TEMP yellow / MEM violet) — at 16 px tray size a two-digit number
    // stays readable where bars did not. Na renders as a gray "--".
    // The caller owns both the Icon and the HICON (Icon.FromHandle does not take over).
    private static (SD.Icon Icon, nint Hicon) CreateValueIcon(byte value, SD.Color color)
    {
        const int size = 128;
        const float box = 120f; // target text extent — near full bleed for readability

        using var bitmap = new SD.Bitmap(size, size);
        using var graphics = SD.Graphics.FromImage(bitmap);
        graphics.TextRenderingHint = SD.Text.TextRenderingHint.AntiAliasGridFit;

        using var background = new SD.SolidBrush(SD.Color.FromArgb(0x18, 0x18, 0x18));
        graphics.FillRectangle(background, 0, 0, size, size);

        var na = value == Na;
        var text = na ? "--" : value.ToString();

        // One measuring pass scales the font so the text fills the box regardless of
        // digit count ("7", "47" and "104" all come out as large as they can be).
        using var probe = new SD.Font("Segoe UI", 80, SD.FontStyle.Bold);
        var measured = graphics.MeasureString(text, probe);
        var scale = Math.Min(box / measured.Width, box / measured.Height);
        using var font = new SD.Font("Segoe UI", 80 * scale, SD.FontStyle.Bold);

        using var format = new SD.StringFormat
        {
            Alignment = SD.StringAlignment.Center,
            LineAlignment = SD.StringAlignment.Center,
        };
        using var brush = new SD.SolidBrush(na ? TrayNaColor : color);
        graphics.DrawString(text, font, brush, new SD.RectangleF(0, 0, size, size), format);

        var hicon = bitmap.GetHicon();
        return (SD.Icon.FromHandle(hicon), hicon);
    }

    private MenuFlyout BuildTrayMenu()
    {
        // The tray menu is shown in the default ContextMenuMode.PopupMenu (native Win32
        // menu). In that mode H.NotifyIcon executes only Command — Click is never raised.
        var autoStart = new ToggleMenuFlyoutItem
        {
            Text = "Start with Windows",
            IsChecked = AutoStartService.IsEnabled(),
        };
        autoStart.Command = new RelayCommand(() => ToggleAutoStart(autoStart));

        var exit = new MenuFlyoutItem
        {
            Text = "Exit",
            Command = new RelayCommand(ExitApp),
        };

        var flyout = new MenuFlyout();
        flyout.Items.Add(autoStart);
        flyout.Items.Add(new MenuFlyoutSeparator());
        flyout.Items.Add(exit);
        return flyout;
    }

    private void ToggleAutoStart(ToggleMenuFlyoutItem item)
    {
        // The native menu does not auto-toggle IsChecked, so it still holds the state
        // from before the click; flip it here only after the service call succeeds.
        try
        {
            if (item.IsChecked)
            {
                AutoStartService.Disable();
                item.IsChecked = false;
            }
            else
            {
                AutoStartService.Enable();
                item.IsChecked = true;
            }
        }
        catch (Exception ex)
        {
            OnErrorMessage(this, $"Autostart failed: {ex.Message}");
        }
    }

    // ---- Window show / hide ------------------------------------------------
    private void ShowMainWindow()
    {
        AppWindow.Show();
        if (AppWindow.Presenter is OverlappedPresenter presenter &&
            presenter.State == OverlappedPresenterState.Minimized)
        {
            presenter.Restore();
        }

        Activate();
    }

    private void OnAppWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_exiting)
        {
            return; // real shutdown — let it close
        }

        args.Cancel = true; // clicking X hides to the tray instead of quitting
        sender.Hide();
    }

    private void OnAppWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (args.DidPresenterChange &&
            sender.Presenter is OverlappedPresenter presenter &&
            presenter.State == OverlappedPresenterState.Minimized)
        {
            sender.Hide(); // minimize goes to the tray
        }
    }

    private void ExitApp()
    {
        _exiting = true;
        _titleTimer?.Stop();
        _trayTimer?.Stop();
        DisposeTrays();
        _client.Stop();
        _hardware.Dispose();
        Application.Current.Exit();
    }

    private void DisposeTrays()
    {
        for (var i = 0; i < 2; i++)
        {
            _trays[i]?.Dispose();
            _trays[i] = null;
        }

        DestroyTrayIconImages();
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        // Backstop cleanup (idempotent). Stopping the services below still raises their
        // events, so mark the window as gone before any callback can touch it.
        _exiting = true;
        _titleTimer?.Stop();
        _trayTimer?.Stop();
        DisposeTrays();
        _client.Stop();
        _hardware.Dispose();
    }

    // ---- Readout -----------------------------------------------------------
    private void OnSample(object? sender, HardwareSample s) =>
        _dispatcher.TryEnqueue(() =>
        {
            if (_exiting)
            {
                return; // the window may already be closed — touching it throws
            }

            SetCell(CpuLoad, Value(s.CpuLoad), LoadBrush);
            SetCell(CpuTemp, Value(s.CpuTemp), TempBrush);
            SetCell(CpuMem, Value(s.RamUsed), MemBrush);
            SetCell(GpuLoad, Value(s.GpuLoad), LoadBrush);
            SetCell(GpuTemp, Value(s.GpuTemp), TempBrush);
            SetCell(GpuMem, Value(s.VramUsed), MemBrush);

            s.ToPayload().CopyTo(_trayValues, 0);
            if (_trays[0] is { } cpuTray)
            {
                cpuTray.ToolTipText = $"CPU {Pct(s.CpuLoad)} {Deg(s.CpuTemp)} {Pct(s.RamUsed)}";
            }

            if (_trays[1] is { } gpuTray)
            {
                gpuTray.ToolTipText = $"GPU {Pct(s.GpuLoad)} {Deg(s.GpuTemp)} {Pct(s.VramUsed)}";
            }

            // The tray icons are deliberately NOT redrawn here: the value shown in a
            // cycle slot is frozen for the whole slot — only the timer tick rerenders
            // them, picking the latest sample at that moment.

            // While connected the chart is fed from delivered frames (OnFrameSent);
            // local samples feed it only in the disconnected fallback.
            if (!_mirror)
            {
                PushHistory(s.ToPayload());
                if (_view != 0)
                {
                    RedrawChart();
                }
            }
        });

    // ---- Charts (hybrid source + view cycling + drawing) --------------------
    // Connected: exact mirror of the device — history rebuilt from zero at connect and fed
    // only with frames actually written over BLE (a dropped write is missing on both ends).
    // Disconnected: history is kept and continues from local samples (same data source).
    private void OnConnectedChanged(object? sender, bool connected) =>
        _dispatcher.TryEnqueue(() =>
        {
            if (_exiting)
            {
                return;
            }

            _mirror = connected;
            if (connected)
            {
                ResetHistory(); // the device starts its chart from zero at connect
                if (_view != 0)
                {
                    RedrawChart();
                }
            }
        });

    private void OnFrameSent(object? sender, byte[] frame) =>
        _dispatcher.TryEnqueue(() =>
        {
            if (_exiting || !_mirror)
            {
                return;
            }

            PushHistory(frame);
            if (_view != 0)
            {
                RedrawChart();
            }
        });

    // Frame layout matches the metric order: [cpuLoad, cpuTemp, ram, gpuLoad, gpuTemp, vram].
    private void PushHistory(byte[] frame)
    {
        for (int m = 0; m < 6; m++)
        {
            _hist[m][_histHead] = frame[m];
        }

        _histHead = (_histHead + 1) % HistLen;
        if (_histCount < HistLen)
        {
            _histCount++;
        }
    }

    private void ResetHistory()
    {
        _histCount = 0;
        _histHead = 0;
    }

    // Left click anywhere in the body advances the view, like the device BOOT button.
    private void OnBodyPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        var kind = e.GetCurrentPoint(Body).Properties.PointerUpdateKind;
        if (kind != PointerUpdateKind.LeftButtonPressed)
        {
            return;
        }

        _view = (_view + 1) % ViewCount;
        ApplyView();
    }

    private void ApplyView()
    {
        TableView.Visibility = _view == 0 ? Visibility.Visible : Visibility.Collapsed;
        ChartView.Visibility = _view == 0 ? Visibility.Collapsed : Visibility.Visible;
        if (_view != 0)
        {
            ChartTitle.Text = _view == 1 ? "CPU" : "GPU";
            RedrawChart();
        }
    }

    private void OnPlotSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_view != 0)
        {
            RedrawChart();
        }
    }

    // Redraws the current chart from scratch: axes, axis labels and the three metric
    // lines (LOAD/TEMP/MEM) on a shared fixed 0..100 scale. Cheap at 1 sample/s.
    private void RedrawChart()
    {
        PlotCanvas.Children.Clear();

        double w = PlotCanvas.ActualWidth;
        double h = PlotCanvas.ActualHeight;
        if (w < 60 || h < 40)
        {
            return; // not laid out yet
        }

        // Plot area margins: left for the Y labels, bottom for the X label.
        double plotL = 34, plotT = 8, plotR = w - 4, plotB = h - 20;

        PlotCanvas.Children.Add(MakeLine(plotL, plotT, plotL, plotB)); // Y axis
        PlotCanvas.Children.Add(MakeLine(plotL, plotB, plotR, plotB)); // X axis
        AddAxisLabel("100", 0, plotT - 8, plotL - 6);  // Y max
        AddAxisLabel("0", 0, plotB - 8, plotL - 6);    // Y min
        AddAxisLabel("t", plotR - 8, plotB + 4, 12);   // X axis

        int chartBase = (_view - 1) * 3; // first of this chart's three metric indices
        for (int k = 0; k < 3; k++)
        {
            AddMetricLine(chartBase + k, MetricBrushes[k], plotL, plotT, plotR, plotB);
        }
    }

    private static Line MakeLine(double x1, double y1, double x2, double y2) =>
        new() { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = AxisBrush, StrokeThickness = 1 };

    private void AddAxisLabel(string text, double left, double top, double width)
    {
        var label = new TextBlock
        {
            Text = text,
            FontSize = 13,
            Foreground = NaBrush,
            Width = width,
            TextAlignment = TextAlignment.Right,
        };
        Canvas.SetLeft(label, left);
        Canvas.SetTop(label, top);
        PlotCanvas.Children.Add(label);
    }

    // Polyline over the last _histCount samples (oldest at left). Y scale is fixed 0..100;
    // NA samples break the line (gap), a lone sample after a gap becomes a dot. Fixed step
    // per sample: the plot fills from the left and spans full width at HistLen samples.
    private void AddMetricLine(int metricIdx, SolidColorBrush stroke,
                               double plotL, double plotT, double plotR, double plotB)
    {
        Polyline? line = null;

        void EndSegment()
        {
            if (line != null && line.Points.Count == 1)
            {
                // A single-point polyline renders nothing — draw the lone sample as a dot.
                var p = line.Points[0];
                PlotCanvas.Children.Remove(line);
                var dot = new Ellipse { Width = 3, Height = 3, Fill = stroke };
                Canvas.SetLeft(dot, p.X - 1.5);
                Canvas.SetTop(dot, p.Y - 1.5);
                PlotCanvas.Children.Add(dot);
            }

            line = null;
        }

        for (int i = 0; i < _histCount; i++)
        {
            int slot = (_histHead - _histCount + i + HistLen) % HistLen;
            byte v = _hist[metricIdx][slot];
            if (v == Na)
            {
                EndSegment(); // gap for missing sample
                continue;
            }

            if (v > 100)
            {
                v = 100;
            }

            double x = plotL + (plotR - plotL) * i / (HistLen - 1);
            double y = plotB - (plotB - plotT) * v / 100.0;
            if (line == null)
            {
                line = new Polyline { Stroke = stroke, StrokeThickness = 2 };
                PlotCanvas.Children.Add(line);
            }

            line.Points.Add(new Point(x, y));
        }

        EndSegment();
    }

    // Connection status lives in the title bar, not the body, as an emoji prefix. Title
    // covers the taskbar/Alt-Tab label; TitleText is the visible custom title bar.
    private void OnBleStatus(object? sender, BleStatus status) =>
        ApplyTitleStatus(status.Kind, status.Detail);

    // Hardware/autostart errors share the title's error surface.
    private void OnErrorMessage(object? sender, string message) =>
        ApplyTitleStatus(BleStatusKind.Error, message);

    private void ApplyTitleStatus(BleStatusKind kind, string? detail) =>
        _dispatcher.TryEnqueue(() =>
        {
            if (_exiting)
            {
                return; // the window may already be closed — touching it throws
            }

            _titleKind = kind;
            _titleDetail = detail;

            // Waiting states animate (two frames flipped by the timer); the rest are static.
            bool animated = kind is BleStatusKind.Scanning
                or BleStatusKind.Connecting
                or BleStatusKind.Discovering;
            if (animated)
            {
                _titleTimer ??= CreateTitleTimer();
                if (!_titleTimer.IsRunning)
                {
                    _titleTimer.Start();
                }
            }
            else
            {
                _titleTimer?.Stop();
            }

            RenderTitle();
        });

    private DispatcherQueueTimer CreateTitleTimer()
    {
        var timer = _dispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(600);
        timer.Tick += (_, _) =>
        {
            if (_exiting)
            {
                return;
            }

            _titleFrame = !_titleFrame;
            RenderTitle();
        };
        return timer;
    }

    private void RenderTitle()
    {
        string emoji = _titleKind switch
        {
            BleStatusKind.Scanning => _titleFrame ? "🔎" : "🔍",
            BleStatusKind.Connecting or BleStatusKind.Discovering => _titleFrame ? "⌛" : "⏳",
            BleStatusKind.Connected => "🟢",
            BleStatusKind.Disconnected => "🔴",
            BleStatusKind.Error => "⚠️",
            _ => "💤", // Idle
        };

        // Errors are the one state that keeps its diagnostic text next to the emoji.
        string text = _titleKind == BleStatusKind.Error && !string.IsNullOrEmpty(_titleDetail)
            ? $"{emoji} DeviceMonitor — {_titleDetail}"
            : $"{emoji} DeviceMonitor";
        Title = text;
        TitleText.Text = text;
    }

    private static void SetCell(TextBlock cell, string text, SolidColorBrush color)
    {
        cell.Text = text;
        cell.Foreground = text == "--" ? NaBrush : color;
    }

    // Units live in the column headers, so the cell shows the bare number (or "--" for 0xFF,
    // matching the device screen).
    private static string Value(byte v) => v == 0xFF ? "--" : $"{v}";

    // Tooltip variants with units ("--" stays bare — no unit on a missing reading).
    private static string Pct(byte v) => v == 0xFF ? "--" : $"{v}%";
    private static string Deg(byte v) => v == 0xFF ? "--" : $"{v}°C";
}
