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
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define ORDER 8500

/* File menu command id (must be unique across all loaded TTX plugins) */
#define ID_MENU_HEXVIEW  55950

/* child control ids, local to the hex view window only */
#define ID_BTN_GROUP4    61101
#define ID_BTN_GROUP8    61102
#define ID_BTN_GROUP32   61103
#define ID_EDIT_HEXVIEW  61110

#define HEXVIEW_CLASS_NAME L"TTXHexViewWindow"

/* the companion window's default/minimum width, in pixels */
#define HEXVIEW_DEFAULT_WIDTH  260
#define HEXVIEW_MIN_WIDTH      140

/* how many raw received bytes are retained for redisplay (the oldest
   half is evicted and the view fully rebuilt once this is exceeded) */
#define HEXVIEW_MAX_BYTES 65536

/* selection-highlight polling */
#define HEXVIEW_TIMER_SELECTION 1
#define HEXVIEW_TIMER_INTERVAL_MS 300
#define HEXVIEW_SELBUF_LEN 4096
#define HEXVIEW_HIGHLIGHT_COLOR RGB(255, 255, 153) /* light yellow */

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
	HWND Btn32;
	HFONT hFont;
	BOOL visible;
	int groupSize;   /* 4, 8 or 32 bytes per line */
	int panelWidth;  /* current window width, in pixels */

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
   size: "%08X  " + perLine*"XX " + " " + perLine ascii chars + "\r\n" */
static int RowCharWidth(int perLine)
{
	return 10 + perLine * 3 + 1 + perLine + 2;
}

/* character offset, within its row, of byte `idx`'s hex token - a pure
   formula, since only the last row can ever be incomplete and every row
   before it is always fully-filled */
static int HexCharOffset(int idx, int perLine)
{
	int row = idx / perLine;
	int col = idx % perLine;

	return row * RowCharWidth(perLine) + 10 + col * 3;
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

	if (pvar->EditCtrl == NULL) {
		return;
	}

	pvar->highlightActive = FALSE;
	pvar->lastSelBuf[0] = L'\0';

	text = FormatRows(0, pvar->byteCount, pvar->groupSize);
	SetWindowTextW(pvar->EditCtrl, (text != NULL) ? text : L"");
	free(text);

	SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
	SendMessageW(pvar->EditCtrl, EM_SCROLLCARET, 0, 0);
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
			/* evict the oldest half to make room for new bytes */
			int keep = HEXVIEW_MAX_BYTES / 2;
			int drop = pvar->byteCount - keep;

			memmove(pvar->byteBuf, pvar->byteBuf + drop, (size_t)keep);
			pvar->byteCount = keep;
		}
		pvar->byteBuf[pvar->byteCount++] = b;
		added = TRUE;
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

static void UpdateGroupButtons(void)
{
	if (pvar->Btn4 == NULL) {
		return;
	}
	EnableWindow(pvar->Btn4, pvar->groupSize != 4);
	EnableWindow(pvar->Btn8, pvar->groupSize != 8);
	EnableWindow(pvar->Btn32, pvar->groupSize != 32);
}

static void SetGroupSize(int newSize)
{
	if (pvar->groupSize == newSize) {
		return;
	}
	pvar->groupSize = newSize;
	UpdateGroupButtons();
	RenderAll();
}

/*
 * ---- terminal-selection -> hex byte highlighting ----
 */

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

static void ApplyHighlight(int startChar, int endChar)
{
	CHARFORMAT2W cf;

	if (pvar->EditCtrl == NULL) {
		return;
	}
	SendMessageW(pvar->EditCtrl, EM_SETSEL, (WPARAM)startChar, (LPARAM)endChar);
	ZeroMemory(&cf, sizeof(cf));
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_BACKCOLOR;
	cf.crBackColor = HEXVIEW_HIGHLIGHT_COLOR;
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
 */
static void CheckSelectionHighlight(void)
{
	wchar_t selBuf[HEXVIEW_SELBUF_LEN];
	char utf8[HEXVIEW_SELBUF_LEN];
	int selLen, utf8Len, j;
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

	for (j = 0; j + utf8Len <= pvar->byteCount && !found; j++) {
		if (memcmp(pvar->byteBuf + j, utf8, (size_t)utf8Len) == 0) {
			int lastByte = j + utf8Len - 1;
			int startChar = HexCharOffset(j, pvar->groupSize);
			int endChar = HexCharOffset(lastByte, pvar->groupSize) + 2; /* the last byte's 2 hex digits */

			ClearHighlight();
			ApplyHighlight(startChar, endChar);
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
			cf.yHeight = 180; /* 9pt, in twips */
			wcscpy_s(cf.szFaceName, _countof(cf.szFaceName), L"Consolas");
			SendMessageW(pvar->EditCtrl, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);

			SendMessageW(pvar->Btn4, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);
			SendMessageW(pvar->Btn8, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);
			SendMessageW(pvar->Btn32, WM_SETFONT, (WPARAM)pvar->hFont, TRUE);

			UpdateGroupButtons();
			RenderAll();
			return 0;
		}
		case WM_SIZE: {
			int cw = LOWORD(lParam);
			int ch = HIWORD(lParam);
			int bw = 72, bh = 24, pad = 4;
			HDWP dwp = BeginDeferWindowPos(4); /* batch the moves into one repaint pass */

			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn4, NULL, pad, pad, bw, bh, SWP_NOZORDER);
			}
			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn8, NULL, pad * 2 + bw, pad, bw, bh, SWP_NOZORDER);
			}
			if (dwp != NULL) {
				dwp = DeferWindowPos(dwp, pvar->Btn32, NULL, pad * 3 + bw * 2, pad, bw, bh, SWP_NOZORDER);
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
			mmi->ptMinTrackSize.x = HEXVIEW_MIN_WIDTH;
			return 0;
		}
		case WM_COMMAND:
			switch (LOWORD(wParam)) {
				case ID_BTN_GROUP4:
					SetGroupSize(4);
					break;
				case ID_BTN_GROUP8:
					SetGroupSize(8);
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
			pvar->Btn4 = pvar->Btn8 = pvar->Btn32 = NULL;
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
	pvar->Btn4 = pvar->Btn8 = pvar->Btn32 = NULL;
	pvar->hFont = NULL;
	pvar->visible = FALSE;
	pvar->groupSize = 8;
	pvar->highlightActive = FALSE;
	pvar->lastSelBuf[0] = L'\0';
	pvar->panelWidth = HEXVIEW_DEFAULT_WIDTH;
	pvar->subclassed = FALSE;
	pvar->origMainWndProc = NULL;
	pvar->syncing = FALSE;
	pvar->lastLeft = 0;
	pvar->lastTop = 0;
	pvar->byteCount = 0;

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
