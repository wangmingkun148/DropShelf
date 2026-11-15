// 压缩和转换图片，和 C# 版的 ImageCompressor、ImageConverter 对应。原文件不会改变，结果另存为新文件。
// 同步执行，在已经初始化 COM 的后台线程上调用（见 task_run_long_imaging）。
#pragma once

#include "base.h"

typedef enum ImageStatus
{
    IMAGE_DONE,                 // 压缩或转换好了
    IMAGE_ALREADY,              // 本来就足够小 / 本来就是这个格式
    IMAGE_NOT_IMAGE,
    IMAGE_FAILED
} ImageStatus;

/// 按扩展名看是不是能处理的图片。
bool image_is_path(const wchar_t *path);

/// 压缩为不超过 maximum_bytes 的 JPG：先在保持尺寸的前提下降低画质，仍然过大时再按比例缩小。
/// output_directory 为 NULL 时放在原图旁边。成功时 output 是新文件的路径（调用方 free）。
ImageStatus image_compress(const wchar_t *source, long long maximum_bytes, const wchar_t *output_directory, wchar_t **output);

/// 另存为 JPG 或 PNG（ImageFormat）：按拍摄方向转正；PNG 保留透明，JPG 的透明部分铺白色。
ImageStatus image_convert(const wchar_t *source, int format, const wchar_t *output_directory, wchar_t **output);

/// 输出文件名中的大小，例如 500KB、2MB。调用方 free。
wchar_t *image_format_limit(long long maximum_bytes);
