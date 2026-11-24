using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services.Gestures;

namespace DropShelf.ViewModels;

/// <summary>晃动示范校准的对话框：示范三次，按实际的晃法给出建议的灵敏度，采用后写回滑块。</summary>
public sealed class ShakeCalibrationViewModel : ObservableObject
{
    private readonly List<int> _levels = [];
    private bool _tooWeak;

    public ShakeCalibrationViewModel(Action<int> apply, Action cancel)
    {
        ApplyCommand = new RelayCommand(_ => { if (IsComplete) apply(Recommended); }, _ => IsComplete);
        RetryCommand = new RelayCommand(_ => Restart(), _ => _levels.Count > 0 || _tooWeak);
        CancelCommand = new RelayCommand(_ => cancel());
    }

    public ICommand ApplyCommand { get; }
    public ICommand RetryCommand { get; }
    public ICommand CancelCommand { get; }

    /// <summary>已经记下几次示范。</summary>
    public int Done => _levels.Count;
    public bool FirstDone => Done >= 1;
    public bool SecondDone => Done >= 2;
    public bool ThirdDone => Done >= 3;
    public bool IsComplete => Done >= ShakeCalibration.Demonstrations;
    public bool IsPracticing => !IsComplete;

    /// <summary>建议的灵敏度；示范完之前没有意义。</summary>
    public int Recommended => IsComplete ? ShakeCalibration.Recommend(_levels) : ShakeSensitivities.Balanced;

    public string Status => IsComplete ? Strings.CalibrationDone
        : _tooWeak ? Strings.CalibrationTooWeak
        : Done == 0 ? Strings.CalibrationStart
        : Strings.CalibrationProgress(Done, ShakeCalibration.Demonstrations - Done);

    public string Result
    {
        get
        {
            var (reversals, distance) = MouseGestureDetector.Describe(Recommended);
            return Strings.SensitivityDetail(reversals, distance);
        }
    }

    /// <summary>交来一次示范；晃得太轻、最灵敏也触发不了时不算数，返回 false。</summary>
    public bool Submit(IReadOnlyList<ShakeSample> stroke)
    {
        if (IsComplete) return false;
        var level = ShakeCalibration.SteadiestLevel(stroke);
        _tooWeak = level is null;
        if (level is { } counted) _levels.Add(counted);
        RefreshText();
        return level is not null;
    }

    private void Restart()
    {
        _levels.Clear();
        _tooWeak = false;
        RefreshText();
    }

    public void RefreshText()
    {
        foreach (var name in new[]
                 {
                     nameof(Done), nameof(FirstDone), nameof(SecondDone), nameof(ThirdDone), nameof(IsComplete), nameof(IsPracticing),
                     nameof(Recommended), nameof(Status), nameof(Result)
                 })
            OnPropertyChanged(name);
        CommandManager.InvalidateRequerySuggested();
    }
}
