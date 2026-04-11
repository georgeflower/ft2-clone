// PSG instrument editor for ft2-clone Atari mode
// See ft2_psg_instr_ed.h for documentation.

#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ft2_psg_instr_ed.h"
#include "ft2_atari_mode.h"
#include "ft2_atari_replayer.h"
#include "ft2_structs.h"
#include "ft2_gui.h"
#include "ft2_palette.h"
#include "ft2_header.h"
#include "ft2_mouse.h"
#include "ft2_replayer.h"
#include "ft2_pattern_ed.h"
#include "ft2_unicode.h"

// Editor screen area (same region used by showInstEditor)
#define PSG_ED_X    0
#define PSG_ED_Y  173
#define PSG_ED_W  632
#define PSG_ED_H  224

// Row Y offsets (relative to PSG_ED_Y)
#define PSG_ROW0   4    // title bar
#define PSG_ROW1  18    // name
#define PSG_ROW2  34    // vol envelope
#define PSG_ROW3  50    // arp table
#define PSG_ROW4  66    // pitch envelope
#define PSG_ROW5  86    // mixer flags
#define PSG_ROW6 102    // hw envelope
#define PSG_ROW7 118    // noise period
#define PSG_ROW8 134    // loop points
#define PSG_ROW9 154    // action buttons

// Envelope/table cell layout
#define PSG_CELL_X_OFF  80  // x offset from PSG_ED_X for first cell
#define PSG_CELL_W      28  // cell width in pixels
#define PSG_NUM_CELLS   16  // number of cells shown per row

// Cursor: which field is selected (0=volEnv, 1=arp, 2=pitch, 3=flags, 4=hwenv, 5=noise, 6=name)
static int8_t  psgEdCursor = 0;
static int8_t  psgEdSubCursor = 0;

// -----------------------------------------------------------------------
// Serialization
// -----------------------------------------------------------------------

#define PSGI_MAGIC   "PSGI"
#define PSGI_VERSION 1

bool savePsgInstr(const char *path, const psgInstrument_t *ins)
{
	if (path == NULL || ins == NULL)
		return false;

	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return false;

	// Magic + version
	fwrite(PSGI_MAGIC, 1, 4, f);
	const uint8_t version = PSGI_VERSION;
	fwrite(&version, 1, 1, f);

	// Name: 22 bytes, null-padded
	char nameBuf[22];
	memset(nameBuf, 0, sizeof (nameBuf));
	strncpy(nameBuf, ins->name, 22);
	fwrite(nameBuf, 1, 22, f);

	// Volume envelope (64 bytes)
	fwrite(ins->volEnvelope, 1, MAX_PSG_ENVELOPE_LEN, f);
	fwrite(&ins->volEnvLength,    1, 1, f);
	fwrite(&ins->volEnvLoopStart, 1, 1, f);

	// Arpeggio table (64 bytes, signed)
	fwrite(ins->arpTable, 1, MAX_PSG_ARPEGGIO_LEN, f);
	fwrite(&ins->arpLength,    1, 1, f);
	fwrite(&ins->arpLoopStart, 1, 1, f);

	// Pitch envelope (64 × int16, LE = 128 bytes)
	for (int i = 0; i < MAX_PSG_PITCH_LEN; i++)
	{
		uint8_t lo = (uint8_t)(ins->pitchEnvelope[i] & 0xFF);
		uint8_t hi = (uint8_t)((ins->pitchEnvelope[i] >> 8) & 0xFF);
		fwrite(&lo, 1, 1, f);
		fwrite(&hi, 1, 1, f);
	}
	fwrite(&ins->pitchEnvLength,    1, 1, f);
	fwrite(&ins->pitchEnvLoopStart, 1, 1, f);

	// Flags bitfield
	uint8_t flags = 0;
	if (ins->toneEnabled)    flags |= 0x01;
	if (ins->noiseEnabled)   flags |= 0x02;
	if (ins->useHwEnvelope)  flags |= 0x04;
	fwrite(&flags, 1, 1, f);

	// Hardware envelope
	fwrite(&ins->hwEnvShape, 1, 1, f);
	uint8_t hwlo = (uint8_t)(ins->hwEnvPeriod & 0xFF);
	uint8_t hwhi = (uint8_t)((ins->hwEnvPeriod >> 8) & 0xFF);
	fwrite(&hwlo, 1, 1, f);
	fwrite(&hwhi, 1, 1, f);

	// Noise period
	fwrite(&ins->noisePeriod, 1, 1, f);

	fclose(f);
	return true;
}

