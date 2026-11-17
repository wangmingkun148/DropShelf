#include "image_quality.h"

#include <math.h>

/// 前面这么多次完全按估计的位置去试，之后区间缩得不够快就改为对半分。
#define FREE_GUESSES 2

/// JPEG 的画质决定量化表放大多少（IJG 的换算：50 以下是 5000 / 画质，以上是 200 - 2 × 画质）。
/// 文件大小的对数和它的对数大致成直线，按它估计比直接按画质估计准得多。
static double position_of(int quality)
{
    if (quality < 1) quality = 1;
    double scale = quality < 50 ? 5000.0 / quality : 200.0 - 2.0 * quality;
    return -log(scale < 1 ? 1 : scale);
}

/// 每编码一次都要处理整张图片，所以尽量少试：不对半分，而是按两头已知的大小估计上限落在哪个画质，直接去试那里，
/// 通常两三次就能把区间缩到相邻的两个画质。估计只决定先试哪里，结果和逐个试出来的一样。
/// 大小不按预想变化时（估计帮不上忙）退回对半分，次数最多比对半分多两三次。
int image_quality_search(int low, long long low_size, int high, long long high_size, long long maximum, QualityMeasure measure, void *context)
{
    int width = high - low, tried = 0;
    while (high - low > 1)
    {
        int allowed = width >> (tried > FREE_GUESSES ? tried - FREE_GUESSES : 0);
        if (allowed < 2) allowed = 2;
        int guess = (low + high) / 2;
        if (high - low <= allowed && maximum > 0 && low_size > 0 && high_size > low_size)
        {
            double share = (log((double)maximum) - log((double)low_size)) / (log((double)high_size) - log((double)low_size));
            if (share < 0) share = 0;
            if (share > 1) share = 1;
            double target = position_of(low) + share * (position_of(high) - position_of(low));
            guess = low;
            while (guess + 1 < high && position_of(guess + 1) <= target) guess++;
        }
        if (guess <= low) guess = low + 1;
        if (guess >= high) guess = high - 1;
        long long size = measure(context, guess);
        if (size < 0) return -1;
        tried++;
        if (size <= maximum)
        {
            low = guess;
            low_size = size;
        }
        else
        {
            high = guess;
            high_size = size;
        }
    }
    return low;
}
