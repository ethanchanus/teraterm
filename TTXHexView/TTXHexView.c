/* TTXHexView -- show a hex view of raw send/receive traffic, in a
 * companion window that stays aligned beside the main terminal window.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "teraterm.h"
#include "tttypes.h"
#include "ttplugin.h"
#include "tt_res.h"

#include <windows.h>
#define _RICHEDIT_VER 0x0500
#include <richedit.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ORDER 8500

/* File menu command id (must be unique across all loaded TTX plugins) */
#define ID_MENU_HEXVIEW  55950

/* child control ids, local to the hex view window only */
#define ID_BTN_GROUP4    61101
#define ID_BTN_GROUP8    61102
#define ID_BTN_GROUP16   61104
#define ID_BTN_GROUP32   61103
#define ID_EDIT_HEXVIEW  61110

#define HEXVIEW_CLASS_NAME L"TTXHexViewWindow"

/* the companion window's absolute minimum width, in pixels - must always
   fit all 4 grouping buttons (see WM_SIZE) without clipping/overlap; the
   actual default/live minimum is whichever is larger between this and
   ComputeRequiredPanelWidth() for the current grouping (see WM_GETMINMAXINFO) */
#define HEXVIEW_MIN_WIDTH      280

/* how many raw received bytes are retained for redisplay (the oldest
   half is evicted and the view fully rebuilt once this is exceeded) */
#define HEXVIEW_MAX_BYTES 65536

/* how many byte-index <-> terminal-line "marks" (one per line break) are
   retained, and how many occurrences of a repeated selected text are
   considered when disambiguating which one the terminal selection means */
#define HEXVIEW_MAX_LINE_MARKS 4096
#define HEXVIEW_MAX_MATCHES 64

/* selection-highlight polling */
#define HEXVIEW_TIMER_SELECTION 1
#define HEXVIEW_TIMER_INTERVAL_MS 300
#define HEXVIEW_SELBUF_LEN 4096
#define HEXVIEW_HIGHLIGHT_COLOR RGB(255, 255, 153) /* light yellow, hex column */
#define HEXVIEW_HIGHLIGHT_ASCII_COLOR RGB(144, 238, 144) /* light green, ascii column */

/* the hex-dump text's font: shared between WM_CREATE's EM_SETCHARFORMAT
   (which actually renders the text) and ComputeRequiredPanelWidth (which
   must measure with the SAME font, or its width calculation drifts from
   what the control actually displays) */
#define HEXVIEW_CONTENT_FONT_FACE L"Consolas"
#define HEXVIEW_CONTENT_FONT_TWIPS 180 /* 9pt (twips = 1/20 point) */

/* per-column text colors, one palette per theme, so the offset/hex/ascii
   columns stay visually distinct and readable whether Windows is set to
   its light ("day") or dark ("night") app theme (see IsSystemDarkMode).
   The dark palette mirrors common dark-editor colors (e.g. VS Code's
   default dark theme) for proven readability against a dark background. */
#define HEXVIEW_LIGHT_BKGND  RGB(255, 255, 255)
#define HEXVIEW_LIGHT_OFFSET RGB(120, 120, 120) /* muted gray - de-emphasized */
#define HEXVIEW_LIGHT_HEX    RGB(0, 0, 0)
#define HEXVIEW_LIGHT_ASCII  RGB(0, 90, 190)    /* medium blue */
#define HEXVIEW_DARK_BKGND   RGB(30, 30, 30)
#define HEXVIEW_DARK_OFFSET  RGB(133, 133, 133)
#define HEXVIEW_DARK_HEX     RGB(212, 212, 212)
#define HEXVIEW_DARK_ASCII   RGB(86, 156, 214)  /* light blue */

static HANDLE hInst; /* Instance handle of TTX*.DLL */
static HMODULE hRichEditDll;

typedef struct {
	PTTSet ts;
	PComVar cv;

	/* functions imported from the core (used for the selection highlight) */
	const TTXImports *imports;

	/* hooked functions (recv/read only: the hex view shows received data only) */
	Trecv origPrecv;
	TReadFile origPReadFile;

	/* menu */
	HMENU FileMenu;

	/* hex view companion window, a separate top-level window owned by
	   the main window (not a child) */
	HWND HexWin;
	HWND EditCtrl;
	HWND Btn4;
	HWND Btn8;
	HWND Btn16;
	HWND Btn32;
	HFONT hFont;
	BOOL visible;
	int groupSize;   /* 4, 8, 16 or 32 bytes per line */
	int panelWidth;  /* current window width, in pixels */
	BOOL darkMode;   /* last-detected Windows app theme (see IsSystemDarkMode) */

	/* main window subclass, used to keep the panel aligned beside it */
	BOOL subclassed;
	WNDPROC origMainWndProc;

	/* true while this plugin is itself repositioning the main window or the
	   hex view window, to avoid the two windows' position-sync feeding back
	   into each other */
	BOOL syncing;
	/* the hex view window's last known position, used to compute how far the
	   user just dragged it so the same delta can be applied to the main window */
	int lastLeft;
	int lastTop;

	/* terminal-selection highlight tracking */
	BOOL highlightActive;
	wchar_t lastSelBuf[HEXVIEW_SELBUF_LEN];

	/* captured received bytes, as one continuous stream (not split by how
	   many bytes happened to arrive in each individual recv()/ReadFile()
	   call), so the hex dump wraps into fixed-width rows exactly like a
	   normal hex editor instead of starting a new row per system call */
	int byteCount;
	BYTE byteBuf[HEXVIEW_MAX_BYTES];

	/* approximate byte-index -> terminal absolute-line-number mapping, used
	   to tell which occurrence of a repeated selected text corresponds to
	   the terminal's actual selection when the same text appears more than
	   once. localLine counts LF bytes seen so far (Tera Term's own line
	   counter also advances once per line feed); lineMark* records the byte
	   index where each line starts, alongside localLine's value there.
	   lineOffset (once calibrated against an unambiguous match) converts a
	   localLine value to Tera Term's own absolute SelectStart.y numbering. */
	int localLine;
	int lineMarkByteIdx[HEXVIEW_MAX_LINE_MARKS];
	int lineMarkLocalLine[HEXVIEW_MAX_LINE_MARKS];
	int lineMarkCount;
	int lineOffset;
	BOOL lineOffsetKnown;

	/* terminal-scroll -> hex-view-scroll following: a pure delta tracker
	   (no calibration needed, unlike lineOffset above - a constant offset
	   between two monotonic counters cancels out of any delta between two
	   readings of each), so it works from the very first scroll, without
	   needing a prior text selection first */
	BOOL topLineKnown;
	int lastTerminalTopLine;
	int hexViewTopLocalLine;
} TInstVar;