bool loadPsgInstr(const char *path, psgInstrument_t *ins)
{
	if (path == NULL || ins == NULL)
		return false;

	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;

	// Magic
	char magic[4];
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, PSGI_MAGIC, 4) != 0)
	{
		fclose(f);
		return false;
	}

	// Version
	uint8_t version;
	if (fread(&version, 1, 1, f) != 1 || version != PSGI_VERSION)
	{
		fclose(f);
		return false;
	}

	memset(ins, 0, sizeof (*ins));

	// Name
	char nameBuf[22];
	if (fread(nameBuf, 1, 22, f) != 22)
	{
		fclose(f);
		return false;
	}
	memcpy(ins->name, nameBuf, 22);
	ins->name[22] = '\0';

	// Volume envelope
	if (fread(ins->volEnvelope,    1, MAX_PSG_ENVELOPE_LEN, f) != MAX_PSG_ENVELOPE_LEN) goto loadErr;
	if (fread(&ins->volEnvLength,    1, 1, f) != 1) goto loadErr;
	if (fread(&ins->volEnvLoopStart, 1, 1, f) != 1) goto loadErr;

	// Arpeggio table
	if (fread(ins->arpTable,    1, MAX_PSG_ARPEGGIO_LEN, f) != MAX_PSG_ARPEGGIO_LEN) goto loadErr;
	if (fread(&ins->arpLength,    1, 1, f) != 1) goto loadErr;
	if (fread(&ins->arpLoopStart, 1, 1, f) != 1) goto loadErr;

	// Pitch envelope
	for (int i = 0; i < MAX_PSG_PITCH_LEN; i++)
	{
		uint8_t lo, hi;
		if (fread(&lo, 1, 1, f) != 1 || fread(&hi, 1, 1, f) != 1) goto loadErr;
		ins->pitchEnvelope[i] = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
	}
	if (fread(&ins->pitchEnvLength,    1, 1, f) != 1) goto loadErr;
	if (fread(&ins->pitchEnvLoopStart, 1, 1, f) != 1) goto loadErr;

	// Flags
	uint8_t flags;
	if (fread(&flags, 1, 1, f) != 1) goto loadErr;
	ins->toneEnabled   = (flags & 0x01) != 0;
	ins->noiseEnabled  = (flags & 0x02) != 0;
	ins->useHwEnvelope = (flags & 0x04) != 0;

	// Hardware envelope
	if (fread(&ins->hwEnvShape, 1, 1, f) != 1) goto loadErr;
	{
		uint8_t lo, hi;
		if (fread(&lo, 1, 1, f) != 1 || fread(&hi, 1, 1, f) != 1) goto loadErr;
		ins->hwEnvPeriod = (uint16_t)lo | ((uint16_t)hi << 8);
	}

	// Noise period
	if (fread(&ins->noisePeriod, 1, 1, f) != 1) goto loadErr;

	fclose(f);
	return true;

loadErr:
	fclose(f);
	return false;
}

// -----------------------------------------------------------------------
// psgInstrDefault
// -----------------------------------------------------------------------

void psgInstrDefault(psgInstrument_t *ins)
{
	memset(ins, 0, sizeof (*ins));
	strncpy(ins->name, "New PSG Ins", 22);

	ins->volEnvelope[0]  = 15;
	ins->volEnvLength    = 1;
	ins->volEnvLoopStart = PSG_NO_LOOP;

	ins->arpLength    = 0;
	ins->arpLoopStart = PSG_NO_LOOP;

	ins->pitchEnvLength    = 0;
	ins->pitchEnvLoopStart = PSG_NO_LOOP;

	ins->toneEnabled   = true;
	ins->noiseEnabled  = false;
	ins->useHwEnvelope = false;
	ins->hwEnvShape    = 0;
	ins->hwEnvPeriod   = 0;
	ins->noisePeriod   = 0;
}

// -----------------------------------------------------------------------
// PSG bank sidecar save / load
// -----------------------------------------------------------------------

// Builds the sidecar path by replacing the extension of xmPathU with
// ".psgibank".  On Windows UNICHAR is wchar_t; elsewhere it is char.
// The returned buffer must be freed by the caller.
static UNICHAR *buildBankPath(const UNICHAR *xmPathU)
{
	if (xmPathU == NULL)
		return NULL;

#ifdef _WIN32
	// Find last dot on Windows (wchar_t)
	const wchar_t *dot = wcsrchr(xmPathU, L'.');
	size_t baseLen = (dot != NULL) ? (size_t)(dot - xmPathU) : wcslen(xmPathU);
	const wchar_t *ext = L".psgibank";
	size_t extLen = wcslen(ext);
	wchar_t *out = (wchar_t *)malloc((baseLen + extLen + 1) * sizeof(wchar_t));
	if (out == NULL)
		return NULL;
	wcsncpy(out, xmPathU, baseLen);
	wcscpy(out + baseLen, ext);
#else
	const char *dot = strrchr(xmPathU, '.');
	size_t baseLen = (dot != NULL) ? (size_t)(dot - xmPathU) : strlen(xmPathU);
	const char *ext = ".psgibank";
	size_t extLen = strlen(ext);
	char *out = (char *)malloc(baseLen + extLen + 1);
	if (out == NULL)
		return NULL;
	strncpy(out, xmPathU, baseLen);
	strcpy(out + baseLen, ext);
#endif

	return out;
}

// Instrument body size (without the per-file PSGI magic/version header)
// = name(22) + volEnv(64) + volLen(1) + volLoop(1)
//            + arpTable(64) + arpLen(1) + arpLoop(1)
//            + pitchEnv(128) + pitchLen(1) + pitchLoop(1)
//            + flags(1) + hwShape(1) + hwPeriod(2) + noisePeriod(1)
// = 289 bytes
#define PSGB_MAGIC   "PSGB"
#define PSGB_VERSION 1

