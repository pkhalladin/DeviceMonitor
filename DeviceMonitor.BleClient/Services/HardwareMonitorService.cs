using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Text;
using System.Threading;
using LibreHardwareMonitor.Hardware;

namespace DeviceMonitor.BleClient.Services;

/// <summary>One poll of the six metrics. 255 (0xFF) means "no reading".</summary>
public sealed class HardwareSample
{
    public byte CpuLoad { get; init; } = 0xFF;
    public byte CpuTemp { get; init; } = 0xFF;
    public byte RamUsed { get; init; } = 0xFF;
    public byte GpuLoad { get; init; } = 0xFF;
    public byte GpuTemp { get; init; } = 0xFF;
    public byte VramUsed { get; init; } = 0xFF;

    public byte[] ToPayload() =>
        new[] { CpuLoad, CpuTemp, RamUsed, GpuLoad, GpuTemp, VramUsed };
}

/// <summary>
/// Reads CPU/GPU load, temperature and memory usage via LibreHardwareMonitorLib and
/// keeps a current 6-byte payload for the BLE client. The library is NOT thread-safe,
/// so all Update()/reads happen on a single background timer with a re-entrancy guard.
/// CPU temperature needs an elevated process (ring0 driver).
/// </summary>
public sealed class HardwareMonitorService : IDisposable
{
    private const byte NA = 0xFF;

    private readonly object _sync = new();
    private readonly UpdateVisitor _visitor = new();
    private readonly List<ISensor> _cpuTempCandidates = new();

    private Computer? _computer;
    private Timer? _timer;
    private int _polling; // re-entrancy guard (0 = idle, 1 = running)
    private bool _dumped; // one-time sensor-tree diagnostic

    private ISensor? _cpuLoad, _ramLoad, _ramUsed, _ramAvail;
    private ISensor? _gpuLoad, _gpuTemp, _gpuMemLoad, _gpuMemUsed, _gpuMemTotal;

    private byte[] _payload = { NA, NA, NA, NA, NA, NA };

    public event EventHandler<HardwareSample>? Updated;
    public event EventHandler<string>? Error;

    /// <summary>Thread-safe copy of the latest 6-byte frame.</summary>
    public byte[] CurrentPayload
    {
        get { lock (_sync) { return (byte[])_payload.Clone(); } }
    }

    public void Start()
    {
        if (_computer is not null)
        {
            return;
        }

        if (!IsElevated())
        {
            Error?.Invoke(this, "Not elevated — CPU temperature unavailable (run as admin)");
        }

        try
        {
            _computer = new Computer
            {
                IsCpuEnabled = true,
                IsGpuEnabled = true,
                IsMemoryEnabled = true,
                IsMotherboardEnabled = true, // Super-I/O CPU temp as a fallback source
            };
            _computer.Open();
            _timer = new Timer(Poll, null, TimeSpan.Zero, TimeSpan.FromSeconds(1));
        }
        catch (Exception ex)
        {
            Error?.Invoke(this, $"Sensors unavailable: {ex.Message}");
        }
    }

    private void Poll(object? state)
    {
        if (Interlocked.Exchange(ref _polling, 1) == 1)
        {
            return; // previous poll still running — skip this tick
        }

        try
        {
            Computer? computer = _computer;
            if (computer is null)
            {
                return;
            }

            computer.Accept(_visitor); // refresh every hardware + subhardware
            LocateSensors(computer);   // cheap; also picks up late-appearing hardware

            if (!_dumped)
            {
                _dumped = true;
                DumpDiagnostics(computer);
            }

            var sample = new HardwareSample
            {
                CpuLoad = ReadPercent(_cpuLoad),
                CpuTemp = ReadCpuTemp(),
                RamUsed = ReadRam(),
                GpuLoad = ReadPercent(_gpuLoad),
                GpuTemp = ReadTemp(_gpuTemp),
                VramUsed = ReadVram(),
            };

            byte[] payload = sample.ToPayload();
            lock (_sync)
            {
                _payload = payload;
            }

            Updated?.Invoke(this, sample);
        }
        catch (Exception ex)
        {
            Error?.Invoke(this, $"Sensor read error: {ex.Message}");
        }
        finally
        {
            Interlocked.Exchange(ref _polling, 0);
        }
    }