typedef TInstVar *PTInstVar;
static PTInstVar pvar;
static TInstVar InstVar;

#define GetFileMenu(menu) GetSubMenuByChildID(menu, ID_FILE_NEWCONNECTION)

static HMENU GetSubMenuByChildID(HMENU menu, UINT id)
{
	int i, j, items, subitems, cur_id;
	HMENU m;

	items = GetMenuItemCount(menu);
	for (i = 0; i < items; i++) {
		m = GetSubMenu(menu, i);
		if (m != NULL) {
			subitems = GetMenuItemCount(m);
			for (j = 0; j < subitems; j++) {
				cur_id = GetMenuItemID(m, j);
				if (cur_id == id) {
					return m;
				}
			}
		}
	}
	return NULL;
}

/*
 * ---- hex formatting / rendering ----
 */

/*
 * ---- hex formatting / rendering ----
 *
 * The received bytes are rendered as one continuous hex dump: a row is
 * exactly `groupSize` bytes wide and is only closed once it is full,
 * regardless of how many separate recv()/ReadFile() calls contributed
 * bytes to it. This is what keeps single characters typed interactively
 * (one byte per system call) packed on the same row instead of each one
 * starting its own line.
 */

/* character width of one *complete* (fully-filled) row, for a given group
   size: "%08X  " + perLine*"XX " + " " + perLine ascii chars + "\r\n" -
   used only to size/format our own off-control wchar_t buffer; NOT valid
   for computing EM_SETSEL positions against the live control (see
   RowStartChar) */
static int RowCharWidth(int perLine)
{
	return 10 + perLine * 3 + 1 + perLine + 2;
}

/* character index, in the live control's own text, of the start of row
   `row`. Once text has been handed to a RichEdit control via
   SetWindowTextW, the control collapses each "\r\n" line ending we wrote
   into a single internal paragraph-break character, so a plain
   `row * RowCharWidth(perLine)` formula undercounts by one character per
   row already crossed (row 1 short by 1, row 2 short by 2, ...). Asking
   the control directly via EM_LINEINDEX sidesteps that entirely. */
static int RowStartChar(int row)
{
	return (int)SendMessageW(pvar->EditCtrl, EM_LINEINDEX, (WPARAM)row, 0);
}

/* character offset, within the live control's text, of byte `idx`'s hex
   token */
static int HexCharOffset(int idx, int perLine)
{
	int col = idx % perLine;

	return RowStartChar(idx / perLine) + 10 + col * 3;
}

/* character offset, within the live control's text, of byte `idx`'s
   ascii char */
static int AsciiCharOffset(int idx, int perLine)
{
	int col = idx % perLine;

	return RowStartChar(idx / perLine) + 10 + perLine * 3 + 1 + col;
}

/* format byteBuf[fromIdx .. toIdx) as hex-dump rows */
static wchar_t *FormatRows(int fromIdx, int toIdx, int perLine)
{
	int total = toIdx - fromIdx;
	int lines;
	size_t cap;
	wchar_t *out;
	int pos, line;

	if (total <= 0) {
		return NULL;
	}
	lines = (total + perLine - 1) / perLine;
	cap = (size_t)(lines + 1) * (size_t)RowCharWidth(perLine) + 64;
	out = (wchar_t *)malloc(cap * sizeof(wchar_t));
	if (out == NULL) {
		return NULL;
	}

	pos = 0;
	for (line = 0; line < lines; line++) {
		int base = fromIdx + line * perLine;
		int n = toIdx - base;
		int i;

		if (n > perLine) {
			n = perLine;
		}

		pos += swprintf_s(out + pos, cap - pos, L"%08X  ", base);
		for (i = 0; i < perLine; i++) {
			if (i < n) {
				pos += swprintf_s(out + pos, cap - pos, L"%02X ", pvar->byteBuf[base + i]);
			} else {
				pos += swprintf_s(out + pos, cap - pos, L"   ");
			}
		}
		pos += swprintf_s(out + pos, cap - pos, L" ");
		for (i = 0; i < n; i++) {
			BYTE b = pvar->byteBuf[base + i];
			out[pos++] = (b >= 0x20 && b < 0x7F) ? (wchar_t)b : L'.';
		}
		out[pos++] = L'\r';
		out[pos++] = L'\n';
	}
	out[pos] = L'\0';

	return out;
}

/*
 * ---- day/night (light/dark) column colors ----
 */

/* Windows' own "light vs dark app" setting (Settings > Personalization >
   Colors); missing key/value (older Windows) is treated as light. */
static BOOL IsSystemDarkMode(void)
{
	HKEY hKey;
	DWORD value = 1, size = sizeof(value), type = 0;

	if (RegOpenKeyExW(HKEY_CURRENT_USER,
	                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
	                   0, KEY_READ, &hKey) != ERROR_SUCCESS) {
		return FALSE;
	}
	if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS ||
	    type != REG_DWORD) {
		value = 1;
	}
	RegCloseKey(hKey);
	return value == 0; /* AppsUseLightTheme: 0 = dark, 1 = light */
}

/* (re)detect the current theme and apply its background + baseline text
   color; called once at WM_CREATE and again on WM_SETTINGCHANGE so a live
   Windows theme switch is picked up without restarting Tera Term */
