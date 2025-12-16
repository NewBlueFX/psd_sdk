// Contributed by Adam @ NewBlue Inc.
// Licensed under the 2-clause BSD License.

#pragma once

#include "PsdFixedSizeString.h"
#include "PsdTypes.h"

PSD_NAMESPACE_BEGIN

/// \ingroup Types
/// \class PlacedLayer
/// \brief Minimal container for placed (smart object) data such as embedded EPS/PDF/etc.
struct PlacedLayer
{
	uint32_t type;						///< Placed layer type enum from Photoshop (see spec). 0 if unknown.
	util::FixedSizeString uid;			///< Unique identifier string if present.
	uint8_t* data;						///< Raw placed data payload (embedded asset bytes) if present.
	uint32_t dataSize;					///< Size of data in bytes.
};

PSD_NAMESPACE_END