bool savePsgBank(UNICHAR *xmPathU)
{
	atariReplayer_t *ar = atariMode_getReplayer();
	if (ar == NULL)
		return false;

	UNICHAR *bankPath = buildBankPath(xmPathU);
	if (bankPath == NULL)
		return false;

	FILE *f = UNICHAR_FOPEN(bankPath, "wb");
	free(bankPath);
	if (f == NULL)
		return false;

	// Header: magic, version, count
	fwrite(PSGB_MAGIC, 1, 4, f);
	const uint8_t version = PSGB_VERSION;
	const uint8_t count   = MAX_PSG_INSTRUMENTS;
	fwrite(&version, 1, 1, f);
	fwrite(&count,   1, 1, f);

	// Save each instrument body (no per-file PSGI header)
	for (int i = 0; i < MAX_PSG_INSTRUMENTS; i++)
	{
		const psgInstrument_t *ins = &ar->instruments[i];

		char nameBuf[22];
		memset(nameBuf, 0, sizeof (nameBuf));
		strncpy(nameBuf, ins->name, 22);
		fwrite(nameBuf, 1, 22, f);

		fwrite(ins->volEnvelope,      1, MAX_PSG_ENVELOPE_LEN,  f);
		fwrite(&ins->volEnvLength,    1, 1, f);
		fwrite(&ins->volEnvLoopStart, 1, 1, f);

		fwrite(ins->arpTable,         1, MAX_PSG_ARPEGGIO_LEN,  f);
		fwrite(&ins->arpLength,       1, 1, f);
		fwrite(&ins->arpLoopStart,    1, 1, f);

		for (int j = 0; j < MAX_PSG_PITCH_LEN; j++)
		{
			uint8_t lo = (uint8_t)(ins->pitchEnvelope[j] & 0xFF);
			uint8_t hi = (uint8_t)((ins->pitchEnvelope[j] >> 8) & 0xFF);
			fwrite(&lo, 1, 1, f);
			fwrite(&hi, 1, 1, f);
		}
		fwrite(&ins->pitchEnvLength,    1, 1, f);
		fwrite(&ins->pitchEnvLoopStart, 1, 1, f);

		uint8_t flags = 0;
		if (ins->toneEnabled)   flags |= 0x01;
		if (ins->noiseEnabled)  flags |= 0x02;
		if (ins->useHwEnvelope) flags |= 0x04;
		fwrite(&flags, 1, 1, f);

		fwrite(&ins->hwEnvShape, 1, 1, f);
		uint8_t hwlo = (uint8_t)(ins->hwEnvPeriod & 0xFF);
		uint8_t hwhi = (uint8_t)((ins->hwEnvPeriod >> 8) & 0xFF);
		fwrite(&hwlo, 1, 1, f);
		fwrite(&hwhi, 1, 1, f);

		fwrite(&ins->noisePeriod, 1, 1, f);
	}

	fclose(f);
	return true;
}

void loadPsgBankIfPresent(UNICHAR *xmPathU)
{
	atariReplayer_t *ar = atariMode_getReplayer();
	if (ar == NULL)
		return;

	UNICHAR *bankPath = buildBankPath(xmPathU);
	if (bankPath == NULL)
		return;

	FILE *f = UNICHAR_FOPEN(bankPath, "rb");
	free(bankPath);
	if (f == NULL)
		return; // sidecar doesn't exist, that's fine

	// Check magic
	char magic[4];
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, PSGB_MAGIC, 4) != 0)
	{
		fclose(f);
		return;
	}

	uint8_t version, count;
	if (fread(&version, 1, 1, f) != 1 || version != PSGB_VERSION)
	{
		fclose(f);
		return;
	}
	if (fread(&count, 1, 1, f) != 1)
	{
		fclose(f);
		return;
	}

	int toLoad = (count < MAX_PSG_INSTRUMENTS) ? count : MAX_PSG_INSTRUMENTS;

	for (int i = 0; i < toLoad; i++)
	{
		psgInstrument_t *ins = &ar->instruments[i];

		char nameBuf[22];
		if (fread(nameBuf, 1, 22, f) != 22) goto bankErr;
		memcpy(ins->name, nameBuf, 22);
		ins->name[22] = '\0';

		if (fread(ins->volEnvelope,      1, MAX_PSG_ENVELOPE_LEN, f) != MAX_PSG_ENVELOPE_LEN) goto bankErr;
		if (fread(&ins->volEnvLength,    1, 1, f) != 1) goto bankErr;
		if (fread(&ins->volEnvLoopStart, 1, 1, f) != 1) goto bankErr;

		if (fread(ins->arpTable,         1, MAX_PSG_ARPEGGIO_LEN, f) != MAX_PSG_ARPEGGIO_LEN) goto bankErr;
		if (fread(&ins->arpLength,       1, 1, f) != 1) goto bankErr;
		if (fread(&ins->arpLoopStart,    1, 1, f) != 1) goto bankErr;

		for (int j = 0; j < MAX_PSG_PITCH_LEN; j++)
		{
			uint8_t lo, hi;
			if (fread(&lo, 1, 1, f) != 1 || fread(&hi, 1, 1, f) != 1) goto bankErr;
			ins->pitchEnvelope[j] = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
		}
		if (fread(&ins->pitchEnvLength,    1, 1, f) != 1) goto bankErr;
		if (fread(&ins->pitchEnvLoopStart, 1, 1, f) != 1) goto bankErr;

		uint8_t flags;
		if (fread(&flags, 1, 1, f) != 1) goto bankErr;
		ins->toneEnabled   = (flags & 0x01) != 0;
		ins->noiseEnabled  = (flags & 0x02) != 0;
		ins->useHwEnvelope = (flags & 0x04) != 0;

		if (fread(&ins->hwEnvShape, 1, 1, f) != 1) goto bankErr;
		{
			uint8_t lo, hi;
			if (fread(&lo, 1, 1, f) != 1 || fread(&hi, 1, 1, f) != 1) goto bankErr;
			ins->hwEnvPeriod = (uint16_t)lo | ((uint16_t)hi << 8);
		}
		if (fread(&ins->noisePeriod, 1, 1, f) != 1) goto bankErr;
	}