static void ApplyTheme(void)
{
	CHARFORMAT2W cf;

	if (pvar->EditCtrl == NULL) {
		return;
	}
	pvar->darkMode = IsSystemDarkMode();

	SendMessageW(pvar->EditCtrl, EM_SETBKGNDCOLOR, 0,
	             (LPARAM)(pvar->darkMode ? HEXVIEW_DARK_BKGND : HEXVIEW_LIGHT_BKGND));

	ZeroMemory(&cf, sizeof(cf));
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_COLOR;
	cf.crTextColor = pvar->darkMode ? HEXVIEW_DARK_HEX : HEXVIEW_LIGHT_HEX;
	SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
}

/* Color each row's offset/hex/ascii columns per the current theme. Called
   after every RenderAll() rebuild, since SetWindowTextW's fresh text always
   needs its per-column colors re-applied. The hex-bytes column is left at
   the document's baseline color (set by ApplyTheme) since it's the
   majority of each row's text - only the offset label and ascii columns
   are overridden here, one row at a time (RichEdit selections are a single
   contiguous range, so per-column coloring can't be done in one call). */
static void ApplyColumnColors(void)
{
	CHARFORMAT2W cf;
	int perLine = pvar->groupSize;
	int totalRows, row;
	COLORREF offsetColor = pvar->darkMode ? HEXVIEW_DARK_OFFSET : HEXVIEW_LIGHT_OFFSET;
	COLORREF asciiColor = pvar->darkMode ? HEXVIEW_DARK_ASCII : HEXVIEW_LIGHT_ASCII;

	if (pvar->EditCtrl == NULL || pvar->byteCount <= 0) {
		return;
	}

	ZeroMemory(&cf, sizeof(cf));
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_COLOR;

	totalRows = (pvar->byteCount + perLine - 1) / perLine;
	for (row = 0; row < totalRows; row++) {
		int rowFirstByte = row * perLine;
		int n = pvar->byteCount - rowFirstByte;
		int rowStart = RowStartChar(row);
		int asciiStart = AsciiCharOffset(rowFirstByte, perLine);

		if (n > perLine) {
			n = perLine;
		}

		SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)rowStart, (LPARAM)(rowStart + 8));
		cf.crTextColor = offsetColor;
		SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

		SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)asciiStart, (LPARAM)(asciiStart + n));
		cf.crTextColor = asciiColor;
		SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
	}
}

/* Fully regenerate the hex dump from byteBuf every time it changes.
   (An earlier version tried to patch only the trailing/incomplete row
   in place via EM_SETSEL/EM_REPLACESEL, computing the patch's start
   position either from a from-scratch character-offset formula or from a
   self-tracked "previous pending row length" delta. Both approaches
   drifted and corrupted the text - richedit's own internal character
   counting for what we'd inserted did not reliably match what we assumed
   or tracked, and errors compounded across many rapid single-byte
   appends (e.g. interactive keystroke echo). A full SetWindowTextW()
   rebuild has no position bookkeeping to get wrong, and is cheap enough
   at this control's realistic text size that it is not worth the risk.) */
static void RenderAll(void)
{
	wchar_t *text;
	CHARRANGE cr;
	BOOL hadSelection;

	if (pvar->EditCtrl == NULL) {
		return;
	}

	pvar->highlightActive = FALSE;
	pvar->lastSelBuf[0] = L'\0';

	/* preserve the user's own text selection (e.g. mid Ctrl+C) across the
	   rebuild below, instead of always dropping it and jumping to the end -
	   our own programmatic highlight never leaves a real selection behind
	   (see ApplyHighlight/ClearHighlight), so any live selection here is
	   necessarily the user's */
	SendMessageW(pvar->EditCtrl, EM_EXGETSEL, 0, (LPARAM)&cr);
	hadSelection = (cr.cpMin != cr.cpMax);

	text = FormatRows(0, pvar->byteCount, pvar->groupSize);
	SetWindowTextW(pvar->EditCtrl, (text != NULL) ? text : L"");
	free(text);

	ApplyColumnColors();

	if (hadSelection && cr.cpMax <= GetWindowTextLengthW(pvar->EditCtrl)) {
		SendMessageW(pvar->EditCtrl, EM_EXSETSEL, 0, (LPARAM)&cr);
	} else {
		SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
		SendMessageW(pvar->EditCtrl, EM_SCROLLCARET, 0, 0);
	}
}

static void StoreChunk(const BYTE *data, int len)
{
	BOOL added = FALSE;
	int i;

	if (len <= 0) {
		return;
	}

	for (i = 0; i < len; i++) {
		BYTE b = data[i];

		/* NUL bytes are pure fill/padding: Tera Term's own parser silently
		   discards them and never displays anything for them, so drop them
		   here too, to keep the hex view matched 100% with the terminal */
		if (b == 0) {
			continue;
		}
		if (pvar->byteCount >= HEXVIEW_MAX_BYTES) {
			/* evict the oldest half to make room for new bytes, and rebase
			   (or drop) the line marks the same way */
			int keep = HEXVIEW_MAX_BYTES / 2;
			int drop = pvar->byteCount - keep;
			int w = 0, r;

			memmove(pvar->byteBuf, pvar->byteBuf + drop, (size_t)keep);
			pvar->byteCount = keep;

			for (r = 0; r < pvar->lineMarkCount; r++) {
				if (pvar->lineMarkByteIdx[r] >= drop) {
					pvar->lineMarkByteIdx[w] = pvar->lineMarkByteIdx[r] - drop;
					pvar->lineMarkLocalLine[w] = pvar->lineMarkLocalLine[r];
					w++;
				}
			}
			pvar->lineMarkCount = w;
		}
		pvar->byteBuf[pvar->byteCount++] = b;
		added = TRUE;

		if (b == '\n') {
			/* a new line begins right after this byte - record it, so a
			   later selection-highlight lookup can tell which line any
			   given byte belongs to (see LocalLineAtByte()) */
			pvar->localLine++;
			if (pvar->lineMarkCount < HEXVIEW_MAX_LINE_MARKS) {
				pvar->lineMarkByteIdx[pvar->lineMarkCount] = pvar->byteCount;
				pvar->lineMarkLocalLine[pvar->lineMarkCount] = pvar->localLine;
				pvar->lineMarkCount++;
			}
		}
	}

	if (!added) {
		return; /* every byte in this call was a filtered-out NUL */
	}

	if (pvar->HexWin != NULL && pvar->visible) {
		RenderAll();
	}
}

