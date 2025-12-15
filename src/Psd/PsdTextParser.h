// Contributed by Adam @ NewBlue Inc.
// Licensed under the 2-clause BSD License.

#pragma once

#include <string>

#include "PsdNamespace.h"

PSD_NAMESPACE_BEGIN

struct Layer;
class Allocator;

/// Extracts style run metadata from Type Tool EngineData and attaches it to the given layer.
/// Caller must ensure layer->text is allocated and layer->text->styleRuns/styleRunCount are initialized.
void ParseEngineStyleRuns(const std::string& engineData, Layer* layer, Allocator* allocator);

PSD_NAMESPACE_END
