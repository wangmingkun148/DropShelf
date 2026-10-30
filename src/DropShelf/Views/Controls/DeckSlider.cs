using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;

namespace DropShelf.Views.Controls;

/// <summary>
/// 切牌滑块：轨道是一排 10 张小牌，滑钮是一张切牌卡。切牌卡越过哪张牌的中线，那张就翻成牌背，退回时再翻回牌面。
/// 点在轨道上时滑钮跳到光标处，不松手就能接着拖。
/// </summary>
public sealed class DeckSlider : Slider
{
    private const int CardCount = 10;
    private static readonly TimeSpan HalfFlip = TimeSpan.FromMilliseconds(130);
    private readonly List<Card> _cards = [];
    private Track? _track;

    static DeckSlider()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(DeckSlider), new FrameworkPropertyMetadata(typeof(DeckSlider)));
    }

    /// <summary>已经翻成牌背的张数。</summary>
    internal int FaceDownCount => _cards.Count(card => card.Down);

    public override void OnApplyTemplate()
    {
        base.OnApplyTemplate();
        _track = GetTemplateChild("PART_Track") as Track;
        _cards.Clear();
        if (GetTemplateChild("PART_Cards") is not Panel panel) return;

        panel.Children.Clear();
        for (var i = 0; i < CardCount; i++)
        {
            var pip = new Rectangle { Width = 3, Height = 3, RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new RotateTransform(45) };
            pip.SetResourceReference(Shape.FillProperty, "TextTertiaryBrush");
            var face = CreateSide("ShelfTileBrush", "ShelfTileBorderBrush");
            face.Child = pip;
            var back = CreateSide("StackEdgeBrush", "CardBackLineBrush");
            back.Opacity = 0;
            var scale = new ScaleTransform();
            panel.Children.Add(new Grid
            {
                Width = 12, Height = 16, RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = scale,
                Children = { face, back }
            });
            _cards.Add(new Card(scale, back));
        }
        UpdateCards(animate: false);
    }

    protected override void OnValueChanged(double oldValue, double newValue)
    {
        base.OnValueChanged(oldValue, newValue);
        UpdateCards(IsLoaded && SystemParameters.ClientAreaAnimation);
    }

    protected override void OnPreviewMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        var thumb = _track?.Thumb;
        var pressedThumb = thumb?.IsMouseOver == true;
        base.OnPreviewMouseLeftButtonDown(e);
        if (thumb is null || pressedThumb || !IsMoveToPointEnabled) return;

        // 滑钮已跳到光标处，把这次按下交给它，切牌卡很窄，不必先瞄准再拖。
        thumb.UpdateLayout();
        thumb.RaiseEvent(new MouseButtonEventArgs(e.MouseDevice, e.Timestamp, MouseButton.Left)
        {
            RoutedEvent = MouseLeftButtonDownEvent,
            Source = thumb
        });
    }

    private static Border CreateSide(string background, string border)
    {
        var side = new Border { BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(2) };
        side.SetResourceReference(Border.BackgroundProperty, background);
        side.SetResourceReference(Border.BorderBrushProperty, border);
        return side;
    }

    /// <summary>牌按滑钮能走到的范围等距排开，第 i 张的中线正好是取值 i×10+5 时滑钮的位置。</summary>
    private void UpdateCards(bool animate)
    {
        var span = Maximum - Minimum;
        for (var i = 0; i < _cards.Count; i++)
        {
            var card = _cards[i];
            var down = span > 0 && (Value - Minimum) / span >= (i + 0.5) / CardCount;
            if (down == card.Down) continue;
            card.Down = down;

            if (!animate)
            {
                card.Scale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
                card.Back.BeginAnimation(OpacityProperty, null);
                card.Back.Opacity = down ? 1 : 0;
                continue;
            }

            // 先压扁到 0，换面后再展开，看起来像绕竖轴翻过来；翻到一半又退回时从当前宽度接着走。
            card.Scale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimationUsingKeyFrames
            {
                KeyFrames =
                {
                    new EasingDoubleKeyFrame(0, KeyTime.FromTimeSpan(HalfFlip), new QuadraticEase { EasingMode = EasingMode.EaseIn }),
                    new EasingDoubleKeyFrame(1, KeyTime.FromTimeSpan(HalfFlip * 2), new QuadraticEase { EasingMode = EasingMode.EaseOut })
                }
            });
            card.Back.BeginAnimation(OpacityProperty, new DoubleAnimationUsingKeyFrames
            {
                KeyFrames = { new DiscreteDoubleKeyFrame(down ? 1 : 0, KeyTime.FromTimeSpan(HalfFlip)) }
            });
        }
    }

    private sealed class Card(ScaleTransform scale, UIElement back)
    {
        public ScaleTransform Scale { get; } = scale;
        public UIElement Back { get; } = back;
        public bool Down { get; set; }
    }
}