/*
 * ---- traffic capture hooks (TCP socket + serial/file handle) ----
 */

static int PASCAL TTXrecv(SOCKET s, char *buf, int len, int flags)
{
	int ret = pvar->origPrecv(s, buf, len, flags);
	if (ret > 0) {
		StoreChunk((const BYTE *)buf, ret);
	}
	return ret;
}

static void PASCAL TTXOpenTCP(TTXSockHooks *hooks)
{
	pvar->origPrecv = *hooks->Precv;
	*hooks->Precv = TTXrecv;
}

static void PASCAL TTXCloseTCP(TTXSockHooks *hooks)
{
	if (pvar->origPrecv) {
		*hooks->Precv = pvar->origPrecv;
	}
}

static BOOL PASCAL TTXReadFile(HANDLE fh, LPVOID buff, DWORD len, LPDWORD rbytes, LPOVERLAPPED rol)
{
	BOOL ret = pvar->origPReadFile(fh, buff, len, rbytes, rol);
	if (ret && rbytes != NULL && *rbytes > 0) {
		StoreChunk((const BYTE *)buff, (int)*rbytes);
	}
	return ret;
}

static void PASCAL TTXOpenFile(TTXFileHooks *hooks)
{
	pvar->origPReadFile = *hooks->PReadFile;
	*hooks->PReadFile = TTXReadFile;
}

static void PASCAL TTXCloseFile(TTXFileHooks *hooks)
{
	if (pvar->origPReadFile) {
		*hooks->PReadFile = pvar->origPReadFile;
	}
}

/*
 * ---- hex view companion window ----
 */

static void PositionHexViewDefault(HWND mainHWin); /* fwd decl: SetGroupSize re-aligns the panel after resizing it */

#define HEXVIEW_INI_SECTION L"TTXHexView"

/* persist the user's last-selected grouping across restarts (teraterm.ini) */
static void SaveGroupSize(int size)
{
	wchar_t buf[16];

	if (pvar->ts == NULL || pvar->ts->SetupFNameW == NULL) {
		return;
	}
	_itow_s(size, buf, _countof(buf), 10);
	WritePrivateProfileStringW(HEXVIEW_INI_SECTION, L"GroupSize", buf, pvar->ts->SetupFNameW);
}

static int LoadGroupSize(void)
{
	int size;

	if (pvar->ts == NULL || pvar->ts->SetupFNameW == NULL) {
		return 8;
	}
	size = (int)GetPrivateProfileIntW(HEXVIEW_INI_SECTION, L"GroupSize", 8, pvar->ts->SetupFNameW);
	if (size != 4 && size != 8 && size != 16 && size != 32) {
		size = 8; /* corrupt/foreign value - fall back to the default */
	}
	return size;
}

/* the window width (screen pixels) needed so a full `perLine`-byte row
   (offset label + hex bytes + ascii) is entirely visible with no
   horizontal clipping/scrolling, for the same font/style as the edit
   control (see WM_CREATE) - computed from scratch so it works even
   before the hex-view window/font exist yet (e.g. at plugin init) */
/* the window width (screen pixels) needed so a full `perLine`-byte row
   (offset label + hex bytes + ascii) is entirely visible with no
   horizontal clipping/scrolling - measured using the EXACT font the edit
   control's TEXT is actually rendered with (HEXVIEW_CONTENT_FONT_*, applied
   via EM_SETCHARFORMAT in WM_CREATE; note this is unrelated to pvar->hFont,
   which is only ever applied to the toolbar buttons) - computed from
   scratch so it works even before the hex-view window/font exist yet
   (e.g. at plugin init) */