bankErr:
	fclose(f);
}

// -----------------------------------------------------------------------
// UI drawing helpers
// -----------------------------------------------------------------------

static void drawPsgLabel(uint16_t x, uint16_t y, const char *text)
{
	textOutShadow(x, y, PAL_FORGRND, PAL_DSKTOP2, text);
}

// Draw a row of PSG_NUM_CELLS cells for a uint8_t array
static void drawU8Row(uint16_t x, uint16_t y, const uint8_t *data, uint8_t len,
                      int selectedIdx, bool isSigned)
{
	char buf[8];
	for (int i = 0; i < PSG_NUM_CELLS; i++)
	{
		if (i < (int)len)
		{
			if (isSigned)
				snprintf(buf, sizeof (buf), "%4d", (int)(int8_t)data[i]);
			else
				snprintf(buf, sizeof (buf), "%3u", (unsigned)data[i]);
		}
		else
		{
			snprintf(buf, sizeof (buf), " -- ");
		}

		if (i == selectedIdx)
			textOutShadow((uint16_t)(x + i * PSG_CELL_W), y, PAL_BLCKTXT, PAL_BLCKMRK, buf);
		else
			textOut((uint16_t)(x + i * PSG_CELL_W), y, PAL_FORGRND, buf);
	}
}

// Draw a row of PSG_NUM_CELLS cells for an int16_t array
static void drawI16Row(uint16_t x, uint16_t y, const int16_t *data, uint8_t len,
                       int selectedIdx)
{
	char buf[8];
	for (int i = 0; i < PSG_NUM_CELLS; i++)
	{
		if (i < (int)len)
			snprintf(buf, sizeof (buf), "%5d", (int)data[i]);
		else
			snprintf(buf, sizeof (buf), "  -- ");

		if (i == selectedIdx)
			textOutShadow((uint16_t)(x + i * PSG_CELL_W), y, PAL_BLCKTXT, PAL_BLCKMRK, buf);
		else
			textOut((uint16_t)(x + i * PSG_CELL_W), y, PAL_FORGRND, buf);
	}
}

// -----------------------------------------------------------------------
// Public UI API
// -----------------------------------------------------------------------

psgInstrument_t *getCurPsgInstr(void)
{
	atariReplayer_t *ar = atariMode_getReplayer();
	if (ar == NULL)
		return NULL;

	int idx = (int)editor.curInstr - 1; // editor.curInstr is 1-based
	if (idx < 0 || idx >= MAX_PSG_INSTRUMENTS)
		return NULL;

	return atariReplayer_getInstrument(ar, idx);
}

void showPsgInstrEditor(void)
{
	hidePatternEditor();
	ui.atariExportShown = true;
	drawPsgInstrEditor();
}

void hidePsgInstrEditor(void)
{
	ui.atariExportShown = false;
}

