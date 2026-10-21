using DropShelf.Models;

namespace DropShelf.Services.Gestures;

public sealed class MouseGestureDetector
{
    private readonly Queue<long> _reversals = new();
    private readonly Queue<(long Time, double Distance)> _travel = new();
    private GestureProfile _profile;
    private bool _hasLastPoint;
    private int _lastX;
    private int _lastY;
    private long _lastTime;
    private double _directionX;
    private double _directionY;
    private double _segmentDistance;
    private int _consecutiveEdgeSamples;
    private bool _toleratedSideStep;
    private bool _hasRawPoint;
    private int _rawX;
    private int _rawY;
    private long _rawTime;

    public MouseGestureDetector(GestureSensitivity sensitivity) => _profile = GestureProfile.For(sensitivity);

    public void SetSensitivity(GestureSensitivity sensitivity)
    {
        _profile = GestureProfile.For(sensitivity);
        Reset();
    }

    /// <summary>
    /// 接收原始鼠标事件（可能高达 1000 次/秒），按位移或时间重新取样后再判断，
    /// 避免高回报率鼠标的细碎位移让方向判断失真。
    /// </summary>
    public bool AddRawSample(int x, int y, long timestampMilliseconds, bool nearOuterScreenEdge)
    {
        if (_hasRawPoint)
        {
            var deltaX = x - _rawX;
            var deltaY = y - _rawY;
            if (deltaX * deltaX + deltaY * deltaY < 36 && timestampMilliseconds - _rawTime < 30) return false;
        }

        _rawX = x;
        _rawY = y;
        _rawTime = timestampMilliseconds;
        _hasRawPoint = true;
        return AddSample(x, y, timestampMilliseconds, nearOuterScreenEdge);
    }

    public bool AddSample(int x, int y, long timestampMilliseconds, bool nearOuterScreenEdge)
    {
        if (!_hasLastPoint)
        {
            SetLastPoint(x, y, timestampMilliseconds);
            return false;
        }

        var elapsed = timestampMilliseconds - _lastTime;
        var deltaX = x - _lastX;
        var deltaY = y - _lastY;
        SetLastPoint(x, y, timestampMilliseconds);

        if (nearOuterScreenEdge)
        {
            _consecutiveEdgeSamples++;
            if (_consecutiveEdgeSamples >= 2) ResetMotionKeepingLastPoint();
            return false;
        }
        _consecutiveEdgeSamples = 0;

        // 高回报率鼠标的两次采样可能落在同一毫秒，时间差为 0 属于正常情况。
        if (elapsed < 0 || elapsed > 500)
        {
            ResetMotionKeepingLastPoint();
            return false;
        }

        var distance = Math.Sqrt(deltaX * deltaX + deltaY * deltaY);
        if (distance < 2) return false;

        var newDirectionX = deltaX / distance;
        var newDirectionY = deltaY / distance;
        if (_directionX == 0 && _directionY == 0)
        {
            AddTravel(timestampMilliseconds, distance);
            _directionX = newDirectionX;
            _directionY = newDirectionY;
            _segmentDistance = distance;
            return false;
        }

        var alignment = newDirectionX * _directionX + newDirectionY * _directionY;
        if (alignment is > -0.18 and < 0.18 && !_toleratedSideStep && distance < _profile.MinimumSegmentDistance * 0.6)
        {
            // 掉头瞬间常有一次细小的侧向位移，容忍一次；连续的侧向移动（如绕圈）仍按普通移动处理。
            _toleratedSideStep = true;
            return false;
        }
        _toleratedSideStep = false;

        if (alignment <= -0.18)
        {
            AddTravel(timestampMilliseconds, distance);
            if (_segmentDistance >= _profile.MinimumSegmentDistance)
                _reversals.Enqueue(timestampMilliseconds);

            _directionX = newDirectionX;
            _directionY = newDirectionY;
            _segmentDistance = distance;
        }
        else if (alignment >= 0.18)
        {
            AddTravel(timestampMilliseconds, distance);
            _segmentDistance += distance;
        }
        else
        {
            // 大角度转弯视为普通移动，不计入来回晃动。
            ResetMotionKeepingLastPoint();
            AddTravel(timestampMilliseconds, distance);
            _directionX = newDirectionX;
            _directionY = newDirectionY;
            _segmentDistance = distance;
            return false;
        }

        Trim(timestampMilliseconds);
        var totalTravel = _travel.Sum(item => item.Distance);
        var gestureDuration = _travel.TryPeek(out var firstTravel)
            ? timestampMilliseconds - firstTravel.Time
            : 0;
        if (_reversals.Count < _profile.RequiredReversals ||
            totalTravel < _profile.MinimumTotalTravel ||
            gestureDuration < _profile.MinimumDurationMilliseconds)
            return false;

        Reset();
        return true;
    }

    public void Reset()
    {
        _hasLastPoint = false;
        _lastX = 0;
        _lastY = 0;
        _lastTime = 0;
        _directionX = 0;
        _directionY = 0;
        _segmentDistance = 0;
        _consecutiveEdgeSamples = 0;
        _toleratedSideStep = false;
        _hasRawPoint = false;
        _reversals.Clear();
        _travel.Clear();
    }

    private void SetLastPoint(int x, int y, long time)
    {
        _lastX = x;
        _lastY = y;
        _lastTime = time;
        _hasLastPoint = true;
    }

    private void ResetMotionKeepingLastPoint()
    {
        _toleratedSideStep = false;
        _directionX = 0;
        _directionY = 0;
        _segmentDistance = 0;
        _reversals.Clear();
        _travel.Clear();
    }

    private void AddTravel(long time, double distance)
    {
        _travel.Enqueue((time, distance));
        Trim(time);
    }

    private void Trim(long now)
    {
        var cutoff = now - _profile.MaximumDurationMilliseconds;
        while (_reversals.TryPeek(out var reversal) && reversal < cutoff) _reversals.Dequeue();
        while (_travel.TryPeek(out var travel) && travel.Time < cutoff) _travel.Dequeue();
    }

    private sealed record GestureProfile(
        double MinimumSegmentDistance,
        double MinimumTotalTravel,
        int RequiredReversals,
        int MinimumDurationMilliseconds,
        int MaximumDurationMilliseconds)
    {
        public static GestureProfile For(GestureSensitivity sensitivity) => sensitivity switch
        {
            GestureSensitivity.Responsive => new GestureProfile(20, 94, 2, 120, 800),
            GestureSensitivity.Steady => new GestureProfile(51, 238, 3, 220, 750),
            _ => new GestureProfile(34, 153, 3, 180, 800)
        };
    }
}
