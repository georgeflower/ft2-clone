/* SNDH (Atari ST YM2149 chip music) loader
**
** SNDH files contain 68000 machine code music routines and cannot be
** converted into editable FT2 tracker patterns.  This loader parses
** the SNDH header metadata, populates the song name, and shows an
** informational message so the user knows what happened.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../ft2_header.h"
#include "../ft2_module_loader.h"
#include "../ft2_sysreqs.h"
#include "../ft2_sndh.h"

bool loadSNDH(FILE *f, uint32_t filesize)
{
	if (filesize == 0)
	{
		loaderMsgBox("Error loading SNDH: File is empty.");
		return false;
	}

	uint8_t *data = (uint8_t *)malloc(filesize);
	if (data == NULL)
	{
		loaderMsgBox("Error loading SNDH: Not enough memory!");
		return false;
	}

	rewind(f);
	if (fread(data, 1, filesize, f) != filesize)
	{
		free(data);
		loaderMsgBox("Error loading SNDH: I/O error while reading file.");
		return false;
	}

	sndh_info_t info;
	if (!sndh_parseHeader(data, filesize, &info))
	{
		free(data);
		loaderMsgBox("Error loading SNDH: File is missing the SNDH header marker.");
		return false;
	}

	free(data);

	// Populate song name from the SNDH title tag (song_t.name is 20 chars + NUL)
	memset(songTmp.name, 0, sizeof(songTmp.name));
	strncpy(songTmp.name, info.title, sizeof(songTmp.name) - 1);

	// Set minimal safe song fields so the song struct is valid
	songTmp.songLength  = 1;
	songTmp.numChannels = 2;
	songTmp.BPM         = 125;
	songTmp.speed       = 6;
	songTmp.orders[0]   = 0;

	loaderMsgBox(
		"SNDH file loaded (metadata only).\n\n"
		"Title:     %s\n"
		"Author:    %s\n"
		"Year:      %s\n"
		"Sub-tunes: %d  Timer: %.1f Hz\n\n"
		"Note: SNDH files contain Atari ST 68000 machine code and cannot be\n"
		"edited as tracker patterns. Use the SNDH export function to re-export.",
		info.title,
		info.author,
		info.year,
		info.numSubTunes,
		info.timerFreqHz
	);

	return true;
}
