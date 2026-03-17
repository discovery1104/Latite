using System.Diagnostics;
using System.IO.Pipes;
using System.IO;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Threading;

namespace OmotiMusicHelper;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        using var mutex = new Mutex(true, "OmotiMusicHelper.Instance", out bool createdNew);
        if (!createdNew)
        {
            return;
        }

        var host = new MusicHost(Dispatcher.CurrentDispatcher);
        host.Start();
        Dispatcher.Run();
    }
}

internal sealed class MusicHost
{
    private const string PipeName = "OmotiMusicHelperPipe_v1";
    private static readonly TimeSpan ClientIdleTimeout = TimeSpan.FromSeconds(15);
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly Dispatcher _dispatcher;
    private readonly MediaPlayer _player = new();
    private readonly object _stateLock = new();

    private int _ownerPid;
    private string _state = "stopped";
    private string _currentPath = string.Empty;
    private string _lastError = string.Empty;
    private double _volume = 1.0;
    private DateTime _lastClientContactUtc = DateTime.UtcNow;

    public MusicHost(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        _player.MediaOpened += (_, _) =>
        {
            lock (_stateLock)
            {
                if (_state != "paused")
                {
                    _state = "playing";
                }
                _lastError = string.Empty;
            }
        };
        _player.MediaEnded += (_, _) =>
        {
            lock (_stateLock)
            {
                _state = "stopped";
            }
        };
        _player.MediaFailed += (_, args) =>
        {
            lock (_stateLock)
            {
                _state = "error";
                _lastError = args.ErrorException?.Message ?? "Unknown media error";
            }
        };
    }

    public void Start()
    {
        Task.Run(ServerLoopAsync);
        Task.Run(LifetimeLoopAsync);
    }

