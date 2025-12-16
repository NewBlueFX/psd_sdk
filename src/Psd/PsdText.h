// Contributed by Adam @ NewBlue Inc.
// Licensed under the 2-clause BSD License.

#pragma once

#include "PsdFixedSizeString.h"
#include "PsdTypes.h"


PSD_NAMESPACE_BEGIN

/// \ingroup Types
/// \class LayerText
/// \brief Minimal metadata extracted from Photoshop text layers.
struct LayerText
{
	util::FixedSizeString text;					///< UTF16 text converted to UTF8/ASCII where possible.
	util::FixedSizeString fontName;				///< User-facing font name.
	util::FixedSizeString fontPostScriptName;	///< PostScript font name if available.
	bool fauxBold;								///< True if faux bold is requested in EngineData.
	bool fauxItalic;							///< True if faux italic is requested in EngineData.
	int32_t paragraphJustification;				///< -1 if unknown, otherwise Photoshop justification enum (0=left,1=right,2=center,...).

	// Bounds reported by the Type Tool info block (pixels in layer space).
	int32_t boxTop;
	int32_t boxLeft;
	int32_t boxBottom;
	int32_t boxRight;

	// Optional advanced style runs from EngineData.
	struct StyleRun
	{
		uint32_t start;							///< Start offset into the text string.
		uint32_t length;						///< Length of the run.
		util::FixedSizeString fontName;			///< User-facing font name.
		util::FixedSizeString fontPostScriptName;///< PostScript font name.
		float32_t fontSize;						///< Requested font size.
		bool fauxBold;							///< Faux bold for this run.
		bool fauxItalic;						///< Faux italic for this run.
	};

	StyleRun* styleRuns;						///< Array of style runs, if available.
	uint32_t styleRunCount;						///< Number of entries in styleRuns.
};

PSD_NAMESPACE_END
