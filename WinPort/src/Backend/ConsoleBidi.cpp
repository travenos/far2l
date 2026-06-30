#include "ConsoleBidi.h"

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
// BIDI_NEUTRAL: whitespace, punctuation, editor visible-space marks (· ° …).
enum BidiClass : uint8_t { BIDI_L, BIDI_R, BIDI_NUM, BIDI_NEUTRAL, BIDI_BOUND };

wchar_t CellBaseChar(const CHAR_INFO &ci)
{
	if (UNLIKELY(CI_USING_COMPOSITE_CHAR(ci))) {
		const wchar_t *pwc = WINPORT(CompositeCharLookup)(ci.Char.UnicodeChar);
		return pwc ? pwc[0] : 0;
	}
	return (wchar_t)ci.Char.UnicodeChar;
}

bool IsNum(wchar_t wc)
{
	return (wc >= 0x0030 && wc <= 0x0039)   // ASCII digits
		|| (wc >= 0x0660 && wc <= 0x0669)   // Arabic-Indic digits
		|| (wc >= 0x06F0 && wc <= 0x06F9);  // Extended Arabic-Indic digits
}

// Panel/frame glyphs that must stay in their cell (box drawing, blocks, ░▒▓█…).
// Other non-letter symbols — including editor "show whitespace" marks (· ° ␠ …) —
// are neutral and participate in the surrounding RTL/LTR run like spaces.
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
	if (IsNum(wc))
		return BIDI_NUM;
	if (iswalpha((wint_t)wc))
		return BIDI_L;
	if (iswspace((wint_t)wc) || wc < 0x80)
		return BIDI_NEUTRAL;
	if (IsServiceGlyph(wc))
		return BIDI_BOUND;
	return BIDI_NEUTRAL;
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

static bool IsSuffixCell(BidiClass c)
{
	return c == BIDI_L || c == BIDI_NUM || c == BIDI_NEUTRAL;
}

static unsigned TrimmedContentEnd(const CHAR_INFO *line, unsigned int cw)
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

// Trailing LTR suffix is for mixed content (e.g. "… 2", "… Alex"), not EOL padding.
static bool SuffixHasMeaningfulContent(const CHAR_INFO *line,
	const std::vector<BidiClass> &cls, unsigned int begin, unsigned int end)
{
	for (unsigned int i = begin; i < end; ++i) {
		if (cls[i] == BIDI_L || cls[i] == BIDI_NUM)
			return true;
		if (cls[i] == BIDI_NEUTRAL) {
			const wchar_t wc = CellBaseChar(line[i]);
			if (wc != 0 && !iswspace((wint_t)wc))
				return true;
		}
	}
	return false;
}

// Editor / dictionary lines use "… - …" after RTL; keep logical order and alignment.
static bool SuffixLooksLikeTranslation(const CHAR_INFO *line,
	unsigned int begin, unsigned int end)
{
	for (unsigned int i = begin + 1; i + 1 < end; ++i) {
		if (CellBaseChar(line[i]) == L'-'
			&& iswspace((wint_t)CellBaseChar(line[i - 1]))
			&& iswspace((wint_t)CellBaseChar(line[i + 1])))
			return true;
	}
	return false;
}

static bool FindTrailingSuffix(const CHAR_INFO *line, const std::vector<BidiClass> &cls,
	const std::vector<uint8_t> &levels, unsigned int cw, unsigned int last_r,
	unsigned int &suffix_begin, unsigned int &suffix_end)
{
	suffix_begin = suffix_end = last_r + 1;
	if (last_r + 1 >= cw)
		return false;

	const unsigned int content_end = TrimmedContentEnd(line, cw);
	if (last_r + 1 >= content_end)
		return false;

	unsigned int i = last_r + 1;
	for (; i < content_end; ++i) {
		if (cls[i] == BIDI_BOUND || cls[i] == BIDI_R)
			break;
		if (levels[i] == 2)
			break;
		if (cls[i] == BIDI_NEUTRAL && iswspace((wint_t)CellBaseChar(line[i]))) {
			unsigned int j = i;
			while (j < content_end && cls[j] == BIDI_NEUTRAL
				&& iswspace((wint_t)CellBaseChar(line[j])))
				++j;
			if (j - i >= 2 && j < content_end && cls[j] != BIDI_NEUTRAL)
				break;
			if (j - i >= 2 && j < content_end && cls[j] == BIDI_NEUTRAL
				&& CellBaseChar(line[j]) != 0
				&& !iswspace((wint_t)CellBaseChar(line[j])))
				break;
		}
		if (!IsSuffixCell(cls[i]))
			break;
	}
	if (i <= last_r + 1)
		return false;
	suffix_begin = last_r + 1;
	suffix_end = i;
	if (!SuffixHasMeaningfulContent(line, cls, suffix_begin, suffix_end))
		return false;
	return !SuffixLooksLikeTranslation(line, suffix_begin, suffix_end);
}

