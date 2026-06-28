#pragma once

#include "WinCompat.h"

namespace ConsoleBidi
{

bool IsRTL(wchar_t wc);
bool CopyAndReorderLine(unsigned int cy, unsigned int cw, CHAR_INFO *line_buf);
bool CopyAndReorderLine(const CHAR_INFO *src, unsigned int src_w, unsigned int cw, CHAR_INFO *line_buf);
bool ReorderLine(CHAR_INFO *line, unsigned int cw, unsigned int *vis2log = nullptr);
void ExpandDirtyArea(SMALL_RECT &area);
unsigned int VisualColumnToLogical(unsigned int cy, unsigned int vis_x);
unsigned int LogicalColumnToVisual(unsigned int cy, unsigned int log_x, unsigned int cw);

} // namespace ConsoleBidi