static int ComputeRequiredPanelWidth(int perLine)
{
	HFONT font, oldFont;
	HDC dc;
	SIZE sz;
	int visibleChars = 10 + perLine * 3 + 1 + perLine; /* offset label + hex bytes + separator + ascii */
	int pointSize = HEXVIEW_CONTENT_FONT_TWIPS / 20;
	wchar_t sample[210];
	RECT r;
	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
	DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_COMPOSITED;
	int clientWidth, width;

	visibleChars += 2; /* small safety margin against GDI-vs-richedit metric rounding */
	if (visibleChars >= _countof(sample)) {
		visibleChars = _countof(sample) - 1;
	}
	wmemset(sample, L'0', (size_t)visibleChars);
	sample[visibleChars] = L'\0';

	dc = CreateCompatibleDC(NULL);
	font = CreateFontW(-MulDiv(pointSize, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL,
	                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                    DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, HEXVIEW_CONTENT_FONT_FACE);
	oldFont = (HFONT)SelectObject(dc, font);
	GetTextExtentPoint32W(dc, sample, visibleChars, &sz);
	SelectObject(dc, oldFont);
	DeleteDC(dc);
	DeleteObject(font);

	/* the richedit control's own client-edge border + vertical scrollbar,
	   on top of the row text itself */
	clientWidth = sz.cx + GetSystemMetrics(SM_CXEDGE) * 2 + GetSystemMetrics(SM_CXVSCROLL);

	SetRect(&r, 0, 0, clientWidth, 100);
	AdjustWindowRectEx(&r, style, FALSE, exStyle);
	width = r.right - r.left;

	return (width < HEXVIEW_MIN_WIDTH) ? HEXVIEW_MIN_WIDTH : width;
}

static void UpdateGroupButtons(void)
{
	if (pvar->Btn4 == NULL) {
		return;
	}
	EnableWindow(pvar->Btn4, pvar->groupSize != 4);
	EnableWindow(pvar->Btn8, pvar->groupSize != 8);
	EnableWindow(pvar->Btn16, pvar->groupSize != 16);
	EnableWindow(pvar->Btn32, pvar->groupSize != 32);
}

static void SetGroupSize(int newSize)
{
	if (pvar->groupSize == newSize) {
		return;
	}
	pvar->groupSize = newSize;
	pvar->panelWidth = ComputeRequiredPanelWidth(newSize);
	SaveGroupSize(newSize);
	UpdateGroupButtons();
	RenderAll();
	if (pvar->HexWin != NULL && pvar->visible && pvar->cv != NULL) {
		PositionHexViewDefault(pvar->cv->HWin);
	}
}

/*
 * ---- terminal-selection -> hex byte highlighting ----
 */

/* the local line number (see StoreChunk's LF tracking) active at byte
   index idx: the highest recorded lineMarkLocalLine whose byte index is
   <= idx, or 0 if idx is on the very first line */
static int LocalLineAtByte(int idx)
{
	int lo = 0, hi = pvar->lineMarkCount - 1, result = 0;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (pvar->lineMarkByteIdx[mid] <= idx) {
			result = pvar->lineMarkLocalLine[mid];
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return result;
}

/* the reverse of LocalLineAtByte: the byte index where local line `line`
   starts, i.e. the earliest recorded mark whose local line is >= line.
   If `line` is beyond every recorded mark (at/after the latest retained
   byte), returns byteCount (the live/bottom edge); if it is before every
   recorded mark (scrolled into history this instance already evicted -
   see StoreChunk), returns the earliest still-retained mark's byte index. */
static int ByteAtLocalLine(int line)
{
	int lo = 0, hi = pvar->lineMarkCount - 1;
	int result = pvar->byteCount;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (pvar->lineMarkLocalLine[mid] >= line) {
			result = pvar->lineMarkByteIdx[mid];
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}
	return result;
}

/* Poll the terminal's current top-of-viewport line and, whenever it
   changes, scroll the hex view by the same number of lines in the same
   direction - a pure delta (see hexViewTopLocalLine's comment), so no
   calibration/anchor is needed and it works from the very first scroll. */
static void CheckScrollSync(void)
{
	int topLine, delta, targetByteIdx, targetRow, rowStartChar, lineIdx, currentTopLineIdx;

	if (pvar->imports == NULL || pvar->EditCtrl == NULL || !pvar->visible) {
		return;
	}
	topLine = pvar->imports->GetTopLine();

	if (!pvar->topLineKnown) {
		pvar->lastTerminalTopLine = topLine;
		pvar->hexViewTopLocalLine = pvar->localLine; /* assume "currently at the bottom" */
		pvar->topLineKnown = TRUE;
		return;
	}
	if (topLine == pvar->lastTerminalTopLine) {
		return; /* terminal hasn't scrolled since the last poll */
	}
	delta = topLine - pvar->lastTerminalTopLine;
	pvar->lastTerminalTopLine = topLine;

	pvar->hexViewTopLocalLine += delta;
	if (pvar->hexViewTopLocalLine < 0) {
		pvar->hexViewTopLocalLine = 0;
	} else if (pvar->hexViewTopLocalLine > pvar->localLine) {
		pvar->hexViewTopLocalLine = pvar->localLine;
	}

	targetByteIdx = ByteAtLocalLine(pvar->hexViewTopLocalLine);
	targetRow = targetByteIdx / pvar->groupSize;
	rowStartChar = RowStartChar(targetRow);
	lineIdx = (int)SendMessageW(pvar->EditCtrl, EM_LINEFROMCHAR, (WPARAM)rowStartChar, 0);
	currentTopLineIdx = (int)SendMessageW(pvar->EditCtrl, EM_GETFIRSTVISIBLELINE, 0, 0);
	SendMessageW(pvar->EditCtrl, EM_LINESCROLL, 0, lineIdx - currentTopLineIdx);
}

static void ClearHighlight(void)
{
	CHARFORMAT2W cf;
	int len;

	if (pvar->EditCtrl == NULL) {
		return;
	}
	len = GetWindowTextLengthW(pvar->EditCtrl);
	SendMessageW(pvar->EditCtrl, EM_SETSEL, 0, (LPARAM)len);
	ZeroMemory(&cf, sizeof(cf));
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_BACKCOLOR;
	cf.dwEffects = CFE_AUTOBACKCOLOR;
	SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
	SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
}

static void ApplyHighlight(int startChar, int endChar, COLORREF color)
{
	CHARFORMAT2W cf;

	if (pvar->EditCtrl == NULL) {
		return;
	}
	SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)startChar, (LPARAM)endChar);
	ZeroMemory(&cf, sizeof(cf));
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_BACKCOLOR;
	cf.crBackColor = color;
	SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

	/* scroll so the highlighted range is visible, then drop the native
	   selection so only our background color shows */
	SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)startChar, (LPARAM)startChar);
	SendMessageW(pvar->EditCtrl, EM_SCROLLCARET, 0, 0);
	SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
}

/*
 * Poll the terminal's current selection and highlight the matching bytes
 * in the hex view. Best-effort: the terminal only reports the selected
 * TEXT (already decoded/rendered), not raw byte offsets, so the text is
 * re-encoded as UTF-8 and searched for within the retained received bytes.
 * If that text appears more than once, the terminal's reported selection
 * line (GetSelectionStartLine) is used - via a locally tracked byte-index
 * <-> line map - to pick the occurrence that is actually selected, rather
 * than always the first (earliest-received) one.
 */
