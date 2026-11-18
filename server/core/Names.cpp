#include "core/Names.h"

#include <utf8proc.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace whist
{

namespace
{

// Decodes one code point, or returns false for malformed, overlong or surrogate sequences.
bool nextCodePoint(std::string_view text, std::size_t& at, char32_t& codePoint)
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);

    std::size_t length = 0;
    char32_t minimum = 0;
    if(lead < 0x80)
    {
        codePoint = lead;
        at += 1;
        return true;
    }
    if((lead & 0xE0) == 0xC0)
    {
        length = 2;
        minimum = 0x80;
        codePoint = lead & 0x1F;
    }
    else if((lead & 0xF0) == 0xE0)
    {
        length = 3;
        minimum = 0x800;
        codePoint = lead & 0x0F;
    }
    else if((lead & 0xF8) == 0xF0)
    {
        length = 4;
        minimum = 0x10000;
        codePoint = lead & 0x07;
    }
    else
        return false;

    if(at + length > text.size())
        return false;

    for(std::size_t i = 1 ; i < length ; i++)
    {
        if((byte(at + i) & 0xC0) != 0x80)
            return false;
        codePoint = (codePoint << 6) | (byte(at + i) & 0x3F);
    }

    if(codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        return false;

    at += length;
    return true;
}

std::optional<std::u32string> decode(std::string_view text)
{
    std::u32string codePoints;
    std::size_t at = 0;
    char32_t codePoint = 0;
    while(at < text.size())
    {
        if(!nextCodePoint(text, at, codePoint))
            return std::nullopt;
        codePoints += codePoint;
    }
    return codePoints;
}

std::string encode(const std::u32string& codePoints)
{
    std::string text;
    for(char32_t codePoint : codePoints)
    {
        utf8proc_uint8_t bytes[4];
        const utf8proc_ssize_t length = utf8proc_encode_char(static_cast<utf8proc_int32_t>(codePoint), bytes);
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
    }
    return text;
}

// Valid UTF-8 in, so utf8proc never fails on it.
std::string transform(std::string_view text, int options)
{
    if(text.empty())
        return {};

    utf8proc_uint8_t* result = nullptr;
    const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
                                                 static_cast<utf8proc_ssize_t>(text.size()), &result,
                                                 static_cast<utf8proc_option_t>(options));
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(result, &std::free);
    if(length < 0)
        return std::string(text);
    return std::string(reinterpret_cast<const char*>(result), static_cast<std::size_t>(length));
}

utf8proc_category_t categoryOf(char32_t codePoint)
{
    return utf8proc_category(static_cast<utf8proc_int32_t>(codePoint));
}

bool isBidiFormatting(char32_t c)
{
    return c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

bool isJoinerOrSelector(char32_t c)
{
    return c == 0x200C || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF);
}

bool isIgnorable(char32_t c)
{
    return utf8proc_get_property(static_cast<utf8proc_int32_t>(c))->ignorable;
}

bool isSpace(char32_t c)
{
    const utf8proc_category_t category = categoryOf(c);
    return category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP;
}

// Something a reader sees: a letter, number, punctuation or symbol, but not the braille pattern
// that prints as nothing.
bool isVisible(char32_t c)
{
    if(c == 0x2800)
        return false;
    switch(categoryOf(c))
    {
        case UTF8PROC_CATEGORY_LU:
        case UTF8PROC_CATEGORY_LL:
        case UTF8PROC_CATEGORY_LT:
        case UTF8PROC_CATEGORY_LM:
        case UTF8PROC_CATEGORY_LO:
        case UTF8PROC_CATEGORY_ND:
        case UTF8PROC_CATEGORY_NL:
        case UTF8PROC_CATEGORY_NO:
        case UTF8PROC_CATEGORY_PC:
        case UTF8PROC_CATEGORY_PD:
        case UTF8PROC_CATEGORY_PS:
        case UTF8PROC_CATEGORY_PE:
        case UTF8PROC_CATEGORY_PI:
        case UTF8PROC_CATEGORY_PF:
        case UTF8PROC_CATEGORY_PO:
        case UTF8PROC_CATEGORY_SM:
        case UTF8PROC_CATEGORY_SC:
        case UTF8PROC_CATEGORY_SK:
        case UTF8PROC_CATEGORY_SO:
            return true;
        default:
            return false;
    }
}

constexpr int nfc = UTF8PROC_STABLE | UTF8PROC_COMPOSE;

} // namespace

bool isValidUtf8(std::string_view text)
{
    return decode(text).has_value();
}

std::size_t codePointCount(std::string_view text)
{
    const std::optional<std::u32string> codePoints = decode(text);
    return codePoints ? codePoints->size() : text.size();
}

std::optional<std::string> cleanName(std::string_view name)
{
    const std::optional<std::u32string> input = decode(name);
    if(!input)
        return std::nullopt;

    std::u32string kept;
    for(char32_t c : *input)
    {
        const utf8proc_category_t category = categoryOf(c);
        if(category == UTF8PROC_CATEGORY_CC || isBidiFormatting(c))
            return std::nullopt;

        if(isSpace(c))
        {
            if(!kept.empty() && kept.back() != U' ')
                kept += U' ';
            continue;
        }
        if(isJoinerOrSelector(c))
        {
            kept += c;
            continue;
        }
        if(isIgnorable(c))
            continue;

        // Formatting that is not ignorable, such as interlinear annotation, can hide text.
        if(category == UTF8PROC_CATEGORY_CF)
            return std::nullopt;
        kept += c;
    }
    while(!kept.empty() && kept.back() == U' ')
        kept.pop_back();

    std::string cleaned = transform(encode(kept), nfc);
    const std::optional<std::u32string> result = decode(cleaned);
    if(!result || result->empty() || result->size() > maxNameLength)
        return std::nullopt;
    if(!std::ranges::any_of(*result, isVisible))
        return std::nullopt;
    return cleaned;
}

bool isCleanName(std::string_view name)
{
    const std::optional<std::string> cleaned = cleanName(name);
    return cleaned && *cleaned == name;
}

std::string nameKey(std::string_view name)
{
    if(!isValidUtf8(name))
        return std::string(name);

    std::string folded = transform(name, nfc | UTF8PROC_COMPAT | UTF8PROC_CASEFOLD | UTF8PROC_IGNORE);
    std::u32string codePoints = decode(folded).value_or(std::u32string{});
    for(char32_t& c : codePoints)
    {
        if(c == 0x015F)
            c = 0x0219;
        else if(c == 0x0163)
            c = 0x021B;
        else if(isSpace(c))
            c = U' ';
    }
    return encode(codePoints);
}

std::string numberedName(std::string_view name, const std::set<std::string>& takenKeys)
{
    if(!takenKeys.contains(nameKey(name)))
        return std::string(name);

    const std::u32string base = decode(name).value_or(std::u32string{});
    for(unsigned int number = 2 ; ; number++)
    {
        const std::u32string suffix = [&]
        {
            std::u32string text = U" ";
            for(char c : std::to_string(number))
                text += static_cast<char32_t>(c);
            return text;
        }();

        std::u32string cut = base.substr(0, maxNameLength - suffix.size());
        while(!cut.empty() && (cut.back() == U' ' || isJoinerOrSelector(cut.back())))
            cut.pop_back();

        const std::string candidate = transform(encode(cut + suffix), nfc);
        if(!takenKeys.contains(nameKey(candidate)))
            return candidate;
    }
}

} // namespace whist