    private async Task ServerLoopAsync()
    {
        while (true)
        {
            using var pipe = new NamedPipeServerStream(
                PipeName,
                PipeDirection.InOut,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            await pipe.WaitForConnectionAsync().ConfigureAwait(false);

            using var reader = new StreamReader(pipe, leaveOpen: true);
            using var writer = new StreamWriter(pipe) { AutoFlush = true };

            string? requestLine = await reader.ReadLineAsync().ConfigureAwait(false);
            if (string.IsNullOrWhiteSpace(requestLine))
            {
                await writer.WriteLineAsync("{\"ok\":false,\"error\":\"empty request\"}").ConfigureAwait(false);
                continue;
            }

            string response;
            try
            {
                using var doc = JsonDocument.Parse(requestLine);
                response = await _dispatcher.InvokeAsync(() => HandleCommand(doc.RootElement)).Task.ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                response = JsonSerializer.Serialize(new
                {
                    ok = false,
                    state = "error",
                    error = ex.Message
                }, JsonOptions);
            }

            await writer.WriteLineAsync(response).ConfigureAwait(false);
        }
    }

    private string HandleCommand(JsonElement request)
    {
        UpdateClientBinding(request);

        string command = request.TryGetProperty("command", out JsonElement cmdElement)
            ? cmdElement.GetString() ?? string.Empty
            : string.Empty;

        return command switch
        {
            "ping" => SerializeStatus(ok: true),
            "status" => SerializeStatus(ok: true),
            "play" => HandlePlay(request),
            "pause" => HandlePause(),
            "resume" => HandleResume(),
            "stop" => HandleStop(),
            "shutdown" => HandleShutdown(),
            "seek" => HandleSeek(request),
            "volume" => HandleVolume(request),
            _ => JsonSerializer.Serialize(new { ok = false, state = "error", error = $"Unknown command: {command}" }, JsonOptions)
        };
    }

    private void UpdateClientBinding(JsonElement request)
    {
        lock (_stateLock)
        {
            _lastClientContactUtc = DateTime.UtcNow;
            if (request.TryGetProperty("ownerPid", out JsonElement ownerPidElement) &&
                ownerPidElement.ValueKind == JsonValueKind.Number &&
                ownerPidElement.TryGetInt32(out int ownerPid) &&
                ownerPid > 0)
            {
                _ownerPid = ownerPid;
            }
        }
    }

    private async Task LifetimeLoopAsync()
    {
        while (true)
        {
            await Task.Delay(1000).ConfigureAwait(false);

            bool shouldShutdown = false;
            lock (_stateLock)
            {
                if (_ownerPid > 0 && IsProcessGone(_ownerPid))
                {
                    shouldShutdown = true;
                }
                else if ((DateTime.UtcNow - _lastClientContactUtc) > ClientIdleTimeout)
                {
                    shouldShutdown = true;
                }
            }

            if (!shouldShutdown)
            {
                continue;
            }

            await _dispatcher.InvokeAsync(ShutdownFromClientLoss).Task.ConfigureAwait(false);
            break;
        }
    }

    private static bool IsProcessGone(int pid)
    {
        try
        {
            using Process process = Process.GetProcessById(pid);
            return process.HasExited;
        }
        catch
        {
            return true;
        }
    }

    private void ShutdownFromClientLoss()
    {
        _player.Stop();
        _player.Close();
        lock (_stateLock)
        {
            _state = "stopped";
            _currentPath = string.Empty;
            _lastError = string.Empty;
        }
        _dispatcher.BeginInvokeShutdown(DispatcherPriority.Background);
    }

    private string HandlePlay(JsonElement request)
    {
        if (!request.TryGetProperty("path", out JsonElement pathElement))
        {
            return JsonSerializer.Serialize(new { ok = false, state = "error", error = "path missing" }, JsonOptions);
        }

        string? path = pathElement.GetString();
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
        {
            return JsonSerializer.Serialize(new { ok = false, state = "error", error = "file not found" }, JsonOptions);
        }

        double volume = request.TryGetProperty("volume", out JsonElement volumeElement)
            ? Math.Clamp(volumeElement.GetDouble(), 0.0, 1.0)
            : _volume;

        _player.Open(new Uri(path, UriKind.Absolute));
        _player.Volume = volume;
        _player.Play();

        lock (_stateLock)
        {
            _state = "opening";
            _currentPath = path;
            _lastError = string.Empty;
            _volume = volume;
        }

        return SerializeStatus(ok: true);
    }

    private string HandlePause()
    {
        _player.Pause();
        lock (_stateLock)
        {
            _state = "paused";
        }
        return SerializeStatus(ok: true);
    }

    private string HandleResume()
    {
        _player.Play();
        lock (_stateLock)
        {
            _state = "playing";
        }
        return SerializeStatus(ok: true);
    }

    private string HandleStop()
    {
        _player.Stop();
        _player.Close();
        lock (_stateLock)
        {
            _state = "stopped";
            _currentPath = string.Empty;
            _lastError = string.Empty;
        }
        return SerializeStatus(ok: true);
    }

    private string HandleShutdown()
    {
        string response = SerializeStatus(ok: true);
        ShutdownFromClientLoss();
        return response;
    }

    private string HandleSeek(JsonElement request)
    {
        int targetMs = request.TryGetProperty("ms", out JsonElement msElement)
            ? Math.Max(0, msElement.GetInt32())
            : 0;

        _player.Position = TimeSpan.FromMilliseconds(targetMs);
        return SerializeStatus(ok: true);
    }

    private string HandleVolume(JsonElement request)
    {
        double volume = request.TryGetProperty("volume", out JsonElement volumeElement)
            ? Math.Clamp(volumeElement.GetDouble(), 0.0, 1.0)
            : _volume;

        _player.Volume = volume;
        lock (_stateLock)
        {
            _volume = volume;
        }

        return SerializeStatus(ok: true);
    }

    private string SerializeStatus(bool ok)
    {
        lock (_stateLock)
        {
            int durationMs = _player.NaturalDuration.HasTimeSpan
                ? (int)_player.NaturalDuration.TimeSpan.TotalMilliseconds
                : -1;

            return JsonSerializer.Serialize(new
            {
                ok,
                state = _state,
                path = _currentPath,
                error = _lastError,
                positionMs = (int)_player.Position.TotalMilliseconds,
                durationMs,
                volume = _volume
            }, JsonOptions);
        }
    }
}