static void CheckSelectionHighlight(void)
{
	wchar_t selBuf[HEXVIEW_SELBUF_LEN];
	char utf8[HEXVIEW_SELBUF_LEN];
	int selLen, utf8Len, j;
	int matches[HEXVIEW_MAX_MATCHES];
	int matchCount = 0;
	int terminalLine;
	int matchIdx;
	BOOL found = FALSE;

	if (pvar->imports == NULL || pvar->EditCtrl == NULL || !pvar->visible) {
		return;
	}

	selLen = pvar->imports->GetSelectedTextW(selBuf, _countof(selBuf));
	if (selLen <= 0) {
		if (pvar->highlightActive) {
			ClearHighlight();
			pvar->highlightActive = FALSE;
		}
		pvar->lastSelBuf[0] = L'\0';
		return;
	}

	if (wcscmp(selBuf, pvar->lastSelBuf) == 0) {
		return; /* selection unchanged since the last poll */
	}
	wcsncpy_s(pvar->lastSelBuf, _countof(pvar->lastSelBuf), selBuf, _TRUNCATE);

	utf8Len = WideCharToMultiByte(CP_UTF8, 0, selBuf, selLen, utf8, sizeof(utf8) - 1, NULL, NULL);
	if (utf8Len <= 0) {
		return;
	}

	for (j = 0; j + utf8Len <= pvar->byteCount; j++) {
		if (memcmp(pvar->byteBuf + j, utf8, (size_t)utf8Len) == 0) {
			if (matchCount < HEXVIEW_MAX_MATCHES) {
				matches[matchCount++] = j;
			}
		}
	}

	terminalLine = pvar->imports->GetSelectionStartLine();

	if (matchCount > 0) {
		matchIdx = matches[0]; /* fallback: earliest occurrence */

		if (matchCount == 1) {
			/* unambiguous: (re)calibrate how the locally-tracked line count
			   (LF count) relates to the terminal's own line numbering */
			if (terminalLine >= 0) {
				pvar->lineOffset = terminalLine - LocalLineAtByte(matches[0]);
				pvar->lineOffsetKnown = TRUE;
			}
		} else if (pvar->lineOffsetKnown && terminalLine >= 0) {
			/* ambiguous: pick whichever occurrence's line, once converted
			   with the calibrated offset, is closest to the terminal's
			   actual selection line */
			int expectedLocalLine = terminalLine - pvar->lineOffset;
			int bestDiff = abs(LocalLineAtByte(matches[0]) - expectedLocalLine);
			int k;

			for (k = 1; k < matchCount; k++) {
				int diff = abs(LocalLineAtByte(matches[k]) - expectedLocalLine);

				if (diff < bestDiff) {
					bestDiff = diff;
					matchIdx = matches[k];
				}
			}
		}

		{
			int lastByte = matchIdx + utf8Len - 1;
			int perLine = pvar->groupSize;
			int row = matchIdx / perLine;
			int lastRow = lastByte / perLine;

			ClearHighlight();
			/* highlight one row at a time: a match spanning multiple rows
			   must not select across the newline/offset-label in between,
			   or the two colors (and the offset label) bleed into each other */
			for (; row <= lastRow; row++) {
				int rowFirst = (row * perLine > matchIdx) ? row * perLine : matchIdx;
				int rowLast = (row * perLine + perLine - 1 < lastByte) ? row * perLine + perLine - 1 : lastByte;
				int hexStart = HexCharOffset(rowFirst, perLine);
				int hexEnd = HexCharOffset(rowLast, perLine) + 2; /* the last byte's 2 hex digits */
				int asciiStart = AsciiCharOffset(rowFirst, perLine);
				int asciiEnd = AsciiCharOffset(rowLast, perLine) + 1; /* the last byte's 1 ascii char */

				ApplyHighlight(hexStart, hexEnd, HEXVIEW_HIGHLIGHT_COLOR);
				ApplyHighlight(asciiStart, asciiEnd, HEXVIEW_HIGHLIGHT_ASCII_COLOR);
			}
			pvar->highlightActive = TRUE;
			found = TRUE;
		}
	}

	if (!found && pvar->highlightActive) {
		ClearHighlight();
		pvar->highlightActive = FALSE;
	}
}

