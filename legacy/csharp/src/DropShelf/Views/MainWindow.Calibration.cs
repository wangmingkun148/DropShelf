using System.Diagnostics;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Services.Gestures;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

/// <summary>
/// 晃动示范校准：在练习区按住那张“文件”来回晃，记下这期间鼠标的位置和时刻，松手后交给校准去算。
/// 位置取屏幕像素、时刻取毫秒，和平时识别晃动时拿到的一样。
/// </summary>
public partial class MainWindow
{
    private List<ShakeSample>? _stroke;
    private WpfPoint _strokeOrigin;

    private void OnCalibrationPadDown(object sender, MouseButtonEventArgs e)
    {
        if (ViewModel.Calibration is not { IsComplete: false }) return;
        _stroke = [];
        _strokeOrigin = e.GetPosition(CalibrationPad);
        CalibrationChipOffset.BeginAnimation(TranslateTransform.XProperty, null);
        CalibrationChipOffset.BeginAnimation(TranslateTransform.YProperty, null);
        CalibrationChipOffset.X = CalibrationChipOffset.Y = 0;
        AddCalibrationSample();
        CalibrationPad.CaptureMouse();
        e.Handled = true;
    }

    private void OnCalibrationPadMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (_stroke is null) return;
        AddCalibrationSample();
        // 那张“文件”跟着光标走，像真的在拖。
        var point = e.GetPosition(CalibrationPad);
        CalibrationChipOffset.X = point.X - _strokeOrigin.X;
        CalibrationChipOffset.Y = point.Y - _strokeOrigin.Y;
    }

    private void OnCalibrationPadUp(object sender, MouseButtonEventArgs e) => EndCalibrationStroke();

    private void OnCalibrationPadLostCapture(object sender, System.Windows.Input.MouseEventArgs e) => EndCalibrationStroke();

    private void EndCalibrationStroke()
    {
        if (_stroke is not { } stroke) return;
        _stroke = null;
        CalibrationPad.ReleaseMouseCapture();
        // 松手后弹回中间。
        var ease = new BackEase { Amplitude = 0.4, EasingMode = EasingMode.EaseOut };
        CalibrationChipOffset.BeginAnimation(TranslateTransform.XProperty,
            ShelfMotion.Gate(new DoubleAnimation(0, TimeSpan.FromMilliseconds(260)) { EasingFunction = ease }));
        CalibrationChipOffset.BeginAnimation(TranslateTransform.YProperty,
            ShelfMotion.Gate(new DoubleAnimation(0, TimeSpan.FromMilliseconds(260)) { EasingFunction = ease }));
        ViewModel.Calibration?.Submit(stroke);
    }

    private void AddCalibrationSample()
    {
        var point = WindowPlacement.GetCursor();
        _stroke!.Add(new ShakeSample(point.X, point.Y, (long)Stopwatch.GetElapsedTime(0).TotalMilliseconds));
    }
}
