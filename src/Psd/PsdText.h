// Contributed by Adam @ NewBlue Inc.
// Licensed under the 2-clause BSD License.

#pragma once

#include "PsdFixedSizeString.h"
#include "PsdTypes.h"


PSD_NAMESPACE_BEGIN

/// \ingroup Types
/// \class LayerText
/// \brief Minimal metadata extracted from Photoshop text layers.
/// \details Fields map directly to Photoshop's Type Tool (TySh) additional layer info. Text is best-effort ASCII from UTF16,
/// fonts come from EngineData and FontSet, and transforms are TySh 3x2 matrices (see Adobe PSD spec). Coordinates are
/// in PSD pixel space, origin at top-left. paragraphJustification uses Photoshop enums (0=left,1=right,2=center,3=justify).
struct LayerText
{
	util::FixedSizeString text;					///< UTF16 text converted to UTF8/ASCII where possible.
	util::FixedSizeString fontName;				///< User-facing font name.
	util::FixedSizeString fontPostScriptName;	///< PostScript font name if available.
	bool fauxBold;								///< True if faux bold is requested in EngineData.
	bool fauxItalic;							///< True if faux italic is requested in EngineData.
	int32_t paragraphJustification;				///< -1 if unknown, otherwise Photoshop justification enum (0=left,1=right,2=center,...).
	float64_t transform[6];					///< TypeTool transform matrix (xx, xy, yx, yy, tx, ty) to place text in document space.
	int32_t colorSpace;						///< PSD color space enum value (see colorMode::Enum); -1 if unknown (EngineData may be device space).
	float32_t color[4];						///< Components in the above color space; normalized if device RGB, otherwise raw; -1 if unknown.
	float32_t fillColorR;						///< Fill color red component [0.0, 1.0], -1 if unknown.
	float32_t fillColorG;						///< Fill color green component [0.0, 1.0], -1 if unknown.
	float32_t fillColorB;						///< Fill color blue component [0.0, 1.0], -1 if unknown.
	float32_t fillColorA;						///< Fill color alpha component [0.0, 1.0], -1 if unknown.

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
		float32_t fillColorR;					///< Fill color red [0.0, 1.0], -1 if unknown.
		float32_t fillColorG;					///< Fill color green [0.0, 1.0], -1 if unknown.
		float32_t fillColorB;					///< Fill color blue [0.0, 1.0], -1 if unknown.
		float32_t fillColorA;					///< Fill color alpha [0.0, 1.0], -1 if unknown.
	};

	StyleRun* styleRuns;						///< Array of style runs, if available.
	uint32_t styleRunCount;						///< Number of entries in styleRuns.
};

PSD_NAMESPACE_END