static LRESULT CALLBACK HexViewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
		case WM_CREATE: {
			CHARFORMAT2W cf;

			pvar->hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			                           DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

			pvar->Btn4 = CreateWindowExW(0, L"BUTTON", L"4 bytes",
			                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			                             0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_BTN_GROUP4, hInst, NULL);
			pvar->Btn8 = CreateWindowExW(0, L"BUTTON", L"8 bytes",
			                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			                             0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_BTN_GROUP8, hInst, NULL);
			pvar->Btn16 = CreateWindowExW(0, L"BUTTON", L"16 bytes",
			                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			                              0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_BTN_GROUP16, hInst, NULL);
			pvar->Btn32 = CreateWindowExW(0, L"BUTTON", L"32 bytes",
			                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			                              0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_BTN_GROUP32, hInst, NULL);

			/* a plain EDIT control can't paint a per-range background color,
			   so use RichEdit instead (needed for the selection highlight) */
			if (hRichEditDll == NULL) {
				hRichEditDll = LoadLibraryW(L"Msftedit.dll");
			}
			pvar->EditCtrl = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
			                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
			                                 ES_MULTILINE | ES_READONLY,
			                                 0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_EDIT_HEXVIEW, hInst, NULL);
			SendMessageW(pvar->EditCtrl, EM_SETTARGETDEVICE, (WPARAM)NULL, 1); /* disable word-wrap */

			ZeroMemory(&cf, sizeof(cf));
			cf.cbSize = sizeof(cf);
			cf.dwMask = CFM_FACE | CFM_SIZE;
			cf.yHeight = HEXVIEW_CONTENT_FONT_TWIPS;
			wcscpy_s(cf.szFaceName, _countof(cf.szFaceName), HEXVIEW_CONTENT_FONT_FACE);
			SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);

			SendMessageW(pvar->Btn4, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);
			SendMessageW(pvar->Btn8, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);
			SendMessageW(pvar->Btn16, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);
			SendMessageW(pvar->Btn32, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);

			ApplyTheme();
			UpdateGroupButtons();
			RenderAll();
			return 0;
		}
		case WM_SIZE: {
			int cw = LOWORD(lParam);
			int ch = HIWORD(lParam);
			int bw = 64, bh = 24, pad = 4;
			HDWP dwp = BeginDeferWindowPos(5); /* batch the moves into one repaint pass */

			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn4, NULL, pad, pad, bw, bh, SWP_NOZORDER);
			}
			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn8, NULL, pad * 2 + bw, pad, bw, bh, SWP_NOZORDER);
			}
			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn16, NULL, pad * 3 + bw * 2, pad, bw, bh, SWP_NOZORDER);
			}
			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn32, NULL, pad * 4 + bw * 3, pad, bw, bh, SWP_NOZORDER);
			}
			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->EditCtrl, NULL, 0, bh + pad * 2, cw, ch - (bh + pad * 2), SWP_NOZORDER);
			}
			if (dwp != NULL) {
				EndDeferWindowPos(dwp);
			}
			return 0;
		}
		case WM_GETMINMAXINFO: {
			MINMAXINFO *mmi = (MINMAXINFO *)lParam;
			mmi->ptMinTrackSize.x = ComputeRequiredPanelWidth(pvar->groupSize);
			return 0;
		}
		case WM_SETTINGCHANGE:
			/* Windows broadcasts this for ANY setting change; "ImmersiveColorSet"
			   is the well-known marker for a light/dark app theme switch */
			if (lParam != 0 && wcscmp((const wchar_t *)lParam, L"ImmersiveColorSet") == 0) {
				ApplyTheme();
				RenderAll();
			}
			return 0;
		case WM_COMMAND:
			switch (LOWORD(wParam)) {
				case ID_BTN_GROUP4:
					SetGroupSize(4);
					break;
				case ID_BTN_GROUP8:
					SetGroupSize(8);
					break;
				case ID_BTN_GROUP16:
					SetGroupSize(16);
					break;
				case ID_BTN_GROUP32:
					SetGroupSize(32);
					break;
			}
			return 0;
		case WM_SIZING: {
			/* keep the panel flush against the main window's right edge no
			   matter which border is dragged: only its width is free to change */
			RECT *pRect = (RECT *)lParam;
			HWND mainHWin = (pvar->cv != NULL) ? pvar->cv->HWin : NULL;
			if (mainHWin != NULL && IsWindow(mainHWin)) {
				RECT mr;
				int width = pRect->right - pRect->left;
				GetWindowRect(mainHWin, &mr);
				pRect->left = mr.right;
				pRect->right = pRect->left + width;
				pRect->top = mr.top;
				pRect->bottom = mr.bottom;
			}
			return TRUE;
		}
		case WM_MOVING: {
			/* dragging the panel by its title bar carries the main window
			   along by the same delta, so the two always stay aligned */
			RECT *pRect = (RECT *)lParam;
			HWND mainHWin = (pvar->cv != NULL) ? pvar->cv->HWin : NULL;
			int dx = pRect->left - pvar->lastLeft;
			int dy = pRect->top - pvar->lastTop;
			if (!pvar->syncing && (dx != 0 || dy != 0) &&
			    mainHWin != NULL && IsWindow(mainHWin)) {
				RECT mr;
				GetWindowRect(mainHWin, &mr);
				pvar->syncing = TRUE;
				SetWindowPos(mainHWin, NULL, mr.left + dx, mr.top + dy, 0, 0,
				             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
				pvar->syncing = FALSE;
			}
			pvar->lastLeft = pRect->left;
			pvar->lastTop = pRect->top;
			return TRUE;
		}
		case WM_EXITSIZEMOVE: {
			RECT r;
			GetWindowRect(hWnd, &r);
			pvar->panelWidth = r.right - r.left;
			pvar->lastLeft = r.left;
			pvar->lastTop = r.top;
			return 0;
		}
		case WM_TIMER:
			if (wParam == HEXVIEW_TIMER_SELECTION) {
				CheckSelectionHighlight();
				CheckScrollSync();
			}
			return 0;
		case WM_CLOSE:
			ShowWindow(hWnd, SW_HIDE);
			pvar->visible = FALSE;
			KillTimer(hWnd, HEXVIEW_TIMER_SELECTION);
			if (pvar->FileMenu != NULL) {
				CheckMenuItem(pvar->FileMenu, ID_MENU_HEXVIEW, MF_BYCOMMAND | MF_UNCHECKED);
			}
			return 0;
		case WM_DESTROY:
			KillTimer(hWnd, HEXVIEW_TIMER_SELECTION);
			if (pvar->hFont != NULL) {
				DeleteObject(pvar->hFont);
				pvar->hFont = NULL;
			}
			pvar->HexWin = NULL;
			pvar->EditCtrl = NULL;
			pvar->Btn4 = pvar->Btn8 = pvar->Btn16 = pvar->Btn32 = NULL;
			return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void RegisterHexViewClassOnce(void)
{
	static BOOL registered = FALSE;
	WNDCLASSEXW wc;

	if (registered) {
		return;
	}

	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = HexViewWndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = HEXVIEW_CLASS_NAME;
	RegisterClassExW(&wc);
	registered = TRUE;
}

static void PositionHexViewDefault(HWND mainHWin)
{
	RECT r;

	if (pvar->HexWin == NULL || !IsWindow(mainHWin)) {
		return;
	}
	GetWindowRect(mainHWin, &r);
	pvar->syncing = TRUE;
	SetWindowPos(pvar->HexWin, NULL, r.right, r.top,
	             pvar->panelWidth, r.bottom - r.top,
	             SWP_NOZORDER | SWP_NOACTIVATE);
	pvar->syncing = FALSE;
	pvar->lastLeft = r.right;
	pvar->lastTop = r.top;
}

static void CreateHexViewWindow(HWND mainHWin)
{
	RegisterHexViewClassOnce();
	pvar->HexWin = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_COMPOSITED,
		HEXVIEW_CLASS_NAME,
		L"Hex View",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, pvar->panelWidth, 400,
		mainHWin, NULL, hInst, NULL);
}

/*
 * ---- main window subclass, used to keep the panel aligned beside the
 * ---- terminal window while it is moved or resized ----
 */

static LRESULT CALLBACK HexViewMainSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_WINDOWPOSCHANGED && pvar->HexWin != NULL &&
	    IsWindowVisible(pvar->HexWin)) {
		PositionHexViewDefault(hWnd);
	}
	return CallWindowProcW(pvar->origMainWndProc, hWnd, msg, wParam, lParam);
}

static void EnsureSubclassed(HWND mainHWin)
{
	if (!pvar->subclassed && mainHWin != NULL && IsWindow(mainHWin)) {
		pvar->origMainWndProc = (WNDPROC)SetWindowLongPtrW(
			mainHWin, GWLP_WNDPROC, (LONG_PTR)HexViewMainSubclassProc);
		pvar->subclassed = TRUE;
	}
}