    private void LocateSensors(Computer computer)
    {
        _cpuTempCandidates.Clear();
        var gpus = new List<IHardware>();

        foreach (IHardware hw in computer.Hardware)
        {
            switch (hw.HardwareType)
            {
                case HardwareType.Cpu:
                    _cpuLoad = FindSensor(hw, SensorType.Load, "CPU Total");
                    CollectCpuTemps(hw);
                    break;
                case HardwareType.Memory:
                    LocateMemory(hw);
                    break;
                case HardwareType.Motherboard:
                    CollectMotherboardCpuTemps(hw);
                    break;
                case HardwareType.GpuNvidia:
                case HardwareType.GpuAmd:
                case HardwareType.GpuIntel:
                    gpus.Add(hw);
                    break;
            }
        }

        IHardware? gpu = SelectGpu(gpus);
        if (gpu is not null)
        {
            _gpuLoad = FindSensor(gpu, SensorType.Load, "GPU Core");
            _gpuTemp = FindSensor(gpu, SensorType.Temperature, "GPU Core");
            _gpuMemLoad = FindSensor(gpu, SensorType.Load, "GPU Memory");
            _gpuMemUsed = FindSensor(gpu, SensorType.SmallData, "GPU Memory Used");
            _gpuMemTotal = FindSensor(gpu, SensorType.SmallData, "GPU Memory Total");
        }
    }

    // Pick the discrete GPU: the one with the most dedicated VRAM (an integrated GPU
    // reports only a small shared pool), preferring a dedicated vendor over Intel on ties.
    private static IHardware? SelectGpu(List<IHardware> gpus)
    {
        IHardware? best = null;
        float bestScore = float.MinValue;

        foreach (IHardware gpu in gpus)
        {
            float vram = FindSensor(gpu, SensorType.SmallData, "GPU Memory Total")?.Value ?? 0f;
            float score = vram - (gpu.HardwareType == HardwareType.GpuIntel ? 1f : 0f);
            if (score > bestScore)
            {
                bestScore = score;
                best = gpu;
            }
        }

        return best;
    }

    // Gathers CPU temperature sensors in priority order (naming differs by vendor and
    // generation), then any remaining ones. ReadCpuTemp() later picks the first with a value.
    private void CollectCpuTemps(IHardware cpu)
    {
        string[] order =
        {
            "CPU Package",        // Intel
            "Core (Tctl/Tdie)",   // AMD Zen 2+
            "Core (Tdie)", "Core (Tctl)", // older AMD
            "Core Average", "Core Max",   // Intel fallbacks
        };

        foreach (string name in order)
        {
            ISensor? s = FindSensor(cpu, SensorType.Temperature, name);
            if (s is not null && !_cpuTempCandidates.Contains(s))
            {
                _cpuTempCandidates.Add(s);
            }
        }

        foreach (ISensor s in cpu.Sensors)
        {
            if (s.SensorType == SensorType.Temperature && !_cpuTempCandidates.Contains(s))
            {
                _cpuTempCandidates.Add(s);
            }
        }
    }

    private void CollectMotherboardCpuTemps(IHardware motherboard)
    {
        foreach (IHardware sub in motherboard.SubHardware)
        {
            foreach (ISensor s in sub.Sensors)
            {
                if (s.SensorType == SensorType.Temperature &&
                    s.Name.Contains("CPU", StringComparison.OrdinalIgnoreCase) &&
                    !_cpuTempCandidates.Contains(s))
                {
                    _cpuTempCandidates.Add(s);
                }
            }
        }
    }

    private void LocateMemory(IHardware memory)
    {
        // Two Memory nodes exist: "Total Memory" (physical RAM) and "Virtual Memory"
        // (commit charge). Prefer physical RAM, and never overwrite a good match with a
        // null from the other node.
        bool isPhysical = memory.Name.Contains("Total", StringComparison.OrdinalIgnoreCase);
        if (!isPhysical && _ramLoad is not null)
        {
            return;
        }

        _ramLoad = FindSensor(memory, SensorType.Load, "Memory") ?? _ramLoad;
        _ramUsed = FindSensor(memory, SensorType.Data, "Memory Used") ?? _ramUsed;
        _ramAvail = FindSensor(memory, SensorType.Data, "Memory Available") ?? _ramAvail;
    }

    private static ISensor? FindSensor(IHardware hw, SensorType type, string name)
    {
        foreach (ISensor s in hw.Sensors)
        {
            if (s.SensorType == type && string.Equals(s.Name, name, StringComparison.Ordinal))
            {
                return s;
            }
        }

        return null;
    }

    private byte ReadCpuTemp()
    {
        foreach (ISensor s in _cpuTempCandidates)
        {
            if (s.Value is float v && v > 0f)
            {
                return (byte)Math.Clamp((int)MathF.Round(v), 0, 254);
            }
        }

        return NA;
    }

