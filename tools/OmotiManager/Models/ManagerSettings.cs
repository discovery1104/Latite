namespace OmotiManager.Models;

public sealed class ManagerSettings
{
    public string? YtDlpPath { get; set; }

    public string? FfmpegPath { get; set; }

    public string? CookiesPath { get; set; }

    public bool AllowPlaylistDownloads { get; set; }
}
