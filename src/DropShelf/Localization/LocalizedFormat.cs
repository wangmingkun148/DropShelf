using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;

namespace DropShelf.Localization;

/// <summary>
/// 一条界面文字连同它的原文模板：<c>$"已添加 {count} 项"</c> 交到这里时，既能得到“已添加 3 项”，
/// 也留着模板“已添加 {0} 项”和参数 3。语言包按模板找译文，再把同样的参数填进去，见 <see cref="LanguagePack"/>。
/// 普通字符串也可以直接当作一条文字（没有参数）。<see cref="Strings"/> 里的写法不用变，由编译器把带参数的文字交给这里。
/// </summary>
[InterpolatedStringHandler]
public sealed class LocalizedFormat
{
    private readonly StringBuilder? _template;
    private readonly List<object?>? _arguments;
    private readonly string? _literal;

    /// <summary>编译器在遇到带参数的文字时调用：literalLength 是固定部分的长度，formattedCount 是参数个数。</summary>
    public LocalizedFormat(int literalLength, int formattedCount)
    {
        _template = new StringBuilder(literalLength + formattedCount * 4);
        _arguments = new List<object?>(formattedCount);
    }

    private LocalizedFormat(string literal) => _literal = literal;

    public static implicit operator LocalizedFormat(string text) => new(text);

    /// <summary>固定的文字：花括号要写成两个，才不会被当成参数的位置。</summary>
    public void AppendLiteral(string text) => _template!.Append(text.Replace("{", "{{").Replace("}", "}}"));

    public void AppendFormatted<T>(T value) => Add(value, null);

    public void AppendFormatted<T>(T value, string? format) => Add(value, format);

    private void Add(object? value, string? format)
    {
        _template!.Append('{').Append(_arguments!.Count);
        if (!string.IsNullOrEmpty(format)) _template.Append(':').Append(format);
        _template.Append('}');
        _arguments.Add(value);
    }

    /// <summary>原文模板，参数的位置写成 {0}、{1}……；没有参数的文字就是它本身。语言包用它查找译文。</summary>
    public string Template => _literal ?? _template!.ToString();

    /// <summary>没有参数：模板就是最终的文字，里面的花括号不是参数的位置。</summary>
    public bool IsLiteral => _literal is not null;

    /// <summary>把参数填进另一个模板（译文）。译文里的参数位置写错时返回 null，由调用方退回原文。</summary>
    public string? Format(string template)
    {
        if (_literal is not null) return template;
        try
        {
            return string.Format(CultureInfo.CurrentCulture, template, _arguments!.ToArray());
        }
        catch (FormatException)
        {
            return null;
        }
    }

    /// <summary>原文：和直接写 $"…" 得到的一样。</summary>
    public override string ToString() => _literal ?? string.Format(CultureInfo.CurrentCulture, _template!.ToString(), _arguments!.ToArray());
}