void drawPsgInstrEditor(void)
{
	if (!ui.atariExportShown)
		return;

	drawFramework(PSG_ED_X, PSG_ED_Y, PSG_ED_W, PSG_ED_H, FRAMEWORK_TYPE1);

	// Row 0: title
	drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW0, "PSG INSTRUMENT EDITOR");

	const psgInstrument_t *ins = getCurPsgInstr();
	if (ins == NULL)
	{
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW1, "(no instrument)");
		return;
	}

	// Row 1: name
	char nameLine[64];
	if (psgEdCursor == 6)
	{
		// Name-editing mode: show with blinking cursor indicator
		int nlen = (int)strlen(ins->name);
		snprintf(nameLine, sizeof (nameLine), "Name: %.22s_", ins->name);
		(void)nlen;
		textOutShadow(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW1, PAL_BLCKTXT, PAL_BLCKMRK, nameLine);
	}
	else
	{
		snprintf(nameLine, sizeof (nameLine), "Name: %.22s  [EDIT]", ins->name);
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW1, nameLine);
	}

	// Row 2: vol envelope
	{
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW2, "VolEnv:");
		int selCol = (psgEdCursor == 0) ? psgEdSubCursor : -1;
		drawU8Row(PSG_ED_X + PSG_CELL_X_OFF, PSG_ED_Y + PSG_ROW2,
		          ins->volEnvelope, ins->volEnvLength, selCol, false);

		char lenBuf[48];
		if (ins->volEnvLoopStart == PSG_NO_LOOP)
			snprintf(lenBuf, sizeof (lenBuf), " Len:%u Loop:off", (unsigned)ins->volEnvLength);
		else
			snprintf(lenBuf, sizeof (lenBuf), " Len:%u Loop:%u", (unsigned)ins->volEnvLength, (unsigned)ins->volEnvLoopStart);
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW2 + 9, lenBuf);
	}

	// Row 3: arp table
	{
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW3, "ArpTbl:");
		int selCol = (psgEdCursor == 1) ? psgEdSubCursor : -1;
		drawU8Row(PSG_ED_X + PSG_CELL_X_OFF, PSG_ED_Y + PSG_ROW3,
		          (const uint8_t *)ins->arpTable, ins->arpLength, selCol, true);

		char lenBuf[48];
		if (ins->arpLoopStart == PSG_NO_LOOP)
			snprintf(lenBuf, sizeof (lenBuf), " Len:%u Loop:off", (unsigned)ins->arpLength);
		else
			snprintf(lenBuf, sizeof (lenBuf), " Len:%u Loop:%u", (unsigned)ins->arpLength, (unsigned)ins->arpLoopStart);
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW3 + 9, lenBuf);
	}

	// Row 4: pitch envelope
	{
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW4, "Pitch :");
		int selCol = (psgEdCursor == 2) ? psgEdSubCursor : -1;
		drawI16Row(PSG_ED_X + PSG_CELL_X_OFF, PSG_ED_Y + PSG_ROW4,
		           ins->pitchEnvelope, ins->pitchEnvLength, selCol);

		char lenBuf[48];
		if (ins->pitchEnvLoopStart == PSG_NO_LOOP)
			snprintf(lenBuf, sizeof (lenBuf), " Len:%u Loop:off", (unsigned)ins->pitchEnvLength);
		else
			snprintf(lenBuf, sizeof (lenBuf), " Len:%u Loop:%u", (unsigned)ins->pitchEnvLength, (unsigned)ins->pitchEnvLoopStart);
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW4 + 9, lenBuf);
	}

	// Row 5: mixer flags
	{
		const char *toneTxt  = ins->toneEnabled  ? "[X] TONE"  : "[ ] TONE";
		const char *noiseTxt = ins->noiseEnabled ? "[X] NOISE" : "[ ] NOISE";
		if (ins->toneEnabled)
			textOutShadow(PSG_ED_X + 4,  PSG_ED_Y + PSG_ROW5, PAL_BLCKTXT, PAL_BLCKMRK, toneTxt);
		else
			drawPsgLabel(PSG_ED_X + 4,  PSG_ED_Y + PSG_ROW5, toneTxt);
		if (ins->noiseEnabled)
			textOutShadow(PSG_ED_X + 80, PSG_ED_Y + PSG_ROW5, PAL_BLCKTXT, PAL_BLCKMRK, noiseTxt);
		else
			drawPsgLabel(PSG_ED_X + 80, PSG_ED_Y + PSG_ROW5, noiseTxt);
	}

	// Row 6: HW envelope
	{
		char hwBuf[80];
		snprintf(hwBuf, sizeof (hwBuf),
		         "HwEnv:%s  Shape:%u  Period:%u",
		         ins->useHwEnvelope ? "ON " : "OFF",
		         (unsigned)ins->hwEnvShape,
		         (unsigned)ins->hwEnvPeriod);
		if (psgEdCursor == 4)
			textOutShadow(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW6, PAL_BLCKTXT, PAL_BLCKMRK, hwBuf);
		else
			drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW6, hwBuf);
	}

	// Row 7: noise period
	{
		char noiseBuf[32];
		snprintf(noiseBuf, sizeof (noiseBuf), "Noise period: %2u", (unsigned)ins->noisePeriod);
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW7, noiseBuf);
	}

	// Row 8: loop points
	{
		char loopBuf[80];
		snprintf(loopBuf, sizeof (loopBuf),
		         "VolLoop:%02X  ArpLoop:%02X  PitchLoop:%02X",
		         (unsigned)ins->volEnvLoopStart,
		         (unsigned)ins->arpLoopStart,
		         (unsigned)ins->pitchEnvLoopStart);
		drawPsgLabel(PSG_ED_X + 4, PSG_ED_Y + PSG_ROW8, loopBuf);
	}

	// Row 9: action buttons
	drawFramework(PSG_ED_X + 4,   PSG_ED_Y + PSG_ROW9, 36, 13, FRAMEWORK_TYPE2);
	drawFramework(PSG_ED_X + 44,  PSG_ED_Y + PSG_ROW9, 44, 13, FRAMEWORK_TYPE2);
	drawFramework(PSG_ED_X + 92,  PSG_ED_Y + PSG_ROW9, 44, 13, FRAMEWORK_TYPE2);
	drawFramework(PSG_ED_X + 140, PSG_ED_Y + PSG_ROW9, 44, 13, FRAMEWORK_TYPE2);

	textOutShadow(PSG_ED_X + 8,   PSG_ED_Y + PSG_ROW9 + 3, PAL_FORGRND, PAL_DSKTOP2, "NEW");
	textOutShadow(PSG_ED_X + 48,  PSG_ED_Y + PSG_ROW9 + 3, PAL_FORGRND, PAL_DSKTOP2, "COPY");
	textOutShadow(PSG_ED_X + 96,  PSG_ED_Y + PSG_ROW9 + 3, PAL_FORGRND, PAL_DSKTOP2, "XCHG");
	textOutShadow(PSG_ED_X + 144, PSG_ED_Y + PSG_ROW9 + 3, PAL_FORGRND, PAL_DSKTOP2, "CLEAR");
}

void updatePsgInstrEditor(void)
{
	if (ui.atariExportShown)
		drawPsgInstrEditor();
}

// -----------------------------------------------------------------------
// Mouse handling
// -----------------------------------------------------------------------