static void ToggleHexView(HWND mainHWin)
{
	EnsureSubclassed(mainHWin);

	if (pvar->HexWin == NULL) {
		CreateHexViewWindow(mainHWin);
	}

	if (pvar->visible) {
		ShowWindow(pvar->HexWin, SW_HIDE);
		pvar->visible = FALSE;
		KillTimer(pvar->HexWin, HEXVIEW_TIMER_SELECTION);
		if (pvar->highlightActive) {
			ClearHighlight();
			pvar->highlightActive = FALSE;
		}
		pvar->lastSelBuf[0] = L'\0';
	} else {
		PositionHexViewDefault(mainHWin);
		ShowWindow(pvar->HexWin, SW_SHOW);
		pvar->visible = TRUE;
		pvar->topLineKnown = FALSE; /* re-baseline scroll-sync from the current position */
		SetTimer(pvar->HexWin, HEXVIEW_TIMER_SELECTION, HEXVIEW_TIMER_INTERVAL_MS, NULL);
	}

	if (pvar->FileMenu != NULL) {
		CheckMenuItem(pvar->FileMenu, ID_MENU_HEXVIEW,
		              MF_BYCOMMAND | (pvar->visible ? MF_CHECKED : MF_UNCHECKED));
	}
}

/*
 * ---- TTX plugin entry points ----
 */

static void PASCAL TTXModifyMenu(HMENU menu)
{
	UINT flag = MF_BYCOMMAND | MF_STRING | MF_ENABLED;

	pvar->FileMenu = GetFileMenu(menu);
	if (pvar->FileMenu == NULL) {
		return;
	}
	if (pvar->visible) {
		flag |= MF_CHECKED;
	}

	InsertMenuW(pvar->FileMenu, ID_FILE_EXIT, MF_BYCOMMAND | MF_SEPARATOR, 0, NULL);
	InsertMenuW(pvar->FileMenu, ID_FILE_EXIT, flag, ID_MENU_HEXVIEW, L"Hex &View");
}

static void PASCAL TTXModifyPopupMenu(HMENU menu)
{
	if (pvar->FileMenu != NULL && menu == pvar->FileMenu) {
		CheckMenuItem(pvar->FileMenu, ID_MENU_HEXVIEW,
		              MF_BYCOMMAND | (pvar->visible ? MF_CHECKED : MF_UNCHECKED));
	}
}

static int PASCAL TTXProcessCommand(HWND hWin, WORD cmd)
{
	if (cmd == ID_MENU_HEXVIEW) {
		ToggleHexView(hWin);
		return 1;
	}
	return 0;
}

static void PASCAL TTXEnd(void)
{
	if (pvar->subclassed && pvar->cv != NULL && pvar->cv->HWin != NULL &&
	    IsWindow(pvar->cv->HWin)) {
		SetWindowLongPtrW(pvar->cv->HWin, GWLP_WNDPROC, (LONG_PTR)pvar->origMainWndProc);
		pvar->subclassed = FALSE;
	}
	if (pvar->HexWin != NULL && IsWindow(pvar->HexWin)) {
		DestroyWindow(pvar->HexWin);
		pvar->HexWin = NULL;
	}
	if (hRichEditDll != NULL) {
		FreeLibrary(hRichEditDll);
		hRichEditDll = NULL;
	}
}

static BOOL PASCAL TTXInit2(PTTSet ts, PComVar cv, const TTXImports *(*GetImports)(size_t size))
{
	pvar->ts = ts;
	pvar->cv = cv;
	pvar->imports = GetImports(sizeof(TTXImports));

	pvar->origPrecv = NULL;
	pvar->origPReadFile = NULL;
	pvar->FileMenu = NULL;
	pvar->HexWin = NULL;
	pvar->EditCtrl = NULL;
	pvar->Btn4 = pvar->Btn8 = pvar->Btn16 = pvar->Btn32 = NULL;
	pvar->hFont = NULL;
	pvar->visible = FALSE;
	pvar->groupSize = LoadGroupSize();
	pvar->darkMode = FALSE; /* re-detected by ApplyTheme() once the window exists */
	pvar->highlightActive = FALSE;
	pvar->lastSelBuf[0] = L'\0';
	pvar->panelWidth = ComputeRequiredPanelWidth(pvar->groupSize);
	pvar->subclassed = FALSE;
	pvar->origMainWndProc = NULL;
	pvar->syncing = FALSE;
	pvar->lastLeft = 0;
	pvar->lastTop = 0;
	pvar->byteCount = 0;
	pvar->localLine = 0;
	pvar->lineMarkCount = 0;
	pvar->lineOffset = 0;
	pvar->lineOffsetKnown = FALSE;
	pvar->topLineKnown = FALSE;
	pvar->lastTerminalTopLine = 0;
	pvar->hexViewTopLocalLine = 0;

	return TRUE;
}

static TTXExports Exports = {
	sizeof(TTXExports),
	ORDER,

	NULL, // TTXInit,
	NULL, // TTXGetUIHooks,
	NULL, // TTXGetSetupHooks,
	TTXOpenTCP,
	TTXCloseTCP,
	NULL, // TTXSetWinSize,
	TTXModifyMenu,
	TTXModifyPopupMenu,
	TTXProcessCommand,
	TTXEnd,
	NULL, // TTXSetCommandLine,
	TTXOpenFile,
	TTXCloseFile,

	TTXInit2,
};

BOOL __declspec(dllexport) PASCAL TTXBind(WORD Version, TTXExports *exports)
{
	int size = sizeof(Exports) - sizeof(exports->size);

	if (size > exports->size) {
		size = exports->size;
	}
	memcpy((char *)exports + sizeof(exports->size),
	       (char *)&Exports + sizeof(exports->size),
	       size);
	return TRUE;
}

BOOL WINAPI DllMain(HANDLE hInstance,
                    ULONG ul_reason_for_call,
                    LPVOID lpReserved)
{
	switch (ul_reason_for_call) {
		case DLL_THREAD_ATTACH:
			/* do thread initialization */
			break;
		case DLL_THREAD_DETACH:
			/* do thread cleanup */
			break;
		case DLL_PROCESS_ATTACH:
			/* do process initialization */
			hInst = hInstance;
			pvar = &InstVar;
			break;
		case DLL_PROCESS_DETACH:
			/* do process cleanup */
			break;
	}
	return TRUE;
}