static unsigned int RtlRunBegin(const std::vector<BidiClass> &cls,
	const std::vector<uint8_t> &levels, unsigned int last_r)
{
	unsigned int rtl_begin = last_r;
	while (rtl_begin > 0) {
		const unsigned int p = rtl_begin - 1;
		if (cls[p] == BIDI_R || (cls[p] == BIDI_NEUTRAL && levels[p] == 1))
			rtl_begin = p;
		else
			break;
	}
	return rtl_begin;
}

static void ReorderSuffixTokens(const std::vector<CHAR_INFO> &in,
	const std::vector<unsigned int> &in_map, const std::vector<BidiClass> &cls,
	unsigned int suffix_begin, std::vector<CHAR_INFO> &out,
	std::vector<unsigned int> &out_map)
{
	struct SuffixTok { unsigned off, len; };
	const unsigned int len_s = (unsigned int)in.size();
	std::vector<SuffixTok> toks;
	unsigned int i = 0;
	while (i < len_s) {
		const unsigned int b = i;
		const BidiClass c = cls[suffix_begin + i];
		if (c == BIDI_L || c == BIDI_NUM) {
			while (i < len_s && cls[suffix_begin + i] == c)
				++i;
		} else {
			while (i < len_s && cls[suffix_begin + i] == BIDI_NEUTRAL)
				++i;
		}
		toks.push_back({b, i - b});
	}
	out.clear();
	out_map.clear();
	out.reserve(len_s);
	out_map.reserve(len_s);
	for (int t = (int)toks.size() - 1; t >= 0; --t) {
		for (unsigned int j = 0; j < toks[(unsigned int)t].len; ++j) {
			out.push_back(in[toks[(unsigned int)t].off + j]);
			out_map.push_back(in_map[toks[(unsigned int)t].off + j]);
		}
	}
}

static void PlaceSuffixBeforeRtl(CHAR_INFO *line, const std::vector<BidiClass> &cls,
	unsigned int rtl_begin, unsigned int last_r, unsigned int suffix_begin,
	unsigned int suffix_end, unsigned int *vis2log)
{
	const unsigned int len_h = last_r - rtl_begin + 1;
	const unsigned int len_s = suffix_end - suffix_begin;
	std::vector<CHAR_INFO> buf_h(len_h), buf_s(len_s), buf_s_vis;
	std::vector<unsigned int> map_h(len_h), map_s(len_s), map_s_vis;

	for (unsigned int i = 0; i < len_h; ++i) {
		buf_h[i] = line[rtl_begin + i];
		map_h[i] = vis2log ? vis2log[rtl_begin + i] : rtl_begin + i;
	}
	for (unsigned int i = 0; i < len_s; ++i) {
		buf_s[i] = line[suffix_begin + i];
		map_s[i] = vis2log ? vis2log[suffix_begin + i] : suffix_begin + i;
	}
	// Logical suffix is "… 2" / " Alex"; visual order before RTL is "2 …" / "Alex …".
	ReorderSuffixTokens(buf_s, map_s, cls, suffix_begin, buf_s_vis, map_s_vis);

	unsigned int dst = rtl_begin;
	for (unsigned int i = 0; i < buf_s_vis.size(); ++i) {
		line[dst] = buf_s_vis[i];
		if (vis2log)
			vis2log[dst] = map_s_vis[i];
		++dst;
	}
	for (unsigned int i = 0; i < len_h; ++i) {
		line[dst] = buf_h[i];
		if (vis2log)
			vis2log[dst] = map_h[i];
		++dst;
	}
}

} // namespace

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

	std::vector<BidiClass> cls(cw);
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
				uint8_t run_level = 2;
				if (!has_ltr) {
					// Single-space gaps join Hebrew words; wide gaps (editor columns) stay level 0.
					unsigned int max_consec_ws = 0, cur_ws = 0;
					for (unsigned int j = run_begin; j < i; ++j) {
						if (cls[j] == BIDI_NEUTRAL
							&& iswspace((wint_t)CellBaseChar(line[j]))) {
							++cur_ws;
							if (cur_ws > max_consec_ws)
								max_consec_ws = cur_ws;
						} else {
							cur_ws = 0;
						}
					}
					if (max_consec_ws >= 2)
						continue;
					run_level = 1;
				}
				for (unsigned int j = run_begin; j < i; ++j)
					levels[j] = run_level;
			}
		}
	}

	unsigned int last_r = (unsigned int)-1;
	for (unsigned int i = 0; i < cw; ++i) {
		if (cls[i] == BIDI_R)
			last_r = i;
	}
	unsigned int suffix_begin = 0, suffix_end = 0;
	const bool has_suffix = (last_r != (unsigned int)-1)
		&& FindTrailingSuffix(line, cls, levels, cw, last_r, suffix_begin, suffix_end);

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

	if (has_suffix) {
		unsigned int rtl_begin = RtlRunBegin(cls, levels, last_r);
		unsigned int eff_suffix_begin = suffix_begin;
		while (eff_suffix_begin > rtl_begin
			&& cls[eff_suffix_begin - 1] == BIDI_NEUTRAL
			&& levels[eff_suffix_begin - 1] == 1)
			--eff_suffix_begin;
		PlaceSuffixBeforeRtl(line, cls, rtl_begin, last_r, eff_suffix_begin, suffix_end, vis2log);
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