bool testPsgInstrEditorMouseDown(void)
{
	if (!ui.atariExportShown)
		return false;

	// Bounds check
	if (mouse.x < PSG_ED_X || mouse.x >= PSG_ED_X + PSG_ED_W)
		return false;
	if (mouse.y < PSG_ED_Y || mouse.y >= PSG_ED_Y + PSG_ED_H)
		return false;

	psgInstrument_t *ins = getCurPsgInstr();
	if (ins == NULL)
		return true;

	const bool leftBtn  = mouse.leftButtonPressed;
	const bool rightBtn = mouse.rightButtonPressed;
	if (!leftBtn && !rightBtn)
		return true;

	int mx = mouse.x - PSG_ED_X;
	int my = mouse.y - PSG_ED_Y;

	// Helper: test a cell row
	// Returns 0-based cell index if hit, or -1
#define CELL_HIT(rowY) \
	((my >= (rowY) && my < (rowY) + 8 && mx >= PSG_CELL_X_OFF && mx < PSG_CELL_X_OFF + PSG_NUM_CELLS * PSG_CELL_W) \
	 ? (mx - PSG_CELL_X_OFF) / PSG_CELL_W : -1)

	// Row 2: vol envelope cells
	{
		int cell = CELL_HIT(PSG_ROW2);
		if (cell >= 0)
		{
			psgEdCursor = 0;
			psgEdSubCursor = (int8_t)cell;
			if (cell == ins->volEnvLength && ins->volEnvLength < MAX_PSG_ENVELOPE_LEN)
				ins->volEnvLength++;
			if (cell < ins->volEnvLength)
			{
				if (leftBtn && ins->volEnvelope[cell] < 15)
					ins->volEnvelope[cell]++;
				else if (rightBtn && ins->volEnvelope[cell] > 0)
					ins->volEnvelope[cell]--;
			}
			goto changed;
		}
	}

	// Row 3: arp table cells
	{
		int cell = CELL_HIT(PSG_ROW3);
		if (cell >= 0)
		{
			psgEdCursor = 1;
			psgEdSubCursor = (int8_t)cell;
			if (cell == ins->arpLength && ins->arpLength < MAX_PSG_ARPEGGIO_LEN)
				ins->arpLength++;
			if (cell < ins->arpLength)
			{
				if (leftBtn && ins->arpTable[cell] < 63)
					ins->arpTable[cell]++;
				else if (rightBtn && ins->arpTable[cell] > -64)
					ins->arpTable[cell]--;
			}
			goto changed;
		}
	}

	// Row 4: pitch envelope cells
	{
		int cell = CELL_HIT(PSG_ROW4);
		if (cell >= 0)
		{
			psgEdCursor = 2;
			psgEdSubCursor = (int8_t)cell;
			if (cell == ins->pitchEnvLength && ins->pitchEnvLength < MAX_PSG_PITCH_LEN)
				ins->pitchEnvLength++;
			if (cell < ins->pitchEnvLength)
			{
				if (leftBtn)
					ins->pitchEnvelope[cell] = (int16_t)(ins->pitchEnvelope[cell] + 10);
				else if (rightBtn)
					ins->pitchEnvelope[cell] = (int16_t)(ins->pitchEnvelope[cell] - 10);
			}
			goto changed;
		}
	}

	// Row 5: mixer flags — [X] TONE (x=4..68), [X] NOISE (x=80..152)
	if (my >= PSG_ROW5 && my < PSG_ROW5 + 8)
	{
		if (mx >= 4 && mx < 76)
		{
			ins->toneEnabled = !ins->toneEnabled;
			goto changed;
		}
		if (mx >= 80 && mx < 156)
		{
			ins->noiseEnabled = !ins->noiseEnabled;
			goto changed;
		}
	}

	// Row 6: HW envelope — ON/OFF (x=4..80), Shape value (+60), Period value (+100)
	if (my >= PSG_ROW6 && my < PSG_ROW6 + 8)
	{
		if (mx >= 4 && mx < 80)
		{
			// Toggle useHwEnvelope
			ins->useHwEnvelope = !ins->useHwEnvelope;
			psgEdCursor = 4;
			goto changed;
		}
		if (mx >= 80 && mx < 130)
		{
			// Shape
			psgEdCursor = 4; psgEdSubCursor = 0;
			if (leftBtn && ins->hwEnvShape < 15)
				ins->hwEnvShape++;
			else if (rightBtn && ins->hwEnvShape > 0)
				ins->hwEnvShape--;
			goto changed;
		}
		if (mx >= 130 && mx < 250)
		{
			// Period
			psgEdCursor = 4; psgEdSubCursor = 1;
			if (leftBtn && ins->hwEnvPeriod < 65535)
				ins->hwEnvPeriod++;
			else if (rightBtn && ins->hwEnvPeriod > 0)
				ins->hwEnvPeriod--;
			goto changed;
		}
	}

	// Row 7: noise period
	if (my >= PSG_ROW7 && my < PSG_ROW7 + 8)
	{
		if (leftBtn && ins->noisePeriod < 31)
			ins->noisePeriod++;
		else if (rightBtn && ins->noisePeriod > 0)
			ins->noisePeriod--;
		psgEdCursor = 5;
		goto changed;
	}

	// Row 9: action buttons
	if (my >= PSG_ROW9 && my < PSG_ROW9 + 13)
	{
		if (leftBtn)
		{
			if (mx >= 4 && mx < 40)
			{
				// [NEW]
				psgInstrDefault(ins);
				goto changed;
			}
			if (mx >= 44 && mx < 88)
			{
				// [COPY]
				atariReplayer_t *ar = atariMode_getReplayer();
				if (ar != NULL)
				{
					int src = (int)editor.srcInstr - 1;
					int dst = (int)editor.curInstr - 1;
					if (src >= 0 && src < MAX_PSG_INSTRUMENTS &&
					    dst >= 0 && dst < MAX_PSG_INSTRUMENTS && src != dst)
					{
						ar->instruments[dst] = ar->instruments[src];
					}
				}
				goto changed;
			}
			if (mx >= 92 && mx < 136)
			{
				// [XCHG]
				atariReplayer_t *ar = atariMode_getReplayer();
				if (ar != NULL)
				{
					int a = (int)editor.srcInstr - 1;
					int b = (int)editor.curInstr - 1;
					if (a >= 0 && a < MAX_PSG_INSTRUMENTS &&
					    b >= 0 && b < MAX_PSG_INSTRUMENTS && a != b)
					{
						psgInstrument_t tmp = ar->instruments[a];
						ar->instruments[a]  = ar->instruments[b];
						ar->instruments[b]  = tmp;
					}
				}
				goto changed;
			}
			if (mx >= 140 && mx < 184)
			{
				// [CLEAR]
				psgInstrDefault(ins);
				goto changed;
			}
		}
	}

	// Click on name row — enter name editing
	if (my >= PSG_ROW1 && my < PSG_ROW1 + 8)
	{
		psgEdCursor = 6;
		updatePsgInstrEditor();
		return true;
	}

	return true;

#undef CELL_HIT

changed:
	updatePsgInstrEditor();
	setSongModifiedFlag();
	return true;
}

