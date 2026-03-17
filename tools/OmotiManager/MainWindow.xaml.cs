using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Web;
using System.Windows;
using System.Windows.Data;
using Microsoft.Win32;
using OmotiManager.Models;

namespace OmotiManager;

public partial class MainWindow : Window
{
    private static readonly string[] SupportedExtensions = [".mp3", ".wav", ".ogg", ".flac", ".m4a"];
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private const string BundledYtDlpResourceName = "OmotiManager.Bundled.yt-dlp.exe";
    private const string BundledFfmpegResourceName = "OmotiManager.Bundled.ffmpeg.exe";
    private const long BundledYtDlpSize = 18_408_128;
    private const long BundledFfmpegSize = 99_264_000;

    private readonly ObservableCollection<TrackItem> _tracks = [];
    private readonly ICollectionView _trackView;
    private ManagerSettings _settings = new();
    private Process? _downloadProcess;

    private static string OmotiRootPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Packages", "Microsoft.MinecraftUWP_8wekyb3d8bbwe", "RoamingState", "Omoti");

    private static string MusicFolderPath => Path.Combine(OmotiRootPath, "Music");
    private static string ToolsFolderPath => Path.Combine(OmotiRootPath, "Tools");
    private static string SettingsFilePath => Path.Combine(OmotiRootPath, "ManagerSettings.json");
    private static string PlaylistsFilePath => Path.Combine(OmotiRootPath, "MusicPlaylists.json");
    private static string DefaultCookiesFilePath => Path.Combine(OmotiRootPath, "cookies.txt");

    public MainWindow()
    {
        InitializeComponent();
        LibraryListView.ItemsSource = _tracks;
        _trackView = CollectionViewSource.GetDefaultView(_tracks);
        _trackView.Filter = FilterTrack;
        Loaded += MainWindow_Loaded;
        Closing += MainWindow_Closing;
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        await InitializeAsync();
    }

    private async Task InitializeAsync()
    {
        Directory.CreateDirectory(OmotiRootPath);
        Directory.CreateDirectory(MusicFolderPath);
        Directory.CreateDirectory(ToolsFolderPath);

        RootFolderTextBlock.Text = OmotiRootPath;
        MusicFolderTextBlock.Text = MusicFolderPath;

        await LoadSettingsAsync();
        EnsureBundledYtDlp();
        EnsureBundledFfmpeg();
        DetectTools(saveSettings: false);
        ApplySettingsToUi();
        RefreshLibrary();
        UpdateDownloadState("Ready", "Paste a YouTube URL and download straight into Omoti Music.");
        Log("Omoti Manager ready.");
    }

    private async Task LoadSettingsAsync()
    {
        if (!File.Exists(SettingsFilePath)) return;
        try
        {
            var json = await File.ReadAllTextAsync(SettingsFilePath);
            _settings = JsonSerializer.Deserialize<ManagerSettings>(json, JsonOptions) ?? new ManagerSettings();
        }
        catch (Exception ex)
        {
            Log($"Could not read settings: {ex.Message}");
            _settings = new ManagerSettings();
        }
    }

    private async Task SaveSettingsAsync()
    {
        try
        {
            _settings.AllowPlaylistDownloads = AllowPlaylistCheckBox.IsChecked == true;
            Directory.CreateDirectory(OmotiRootPath);
            var json = JsonSerializer.Serialize(_settings, JsonOptions);
            await File.WriteAllTextAsync(SettingsFilePath, json);
        }
        catch (Exception ex)
        {
            Log($"Could not save settings: {ex.Message}");
        }
    }

    private void ApplySettingsToUi()
    {
        YtDlpPathTextBox.Text = _settings.YtDlpPath ?? string.Empty;
        FfmpegPathTextBox.Text = _settings.FfmpegPath ?? string.Empty;
        AllowPlaylistCheckBox.IsChecked = _settings.AllowPlaylistDownloads;
        UpdatePipelineStatus();
    }

    private void DetectTools(bool saveSettings = true)
    {
        EnsureBundledYtDlp();
        EnsureBundledFfmpeg();
        _settings.YtDlpPath = ResolveToolPath(_settings.YtDlpPath, "yt-dlp.exe");
        _settings.FfmpegPath = ResolveToolPath(_settings.FfmpegPath, "ffmpeg.exe");
        ApplySettingsToUi();
        if (saveSettings)
        {
            _ = SaveSettingsAsync();
        }
    }

