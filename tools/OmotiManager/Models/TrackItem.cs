namespace OmotiManager.Models;

public sealed class TrackItem
{
    public required string Title { get; init; }

    public required string FileName { get; init; }

    public required string Extension { get; init; }

    public required string SizeLabel { get; init; }

    public required string ModifiedLabel { get; init; }

    public required string FullPath { get; init; }
}