// -----------------------------------------------------------------------
// Keyboard handling
// -----------------------------------------------------------------------

void psgInstrEditorKeyDown(SDL_Keycode key)
{
	psgInstrument_t *ins = getCurPsgInstr();
	if (ins == NULL)
		return;

	bool changed = false;

	switch (psgEdCursor)
	{
		// ----- cursor 0: volume envelope -----
		case 0:
		{
			int sc = psgEdSubCursor;
			switch (key)
			{
				case SDLK_UP:
					if (sc < ins->volEnvLength && ins->volEnvelope[sc] < 15)
					{ ins->volEnvelope[sc]++; changed = true; }
					break;
				case SDLK_DOWN:
					if (sc < ins->volEnvLength && ins->volEnvelope[sc] > 0)
					{ ins->volEnvelope[sc]--; changed = true; }
					break;
				case SDLK_LEFT:
					if (psgEdSubCursor > 0)
					{ psgEdSubCursor--; changed = true; }
					break;
				case SDLK_RIGHT:
					if (psgEdSubCursor < ins->volEnvLength &&
					    psgEdSubCursor < MAX_PSG_ENVELOPE_LEN - 1)
					{ psgEdSubCursor++; changed = true; }
					break;
				case SDLK_INSERT:
					if (ins->volEnvLength < MAX_PSG_ENVELOPE_LEN)
					{
						int pos = sc;
						memmove(&ins->volEnvelope[pos + 1], &ins->volEnvelope[pos],
						        ins->volEnvLength - pos);
						ins->volEnvelope[pos] = 0;
						ins->volEnvLength++;
						changed = true;
					}
					break;
				case SDLK_DELETE:
					if (ins->volEnvLength > 0 && sc < ins->volEnvLength)
					{
						memmove(&ins->volEnvelope[sc], &ins->volEnvelope[sc + 1],
						        ins->volEnvLength - sc - 1);
						ins->volEnvLength--;
						if (psgEdSubCursor >= ins->volEnvLength && psgEdSubCursor > 0)
							psgEdSubCursor--;
						changed = true;
					}
					break;
				default: break;
			}
			break;
		}

		// ----- cursor 1: arp table -----
		case 1:
		{
			int sc = psgEdSubCursor;
			switch (key)
			{
				case SDLK_UP:
					if (sc < ins->arpLength && ins->arpTable[sc] < 63)
					{ ins->arpTable[sc]++; changed = true; }
					break;
				case SDLK_DOWN:
					if (sc < ins->arpLength && ins->arpTable[sc] > -64)
					{ ins->arpTable[sc]--; changed = true; }
					break;
				case SDLK_LEFT:
					if (psgEdSubCursor > 0)
					{ psgEdSubCursor--; changed = true; }
					break;
				case SDLK_RIGHT:
					if (psgEdSubCursor < ins->arpLength &&
					    psgEdSubCursor < MAX_PSG_ARPEGGIO_LEN - 1)
					{ psgEdSubCursor++; changed = true; }
					break;
				case SDLK_INSERT:
					if (ins->arpLength < MAX_PSG_ARPEGGIO_LEN)
					{
						int pos = sc;
						memmove(&ins->arpTable[pos + 1], &ins->arpTable[pos],
						        ins->arpLength - pos);
						ins->arpTable[pos] = 0;
						ins->arpLength++;
						changed = true;
					}
					break;
				case SDLK_DELETE:
					if (ins->arpLength > 0 && sc < ins->arpLength)
					{
						memmove(&ins->arpTable[sc], &ins->arpTable[sc + 1],
						        ins->arpLength - sc - 1);
						ins->arpLength--;
						if (psgEdSubCursor >= ins->arpLength && psgEdSubCursor > 0)
							psgEdSubCursor--;
						changed = true;
					}
					break;
				default: break;
			}
			break;
		}

		// ----- cursor 2: pitch envelope -----
		case 2:
		{
			int sc = psgEdSubCursor;
			switch (key)
			{
				case SDLK_UP:
					if (sc < ins->pitchEnvLength)
					{ ins->pitchEnvelope[sc] = (int16_t)(ins->pitchEnvelope[sc] + 10); changed = true; }
					break;
				case SDLK_DOWN:
					if (sc < ins->pitchEnvLength)
					{ ins->pitchEnvelope[sc] = (int16_t)(ins->pitchEnvelope[sc] - 10); changed = true; }
					break;
				case SDLK_LEFT:
					if (psgEdSubCursor > 0)
					{ psgEdSubCursor--; changed = true; }
					break;
				case SDLK_RIGHT:
					if (psgEdSubCursor < ins->pitchEnvLength &&
					    psgEdSubCursor < MAX_PSG_PITCH_LEN - 1)
					{ psgEdSubCursor++; changed = true; }
					break;
				case SDLK_INSERT:
					if (ins->pitchEnvLength < MAX_PSG_PITCH_LEN)
					{
						int pos = sc;
						memmove(&ins->pitchEnvelope[pos + 1], &ins->pitchEnvelope[pos],
						        (ins->pitchEnvLength - pos) * sizeof(int16_t));
						ins->pitchEnvelope[pos] = 0;
						ins->pitchEnvLength++;
						changed = true;
					}
					break;
				case SDLK_DELETE:
					if (ins->pitchEnvLength > 0 && sc < ins->pitchEnvLength)
					{
						memmove(&ins->pitchEnvelope[sc], &ins->pitchEnvelope[sc + 1],
						        (ins->pitchEnvLength - sc - 1) * sizeof(int16_t));
						ins->pitchEnvLength--;
						if (psgEdSubCursor >= ins->pitchEnvLength && psgEdSubCursor > 0)
							psgEdSubCursor--;
						changed = true;
					}
					break;
				default: break;
			}
			break;
		}

		// ----- cursor 3: mixer flags -----
		case 3:
		{
			switch (key)
			{
				case SDLK_t:
					ins->toneEnabled = !ins->toneEnabled;
					changed = true;
					break;
				case SDLK_n:
					ins->noiseEnabled = !ins->noiseEnabled;
					changed = true;
					break;
				default: break;
			}
			break;
		}

		// ----- cursor 4: hw envelope -----
		case 4:
		{
			switch (key)
			{
				case SDLK_UP:
					if (psgEdSubCursor == 0)
					{
						if (ins->hwEnvShape < 15) { ins->hwEnvShape++; changed = true; }
					}
					else
					{
						if (ins->hwEnvPeriod < 65535) { ins->hwEnvPeriod++; changed = true; }
					}
					break;
				case SDLK_DOWN:
					if (psgEdSubCursor == 0)
					{
						if (ins->hwEnvShape > 0) { ins->hwEnvShape--; changed = true; }
					}
					else
					{
						if (ins->hwEnvPeriod > 0) { ins->hwEnvPeriod--; changed = true; }
					}
					break;
				case SDLK_LEFT:
					if (psgEdSubCursor > 0) { psgEdSubCursor--; changed = true; }
					break;
				case SDLK_RIGHT:
					if (psgEdSubCursor < 1) { psgEdSubCursor++; changed = true; }
					break;
				case SDLK_h:
					ins->useHwEnvelope = !ins->useHwEnvelope;
					changed = true;
					break;
				default: break;
			}
			break;
		}

		// ----- cursor 5: noise period -----
		case 5:
		{
			switch (key)
			{
				case SDLK_UP:
					if (ins->noisePeriod < 31) { ins->noisePeriod++; changed = true; }
					break;
				case SDLK_DOWN:
					if (ins->noisePeriod > 0) { ins->noisePeriod--; changed = true; }
					break;
				default: break;
			}
			break;
		}

		// ----- cursor 6: name editing -----
		case 6:
		{
			if (key == SDLK_ESCAPE || key == SDLK_RETURN)
			{
				psgEdCursor = 0;
				changed = true;
			}
			else if (key == SDLK_BACKSPACE)
			{
				int nlen = (int)strlen(ins->name);
				if (nlen > 0)
				{
					ins->name[nlen - 1] = '\0';
					changed = true;
				}
			}
			// Printable ASCII is handled by psgInstrEditorKeyChar
			break;
		}

		default: break;
	}

	// Global navigation: TAB / PGUP / PGDOWN cycle cursor 0..6
	if (key == SDLK_TAB || key == SDLK_PAGEDOWN)
	{
		psgEdCursor = (int8_t)((psgEdCursor + 1) % 7);
		psgEdSubCursor = 0;
		changed = true;
	}
	else if (key == SDLK_PAGEUP)
	{
		psgEdCursor = (int8_t)((psgEdCursor + 6) % 7);
		psgEdSubCursor = 0;
		changed = true;
	}

	if (changed)
	{
		updatePsgInstrEditor();
		setSongModifiedFlag();
	}
}
