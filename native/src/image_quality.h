#pragma once

// 压缩图片时找画质用的搜索。不依赖系统，可以单独测试。

/// 按这个画质编码一次，返回得到的字节数；失败时返回负数。
typedef long long (*QualityMeasure)(void *context, int quality);

/// 找编码结果不超过 maximum 的最高画质（画质越高文件越大）。
/// low 已经确认不超过（low_size 是它的大小）；high 当作超过，只找到 high - 1 为止，high_size 是它的大小，
/// 没有实际编码过时可以给估计值，不知道就给 0。返回找到的画质；measure 失败时返回 -1。
/// 最后一次没有超过上限的 measure 就是返回的画质（一次都没有时是 low）。
int image_quality_search(int low, long long low_size, int high, long long high_size, long long maximum, QualityMeasure measure, void *context);