    private static string? ResolveToolPath(string? configuredPath, string fileName)
    {
        if (!string.IsNullOrWhiteSpace(configuredPath) && File.Exists(configuredPath))
        {
            return Path.GetFullPath(configuredPath);
        }

        foreach (var candidate in new[]
        {
            Path.Combine(ToolsFolderPath, fileName),
            Path.Combine(AppContext.BaseDirectory, fileName)
        })
        {
            if (File.Exists(candidate)) return Path.GetFullPath(candidate);
        }

        var pathEnv = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(pathEnv)) return null;

        foreach (var entry in pathEnv.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            try
            {
                var candidate = Path.Combine(entry, fileName);
                if (File.Exists(candidate)) return Path.GetFullPath(candidate);
            }
            catch
            {
            }
        }

        return null;
    }

    private bool EnsureBundledYtDlp(bool overwrite = false)
    {
        try
        {
            Directory.CreateDirectory(ToolsFolderPath);
            var destination = Path.Combine(ToolsFolderPath, "yt-dlp.exe");
            bool needsWrite = overwrite || !File.Exists(destination) || new FileInfo(destination).Length != BundledYtDlpSize;
            if (!needsWrite)
            {
                if (string.IsNullOrWhiteSpace(_settings.YtDlpPath) || !File.Exists(_settings.YtDlpPath))
                {
                    _settings.YtDlpPath = destination;
                }
                return true;
            }

            using var input = Assembly.GetExecutingAssembly().GetManifestResourceStream(BundledYtDlpResourceName);
            if (input is null)
            {
                Log("Bundled yt-dlp resource was not found inside the app.");
                return false;
            }

            using var output = File.Create(destination);
            input.CopyTo(output);
            _settings.YtDlpPath = destination;
            return true;
        }
        catch (Exception ex)
        {
            Log($"Could not restore bundled yt-dlp: {ex.Message}");
            return false;
        }
    }

    private bool EnsureBundledFfmpeg(bool overwrite = false)
    {
        try
        {
            Directory.CreateDirectory(ToolsFolderPath);
            var destination = Path.Combine(ToolsFolderPath, "ffmpeg.exe");
            bool needsWrite = overwrite || !File.Exists(destination) || new FileInfo(destination).Length != BundledFfmpegSize;
            if (!needsWrite)
            {
                if (string.IsNullOrWhiteSpace(_settings.FfmpegPath) || !File.Exists(_settings.FfmpegPath))
                {
                    _settings.FfmpegPath = destination;
                }
                return true;
            }

            using var input = Assembly.GetExecutingAssembly().GetManifestResourceStream(BundledFfmpegResourceName);
            if (input is null)
            {
                Log("Bundled ffmpeg resource was not found inside the app.");
                return false;
            }

            using var output = File.Create(destination);
            input.CopyTo(output);
            _settings.FfmpegPath = destination;
            return true;
        }
        catch (Exception ex)
        {
            Log($"Could not restore bundled ffmpeg: {ex.Message}");
            return false;
        }
    }

    private bool FilterTrack(object item)
    {
        if (item is not TrackItem track) return false;
        var query = LibrarySearchTextBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(query)) return true;
        return track.Title.Contains(query, StringComparison.OrdinalIgnoreCase)
            || track.FileName.Contains(query, StringComparison.OrdinalIgnoreCase)
            || track.Extension.Contains(query, StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeYoutubeUrl(string inputUrl, bool allowPlaylist)
    {
        if (!Uri.TryCreate(inputUrl, UriKind.Absolute, out var uri))
        {
            return inputUrl.Trim();
        }

        var builder = new UriBuilder(uri);
        var query = HttpUtility.ParseQueryString(builder.Query);
        query.Remove("si");
        if (!allowPlaylist)
        {
            query.Remove("list");
            query.Remove("index");
        }

        builder.Query = query.ToString() ?? string.Empty;
        return builder.Uri.ToString();
    }
    private void RefreshLibrary()
    {
        Directory.CreateDirectory(MusicFolderPath);
        _tracks.Clear();

        long totalBytes = 0;
        var files = Directory.EnumerateFiles(MusicFolderPath)
            .Where(path => SupportedExtensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase))
            .Select(path => new FileInfo(path))
            .OrderBy(info => info.Name, StringComparer.OrdinalIgnoreCase);

        foreach (var file in files)
        {
            totalBytes += file.Length;
            _tracks.Add(new TrackItem
            {
                Title = Path.GetFileNameWithoutExtension(file.Name),
                FileName = file.Name,
                Extension = file.Extension.TrimStart('.').ToUpperInvariant(),
                SizeLabel = FormatBytes(file.Length),
                ModifiedLabel = file.LastWriteTime.ToString("yyyy-MM-dd HH:mm"),
                FullPath = file.FullName,
            });
        }

        _trackView.Refresh();
        TrackCountValueTextBlock.Text = $"{_tracks.Count} tracks";
        StorageValueTextBlock.Text = FormatBytes(totalBytes);
    }

    private void UpdatePipelineStatus()
    {
        var hasYtDlp = !string.IsNullOrWhiteSpace(_settings.YtDlpPath) && File.Exists(_settings.YtDlpPath);
        var hasFfmpeg = !string.IsNullOrWhiteSpace(_settings.FfmpegPath) && File.Exists(_settings.FfmpegPath);
        var cookiesPath = !string.IsNullOrWhiteSpace(_settings.CookiesPath) ? _settings.CookiesPath! : DefaultCookiesFilePath;
        var hasCookies = File.Exists(cookiesPath);

        if (hasYtDlp && hasFfmpeg)
        {
            PipelineValueTextBlock.Text = "ready";
            PipelineDetailTextBlock.Text = hasCookies
                ? "yt-dlp, ffmpeg, and cookies detected. Downloads use mp3 and authenticated requests."
                : "yt-dlp and ffmpeg detected. Downloads will be converted to mp3.";
            return;
        }

        if (hasYtDlp)
        {
            PipelineValueTextBlock.Text = hasCookies ? "yt-dlp + cookies" : "yt-dlp only";
            PipelineDetailTextBlock.Text = hasCookies
                ? "yt-dlp is ready and cookies.txt was found. ffmpeg is missing, so m4a will be preferred."
                : "yt-dlp is ready. ffmpeg is missing, so m4a will be preferred.";
            return;
        }

        PipelineValueTextBlock.Text = "setup needed";
        PipelineDetailTextBlock.Text = "Restore the bundled yt-dlp or browse to an external yt-dlp.exe.";
    }

    private void UpdateDownloadState(string title, string hint)
    {
        DownloadStateTextBlock.Text = title;
        DownloadHintTextBlock.Text = hint;
    }

    private void SetDownloadBusy(bool busy)
    {
        DownloadButton.IsEnabled = !busy;
        ClearUrlButton.IsEnabled = !busy;
        RefreshLibraryButton.IsEnabled = !busy;
        DeleteSelectedButton.IsEnabled = !busy;
        UrlTextBox.IsEnabled = !busy;
    }

    private void Log(string message)
    {
        var line = $"[{DateTime.Now:HH:mm:ss}] {message}";
        LogTextBox.AppendText(line + Environment.NewLine);
        LogTextBox.ScrollToEnd();
    }

    private static string FormatBytes(long value)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        double size = value;
        int unitIndex = 0;
        while (size >= 1024d && unitIndex < units.Length - 1)
        {
            size /= 1024d;
            unitIndex++;
        }
        return $"{size:0.#} {units[unitIndex]}";
    }

    private static string GetAvailableCopyPath(string sourcePath, string destinationFolder)
    {
        var name = Path.GetFileNameWithoutExtension(sourcePath);
        var ext = Path.GetExtension(sourcePath);
        var candidate = Path.Combine(destinationFolder, name + ext);
        int suffix = 2;
        while (File.Exists(candidate))
        {
            candidate = Path.Combine(destinationFolder, $"{name} ({suffix}){ext}");
            suffix++;
        }
        return candidate;
    }

    private static void OpenInExplorer(string path, bool selectFile = false)
    {
        if (selectFile && File.Exists(path))
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true });
            return;
        }

        if (Directory.Exists(path))
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
        }
    }

    private async Task RunDownloadAsync(ProcessStartInfo startInfo)
    {
        var logLines = new List<string>();
        try
        {
            SetDownloadBusy(true);
            _downloadProcess = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
            _downloadProcess.OutputDataReceived += (_, args) =>
            {
                if (!string.IsNullOrWhiteSpace(args.Data))
                {
                    lock (logLines) logLines.Add(args.Data);
                    Dispatcher.InvokeAsync(() => Log(args.Data));
                }
            };
            _downloadProcess.ErrorDataReceived += (_, args) =>
            {
                if (!string.IsNullOrWhiteSpace(args.Data))
                {
                    lock (logLines) logLines.Add(args.Data);
                    Dispatcher.InvokeAsync(() => Log(args.Data));
                }
            };

            if (!_downloadProcess.Start())
            {
                throw new InvalidOperationException("yt-dlp did not start.");
            }

            Log($"Started yt-dlp: {startInfo.FileName}");
            _downloadProcess.BeginOutputReadLine();
            _downloadProcess.BeginErrorReadLine();
            await _downloadProcess.WaitForExitAsync();

            if (_downloadProcess.ExitCode == 0)
            {
                RefreshLibrary();
                UpdateDownloadState("Done", "Download completed and the library has been refreshed.");
                Log("Download completed.");
            }
            else
            {
                string failureReason;
                lock (logLines)
                {
                    failureReason = logLines.LastOrDefault(line => line.StartsWith("ERROR:", StringComparison.OrdinalIgnoreCase))
                        ?? logLines.LastOrDefault(line => line.Contains("Video unavailable", StringComparison.OrdinalIgnoreCase))
                        ?? $"yt-dlp exited with code {_downloadProcess.ExitCode}.";
                }
                UpdateDownloadState("Failed", failureReason);
                Log($"Download failed: {failureReason}");
            }
        }
        catch (Exception ex)
        {
            UpdateDownloadState("Error", ex.Message);
            Log($"Download error: {ex.Message}");
        }
        finally
        {
            _downloadProcess?.Dispose();
            _downloadProcess = null;
            SetDownloadBusy(false);
        }
    }
    private void OpenRootFolderButton_Click(object sender, RoutedEventArgs e) => OpenInExplorer(OmotiRootPath);
    private void OpenMusicFolderButton_Click(object sender, RoutedEventArgs e) => OpenInExplorer(MusicFolderPath);
    private void OpenPlaylistsButton_Click(object sender, RoutedEventArgs e) => OpenInExplorer(File.Exists(PlaylistsFilePath) ? PlaylistsFilePath : OmotiRootPath, File.Exists(PlaylistsFilePath));
    private void ClearUrlButton_Click(object sender, RoutedEventArgs e) => UrlTextBox.Clear();
    private void LibrarySearchTextBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e) => _trackView.Refresh();
    private void RefreshLibraryButton_Click(object sender, RoutedEventArgs e) => RefreshLibrary();

    private async void DetectToolsButton_Click(object sender, RoutedEventArgs e)
    {
        DetectTools();
        await SaveSettingsAsync();
        Log("Tool detection refreshed.");
    }

    private async void BrowseYtDlpButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "yt-dlp executable|yt-dlp.exe|Executable|*.exe" };
        if (dialog.ShowDialog() != true) return;
        _settings.YtDlpPath = dialog.FileName;
        ApplySettingsToUi();
        await SaveSettingsAsync();
    }

    private async void BrowseFfmpegButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "ffmpeg executable|ffmpeg.exe|Executable|*.exe" };
        if (dialog.ShowDialog() != true) return;
        _settings.FfmpegPath = dialog.FileName;
        ApplySettingsToUi();
        await SaveSettingsAsync();
    }

    private async void InstallYtDlpButton_Click(object sender, RoutedEventArgs e)
    {
        if (!EnsureBundledYtDlp(overwrite: true))
        {
            MessageBox.Show(this, "Bundled yt-dlp could not be restored.", "Omoti Manager", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        DetectTools(saveSettings: false);
        ApplySettingsToUi();
        await SaveSettingsAsync();
        Log($"Bundled yt-dlp restored to {_settings.YtDlpPath}");
    }

    private void LibraryListView_MouseDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (LibraryListView.SelectedItem is TrackItem item)
        {
            OpenInExplorer(item.FullPath, selectFile: true);
        }
    }

    private async void ImportFilesButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Multiselect = true,
            Filter = "Audio|*.mp3;*.wav;*.ogg;*.flac;*.m4a|All Files|*.*"
        };
        if (dialog.ShowDialog() != true) return;

        Directory.CreateDirectory(MusicFolderPath);
        int imported = 0;
        foreach (var file in dialog.FileNames)
        {
            var ext = Path.GetExtension(file);
            if (!SupportedExtensions.Contains(ext, StringComparer.OrdinalIgnoreCase)) continue;
            var destination = GetAvailableCopyPath(file, MusicFolderPath);
            File.Copy(file, destination, overwrite: false);
            imported++;
        }

        RefreshLibrary();
        Log($"Imported {imported} track(s) into Omoti Music.");
        await SaveSettingsAsync();
    }

    private void DeleteSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        if (LibraryListView.SelectedItem is not TrackItem item)
        {
            MessageBox.Show(this, "Select a track first.", "Omoti Manager", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var confirm = MessageBox.Show(this, $"Delete '{item.FileName}' from Omoti Music?", "Confirm delete", MessageBoxButton.YesNo, MessageBoxImage.Warning);
        if (confirm != MessageBoxResult.Yes) return;

        File.Delete(item.FullPath);
        RefreshLibrary();
        Log($"Deleted {item.FileName}");
    }

    private async void DownloadButton_Click(object sender, RoutedEventArgs e)
    {
        if (_downloadProcess is { HasExited: false })
        {
            MessageBox.Show(this, "A download is already running.", "Omoti Manager", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var url = UrlTextBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(url))
        {
            MessageBox.Show(this, "Paste a YouTube URL first.", "Omoti Manager", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        DetectTools(saveSettings: false);
        if (string.IsNullOrWhiteSpace(_settings.YtDlpPath) || !File.Exists(_settings.YtDlpPath))
        {
            UpdateDownloadState("Setup needed", "Restore the bundled yt-dlp or browse to yt-dlp.exe first.");
            MessageBox.Show(this, "yt-dlp.exe was not found. Restore the bundled copy first.", "Omoti Manager", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (string.IsNullOrWhiteSpace(_settings.FfmpegPath) || !File.Exists(_settings.FfmpegPath))
        {
            UpdateDownloadState("Setup needed", "Bundled ffmpeg could not be restored, so mp3 conversion is unavailable.");
            MessageBox.Show(this, "ffmpeg.exe was not found. The app could not prepare mp3 conversion.", "Omoti Manager", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        _settings.AllowPlaylistDownloads = AllowPlaylistCheckBox.IsChecked == true;
        if (string.IsNullOrWhiteSpace(_settings.CookiesPath) && File.Exists(DefaultCookiesFilePath))
        {
            _settings.CookiesPath = DefaultCookiesFilePath;
        }
        await SaveSettingsAsync();

        var startInfo = new ProcessStartInfo
        {
            FileName = _settings.YtDlpPath,
            WorkingDirectory = MusicFolderPath,
            UseShellExecute = false,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };

        startInfo.ArgumentList.Add("--newline");
        startInfo.ArgumentList.Add("--windows-filenames");
        startInfo.ArgumentList.Add("--no-mtime");
        startInfo.ArgumentList.Add("--extractor-args");
        startInfo.ArgumentList.Add("youtube:player_client=android,ios,web");
        startInfo.ArgumentList.Add("--output");
        startInfo.ArgumentList.Add(Path.Combine(MusicFolderPath, "%(title)s.%(ext)s"));
        if (!_settings.AllowPlaylistDownloads) startInfo.ArgumentList.Add("--no-playlist");
        if (!string.IsNullOrWhiteSpace(_settings.CookiesPath) && File.Exists(_settings.CookiesPath))
        {
            startInfo.ArgumentList.Add("--cookies");
            startInfo.ArgumentList.Add(_settings.CookiesPath);
        }

        startInfo.ArgumentList.Add("--ffmpeg-location");
        startInfo.ArgumentList.Add(Path.GetDirectoryName(_settings.FfmpegPath)!);
        startInfo.ArgumentList.Add("--format");
        startInfo.ArgumentList.Add("bestaudio/best");
        startInfo.ArgumentList.Add("--extract-audio");
        startInfo.ArgumentList.Add("--audio-format");
        startInfo.ArgumentList.Add("mp3");
        startInfo.ArgumentList.Add("--audio-quality");
        startInfo.ArgumentList.Add("0");
        startInfo.ArgumentList.Add("--embed-metadata");
        UpdateDownloadState("Downloading", "Bundled ffmpeg detected. Final output: mp3.");

        startInfo.ArgumentList.Add(NormalizeYoutubeUrl(url, _settings.AllowPlaylistDownloads));
        await RunDownloadAsync(startInfo);
    }

    private void MainWindow_Closing(object? sender, CancelEventArgs e)
    {
        if (_downloadProcess is { HasExited: false })
        {
            try { _downloadProcess.Kill(true); } catch { }
        }
    }
}