    private static byte ReadPercent(ISensor? s) =>
        s?.Value is float v && v >= 0f
            ? (byte)Math.Clamp((int)MathF.Round(v), 0, 100)
            : NA;

    private static byte ReadTemp(ISensor? s) =>
        s?.Value is float v && v > 0f
            ? (byte)Math.Clamp((int)MathF.Round(v), 0, 254)
            : NA;

    private byte ReadRam()
    {
        if (_ramLoad?.Value is float pct && pct >= 0f)
        {
            return (byte)Math.Clamp((int)MathF.Round(pct), 0, 100);
        }

        // Fallback: compute occupancy from the used/available data sensors (GB).
        if (_ramUsed?.Value is float used && _ramAvail?.Value is float avail && used + avail > 0f)
        {
            return (byte)Math.Clamp((int)MathF.Round(100f * used / (used + avail)), 0, 100);
        }

        return NA;
    }

    private byte ReadVram()
    {
        // "Used %" is what the table shows, so compute occupancy from used/total first;
        // the "GPU Memory" Load sensor (controller utilisation on some GPUs) is a fallback.
        if (_gpuMemUsed?.Value is float used && _gpuMemTotal?.Value is float total && total > 0f)
        {
            return (byte)Math.Clamp((int)MathF.Round(100f * used / total), 0, 100);
        }

        if (_gpuMemLoad?.Value is float pct && pct >= 0f)
        {
            return (byte)Math.Clamp((int)MathF.Round(pct), 0, 100);
        }

        return NA;
    }

    private static bool IsElevated()
    {
        try
        {
            using WindowsIdentity identity = WindowsIdentity.GetCurrent();
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }
        catch
        {
            return false;
        }
    }

    // One-time dump of the full hardware/sensor tree so we can see exactly what
    // LibreHardwareMonitor exposes (and whether the process is elevated). Written to
    // %TEMP%\DeviceMonitor-sensors.txt and the debugger Output window.
    private void DumpDiagnostics(Computer computer)
    {
        try
        {
            var sb = new StringBuilder();
            sb.AppendLine($"Elevated: {IsElevated()}");
            sb.AppendLine($"Matched  cpuLoad={_cpuLoad?.Name ?? "null"}  ramLoad={_ramLoad?.Name ?? "null"}");
            sb.AppendLine($"         gpuLoad={_gpuLoad?.Name ?? "null"}  gpuTemp={_gpuTemp?.Name ?? "null"}");
            sb.AppendLine($"CPU temp candidates ({_cpuTempCandidates.Count}):");
            foreach (ISensor s in _cpuTempCandidates)
            {
                sb.AppendLine($"    \"{s.Name}\" = {Format(s.Value)}");
            }

            sb.AppendLine();
            foreach (IHardware hw in computer.Hardware)
            {
                sb.AppendLine($"[{hw.HardwareType}] {hw.Name}");
                foreach (ISensor s in hw.Sensors)
                {
                    sb.AppendLine($"    {s.SensorType,-12} \"{s.Name}\" = {Format(s.Value)}");
                }

                foreach (IHardware sub in hw.SubHardware)
                {
                    sb.AppendLine($"    SUB [{sub.HardwareType}] {sub.Name}");
                    foreach (ISensor s in sub.Sensors)
                    {
                        sb.AppendLine($"        {s.SensorType,-12} \"{s.Name}\" = {Format(s.Value)}");
                    }
                }
            }

            string text = sb.ToString();
            Debug.WriteLine(text);
            string path = Path.Combine(Path.GetTempPath(), "DeviceMonitor-sensors.txt");
            File.WriteAllText(path, text);
        }
        catch (Exception ex)
        {
            Error?.Invoke(this, $"Dump failed: {ex.Message}");
        }

        static string Format(float? v) => v.HasValue ? v.Value.ToString("0.0") : "null";
    }

    public void Dispose()
    {
        _timer?.Dispose();
        _timer = null;
        try
        {
            _computer?.Close();
        }
        catch
        {
            // Best-effort driver unload on shutdown.
        }

        _computer = null;
    }

    /// <summary>Refreshes each hardware node (and its sub-hardware) per the LHM README.</summary>
    private sealed class UpdateVisitor : IVisitor
    {
        public void VisitComputer(IComputer computer) => computer.Traverse(this);

        public void VisitHardware(IHardware hardware)
        {
            hardware.Update();
            foreach (IHardware sub in hardware.SubHardware)
            {
                sub.Accept(this);
            }
        }

        public void VisitSensor(ISensor sensor) { }

        public void VisitParameter(IParameter parameter) { }
    }
}
