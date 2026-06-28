#include "ConsoleBidi.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <wctype.h>

#include "Backend.h"
#include "WinPort.h"
#include <utils.h>

namespace ConsoleBidi
{

static wchar_t CellBaseChar(const CHAR_INFO &ci)
{
	if (UNLIKELY(CI_USING_COMPOSITE_CHAR(ci))) {
		const wchar_t *pwc = WINPORT(CompositeCharLookup)(ci.Char.UnicodeChar);
		return pwc ? pwc[0] : 0;
	}
	return (wchar_t)ci.Char.UnicodeChar;
}

static bool IsNum(wchar_t wc)
{
	return (wc >= 0x0030 && wc <= 0x0039)   // ASCII digits
		|| (wc >= 0x0660 && wc <= 0x0669)   // Arabic-Indic digits
		|| (wc >= 0x06F0 && wc <= 0x06F9);  // Extended Arabic-Indic digits
}

static Class Classify(const CHAR_INFO &ci)
{
	const wchar_t wc = CellBaseChar(ci);
	if (wc == 0)
		return BIDI_NEUTRAL;
	if (IsRTL(wc))
		return BIDI_R;
	if (IsNum(wc))
		return BIDI_NUM;
	if (iswalpha((wint_t)wc))
		return BIDI_L;
	if (iswspace((wint_t)wc) || wc < 0x80)
		return BIDI_NEUTRAL;
	return BIDI_BOUND;
}

static void CopyConsoleLine(unsigned int cy, unsigned int cw, CHAR_INFO *line_buf)
{
	IConsoleOutput::DirectLineAccess dla(g_winport_con_out, cy);
	const CHAR_INFO *line = dla.Line();
	const unsigned int w = line ? std::min(dla.Width(), cw) : 0;
	if (w)
		memcpy(line_buf, line, w * sizeof(CHAR_INFO));
	if (w < cw)
		memset(line_buf + w, 0, (cw - w) * sizeof(CHAR_INFO));
}

bool IsRTL(wchar_t wc)
{
	return (wc >= 0x0590 && wc <= 0x05FF)   // Hebrew
		|| (wc >= 0x0600 && wc <= 0x07BF)   // Arabic, Syriac, Thaana, NKo
		|| (wc >= 0x0800 && wc <= 0x085F)   // Samaritan, Mandaic
		|| (wc >= 0x08A0 && wc <= 0x08FF)   // Arabic Extended-A
		|| (wc >= 0xFB1D && wc <= 0xFB4F)   // Hebrew presentation forms
		|| (wc >= 0xFB50 && wc <= 0xFDFF)   // Arabic presentation forms-A
		|| (wc >= 0xFE70 && wc <= 0xFEFF);  // Arabic presentation forms-B
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

	std::vector<Class> cls(cw);
	std::vector<uint8_t> levels(cw, 0);
	for (unsigned int i = 0; i < cw; ++i)
		cls[i] = Classify(line[i]);

	for (unsigned int i = 0; i < cw; ) {
		if (cls[i] == BIDI_R) {
			levels[i] = 1;
			++i;
		} else if (cls[i] == BIDI_BOUND) {
			levels[i] = 0;
			++i;
		} else {
			const unsigned int run_begin = i;
			while (i < cw && cls[i] != BIDI_R && cls[i] != BIDI_BOUND)
				++i;
			const bool left_rtl = (run_begin > 0 && cls[run_begin - 1] == BIDI_R);
			const bool right_rtl = (i < cw && cls[i] == BIDI_R);
			if (left_rtl && right_rtl) {
				bool has_ltr = false;
				for (unsigned int j = run_begin; j < i; ++j) {
					if (cls[j] == BIDI_L || cls[j] == BIDI_NUM) {
						has_ltr = true;
						break;
					}
				}
				const uint8_t run_level = has_ltr ? 2 : 1;
				for (unsigned int j = run_begin; j < i; ++j)
					levels[j] = run_level;
			}
		}
	}

	for (unsigned int lev = 2; lev >= 1; --lev) {
		for (unsigned int i = 0; i < cw; ) {
			if (levels[i] < lev) {
				++i;
				continue;
			}
			const unsigned int run_begin = i;
			while (i < cw && levels[i] >= lev)
				++i;
			unsigned int l = run_begin, r = i - 1;
			while (l < r) {
				std::swap(line[l], line[r]);
				std::swap(levels[l], levels[r]);
				if (vis2log)
					std::swap(vis2log[l], vis2log[r]);
				++l;
				--r;
			}
		}
	}
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

static bool RowHasRTL(unsigned int cy)
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
