// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../inc/FontInfoDesired.hpp"

FontInfoDesired::FontInfoDesired(const std::wstring_view& faceName, const unsigned char family, const unsigned weight, const unsigned codePage, CellSizeInDIP cellSizeInDIP, float fontSizeInPt) :
    FontInfoBase(
        faceName,
        family,
        weight,
        codePage),
    _cellSizeInDIP{ cellSizeInDIP },
    _fontSizeInPt{ fontSizeInPt }
{
    _validate();
}

FontInfoDesired::FontInfoDesired(const wchar_t* faceName, const unsigned char family, const unsigned weight, const unsigned codePage, til::size cellSizeInDIP) :
    FontInfoBase(
        std::wstring{},
        family,
        weight,
        codePage),
    _cellSizeInDIP{ static_cast<float>(cellSizeInDIP.width), static_cast<float>(cellSizeInDIP.height) }
{
    const auto size = wcsnlen_s(faceName, LF_FACESIZE);
    _faceName = { faceName, size };
    _validate();
}

const CellSizeInDIP& FontInfoDesired::GetEngineSize() const noexcept
{
    return _cellSizeInDIP;
}

float FontInfoDesired::GetFontSize() const noexcept
{
    return _fontSizeInPt;
}

bool FontInfoDesired::IsTrueTypeFont() const noexcept
{
    return WI_IsFlagSet(_family, TMPF_TRUETYPE);
}

// Populates a fixed-length **null-terminated** buffer with the name of this font, truncating it as necessary.
// Positions within the buffer that are not filled by the font name are zeroed.
void FontInfoDesired::FillLegacyNameBuffer(wchar_t (&buffer)[LF_FACESIZE]) const noexcept
{
    const auto toCopy = std::min(std::size(buffer) - 1, _faceName.size());
    const auto last = std::copy_n(_faceName.data(), toCopy, &buffer[0]);
    *last = L'\0';
}

// This helper determines if this object represents the default raster font. This can either be because internally we're
// using the empty facename and zeros for size, weight, and family, or it can be because we were given explicit
// dimensions from the engine that were the result of loading the default raster font. See GdiEngine::_GetProposedFont().
bool FontInfoDesired::IsDefaultRasterFont() const noexcept
{
    static constexpr CellSizeInDIP cellSizeEmpty{ 0, 0 };
    static constexpr CellSizeInDIP cellSizeRaster8x12{ 8, 12 };
    return _faceName.empty() && (_cellSizeInDIP == cellSizeEmpty || _cellSizeInDIP == cellSizeRaster8x12);
}

static Microsoft::Console::Render::IFontDefaultList* s_pFontDefaultList;

void FontInfoDesired::s_SetFontDefaultList(_In_ Microsoft::Console::Render::IFontDefaultList* const pFontDefaultList) noexcept
{
    s_pFontDefaultList = pFontDefaultList;
}

void FontInfoDesired::_validate()
{
    if (IsTrueTypeFont())
    {
        _cellSizeInDIP.width = 0; // Don't tell the engine about the width for a TrueType font. It makes a mess.
    }

    if (_faceName == DEFAULT_TT_FONT_FACENAME && s_pFontDefaultList != nullptr)
    {
        std::wstring defaultFontFace;
        if (SUCCEEDED(s_pFontDefaultList->RetrieveDefaultFontNameForCodepage(GetCodePage(), defaultFontFace)))
        {
            _faceName = defaultFontFace;

            // If we're assigning a default true type font name, make sure the family is also set to TrueType
            // to help GDI select the appropriate font when we actually create it.
            _family = TMPF_TRUETYPE;
        }
    }
}
