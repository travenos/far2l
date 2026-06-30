#include "ConsoleBidi.h"

#include <fribidi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <wctype.h>

#include "Backend.h"
#include "WinPort.h"
#include <utils.h>

namespace ConsoleBidi
{
namespace
{

// BIDI_BOUND: UI/frame glyphs fixed to their cell (box drawing U+2500–U+25FF).
enum BidiClass : uint8_t { BIDI_L, BIDI_R, BIDI_NUM, BIDI_NEUTRAL, BIDI_BOUND };

wchar_t CellBaseChar(const CHAR_INFO &ci)
{
	if (UNLIKELY(CI_USING_COMPOSITE_CHAR(ci))) {
		const wchar_t *pwc = WINPORT(CompositeCharLookup)(ci.Char.UnicodeChar);
		return pwc ? pwc[0] : 0;
	}
	return (wchar_t)ci.Char.UnicodeChar;
}

static bool IsServiceGlyph(wchar_t wc)
{
	return (wc >= 0x2500 && wc <= 0x25FF);
}

BidiClass Classify(const CHAR_INFO &ci)
{
	const wchar_t wc = CellBaseChar(ci);
	if (wc == 0)
		return BIDI_NEUTRAL;
	if (IsRTL(wc))
		return BIDI_R;
	if ((wc >= 0x0030 && wc <= 0x0039)
		|| (wc >= 0x0660 && wc <= 0x0669)
		|| (wc >= 0x06F0 && wc <= 0x06F9))
		return BIDI_NUM;
	if (iswalpha((wint_t)wc))
		return BIDI_L;
	if (iswspace((wint_t)wc) || wc < 0x80)
		return BIDI_NEUTRAL;
	if (IsServiceGlyph(wc))
		return BIDI_BOUND;
	return BIDI_NEUTRAL;
}

static bool IsVisibleSpaceMark(wchar_t wc)
{
	switch (wc) {
	case 0x00B7:
	case 0x00B0:
	case 0x2420:
	case 0x2422:
		return true;
	default:
		return false;
	}
}

void CopyConsoleLine(unsigned int cy, unsigned int cw, CHAR_INFO *line_buf)
{
	IConsoleOutput::DirectLineAccess dla(g_winport_con_out, cy);
	const CHAR_INFO *line = dla.Line();
	const unsigned int w = line ? std::min(dla.Width(), cw) : 0;
	if (w)
		memcpy(line_buf, line, w * sizeof(CHAR_INFO));
	if (w < cw)
		memset(line_buf + w, 0, (cw - w) * sizeof(CHAR_INFO));
}

bool RowHasRTL(unsigned int cy)
{
	if (!g_winport_con_out)
		return false;

	unsigned int cw, ch;
	g_winport_con_out->GetSize(cw, ch);
	if (cy >= ch)
		return false;

	IConsoleOutput::DirectLineAccess dla(g_winport_con_out, cy);
	const CHAR_INFO *line = dla.Line();
	const unsigned int w = line ? std::min(dla.Width(), cw) : 0;
	for (unsigned int cx = 0; cx < w; ++cx) {
		if (IsRTL(CellBaseChar(line[cx])))
			return true;
	}
	return false;
}

static unsigned int TrimmedContentEnd(const CHAR_INFO *line, unsigned int cw)
{
	unsigned int end = cw;
	while (end > 0) {
		const wchar_t wc = CellBaseChar(line[end - 1]);
		if (wc == 0 || iswspace((wint_t)wc))
			--end;
		else
			break;
	}
	return end;
}

static void ApplySpanMap(CHAR_INFO *line, unsigned int span_begin, FriBidiStrIndex len,
	const FriBidiStrIndex *map, unsigned int *vis2log)
{
	std::vector<CHAR_INFO> cells((size_t)len);
	std::vector<unsigned int> maps((size_t)len);
	for (FriBidiStrIndex v = 0; v < len; ++v) {
		const unsigned int log = span_begin + (unsigned int)map[v];
		cells[(size_t)v] = line[log];
		maps[(size_t)v] = vis2log ? vis2log[log] : log;
	}
	for (FriBidiStrIndex v = 0; v < len; ++v) {
		line[span_begin + (unsigned int)v] = cells[(size_t)v];
		if (vis2log)
			vis2log[span_begin + (unsigned int)v] = maps[(size_t)v];
	}
}

static bool ReorderSpanWithFriBidi(CHAR_INFO *line, unsigned int span_begin, unsigned int span_end,
	unsigned int *vis2log)
{
	const FriBidiStrIndex len = (FriBidiStrIndex)(span_end - span_begin);
	if (len <= 0)
		return true;

	std::vector<FriBidiChar> text((size_t)len);
	std::vector<FriBidiCharType> bidi_types((size_t)len);
	std::vector<FriBidiLevel> levels((size_t)len);
	std::vector<FriBidiStrIndex> map((size_t)len);

	for (FriBidiStrIndex i = 0; i < len; ++i) {
		const wchar_t wc = CellBaseChar(line[span_begin + (unsigned int)i]);
		text[(size_t)i] = (FriBidiChar)wc;
		map[(size_t)i] = i;
	}

	fribidi_get_bidi_types(text.data(), len, bidi_types.data());

	for (FriBidiStrIndex i = 0; i < len; ++i) {
		if (IsVisibleSpaceMark((wchar_t)text[(size_t)i]))
			bidi_types[(size_t)i] = FRIBIDI_TYPE_WS;
	}

	// LTR paragraph everywhere — text stays left-aligned in panels and editor.
	// get_par_embedding_levels (no _ex): no BDI bracket pairing that scrambles "()[]".
	// reorder_line(0): no NSM reorder — appropriate for fixed-width console.
	FriBidiParType base_dir = FRIBIDI_PAR_LTR;
	if (!fribidi_get_par_embedding_levels(bidi_types.data(), len, &base_dir, levels.data()))
		return false;

	if (!fribidi_reorder_line(0, bidi_types.data(), len, 0, base_dir,
			levels.data(), nullptr, map.data()))
		return false;

	ApplySpanMap(line, span_begin, len, map.data(), vis2log);
	return true;
}

static void ReorderBidiSpans(CHAR_INFO *line, unsigned int content_end,
	const std::vector<BidiClass> &cls, unsigned int *vis2log)
{
	unsigned int i = 0;
	while (i < content_end) {
		while (i < content_end && cls[i] == BIDI_BOUND)
			++i;
		if (i >= content_end)
			break;

		const unsigned int span_begin = i;
		while (i < content_end && cls[i] != BIDI_BOUND)
			++i;

		ReorderSpanWithFriBidi(line, span_begin, i, vis2log);
	}
}

} // namespace

bool IsRTL(wchar_t wc)
{
	return (wc >= 0x0590 && wc <= 0x05FF)
		|| (wc >= 0x0600 && wc <= 0x07BF)
		|| (wc >= 0x0800 && wc <= 0x085F)
		|| (wc >= 0x08A0 && wc <= 0x08FF)
		|| (wc >= 0xFB1D && wc <= 0xFB4F)
		|| (wc >= 0xFB50 && wc <= 0xFDFF)
		|| (wc >= 0xFE70 && wc <= 0xFEFF);
}

bool ReorderLine(CHAR_INFO *line, unsigned int cw, unsigned int *vis2log)
{
	if (vis2log) {
		for (unsigned int i = 0; i < cw; ++i)
			vis2log[i] = i;
	}

	bool has_rtl = false;
	for (unsigned int i = 0; i < cw; ++i) {
		if (Classify(line[i]) == BIDI_R) {
			has_rtl = true;
			break;
		}
	}
	if (!has_rtl)
		return false;

	std::vector<BidiClass> cls(cw);
	for (unsigned int i = 0; i < cw; ++i)
		cls[i] = Classify(line[i]);

	const unsigned int content_end = TrimmedContentEnd(line, cw);
	if (content_end == 0)
		return true;

	ReorderBidiSpans(line, content_end, cls, vis2log);
	return true;
}

bool CopyAndReorderLine(unsigned int cy, unsigned int cw, CHAR_INFO *line_buf)
{
	CopyConsoleLine(cy, cw, line_buf);
	return ReorderLine(line_buf, cw, nullptr);
}

bool CopyAndReorderLine(const CHAR_INFO *src, unsigned int src_w, unsigned int cw, CHAR_INFO *line_buf)
{
	const unsigned int copy_w = (src && src_w) ? std::min(src_w, cw) : 0;
	if (copy_w)
		memcpy(line_buf, src, copy_w * sizeof(CHAR_INFO));
	if (copy_w < cw)
		memset(line_buf + copy_w, 0, (cw - copy_w) * sizeof(CHAR_INFO));
	return ReorderLine(line_buf, cw, nullptr);
}

void ExpandDirtyArea(SMALL_RECT &area)
{
	if (!g_winport_con_out)
		return;

	unsigned int cw, ch;
	g_winport_con_out->GetSize(cw, ch);
	for (int cy = area.Top; cy <= area.Bottom; ++cy) {
		if (cy < 0 || (unsigned)cy >= ch)
			continue;
		if (RowHasRTL((unsigned int)cy)) {
			area.Left = 0;
			area.Right = (SHORT)(cw - 1);
			break;
		}
	}
}

unsigned int VisualColumnToLogical(unsigned int cy, unsigned int vis_x)
{
	if (!g_winport_con_out)
		return vis_x;

	unsigned int cw, ch;
	g_winport_con_out->GetSize(cw, ch);
	if (cy >= ch || vis_x >= cw)
		return vis_x;

	std::vector<CHAR_INFO> tmp(cw);
	CopyConsoleLine(cy, cw, &tmp[0]);

	std::vector<unsigned int> vis2log(cw);
	if (!ReorderLine(&tmp[0], cw, &vis2log[0]))
		return vis_x;

	return vis2log[vis_x];
}

unsigned int LogicalColumnToVisual(unsigned int cy, unsigned int log_x, unsigned int cw)
{
	if (!g_winport_con_out || log_x >= cw)
		return log_x;

	std::vector<CHAR_INFO> tmp(cw);
	CopyConsoleLine(cy, cw, &tmp[0]);

	std::vector<unsigned int> vis2log(cw);
	if (!ReorderLine(&tmp[0], cw, &vis2log[0]))
		return log_x;

	for (unsigned int v = 0; v < cw; ++v) {
		if (vis2log[v] == log_x)
			return v;
	}
	return log_x;
}

} // namespace ConsoleBidi
