// Minimal GoogleTest coverage for text EngineData parsing.

#include "gtest/gtest.h"

#include "Psd/PsdPch.h"
#include "Psd/PsdLayer.h"
#include "Psd/PsdMallocAllocator.h"
#include "Psd/PsdMemoryUtil.h"
#include "Psd/PsdText.h"
#include "Psd/PsdTextParser.h"

PSD_USING_NAMESPACE;

namespace
{
	TEST(PsdTextParser, ParsesStyleRunsFromEngineData)
	{
		const char* engineData =
			"/EngineDict 7<<"
			"/StyleSheetSet ["
				"{ /FontName (Helvetica) /FontPostScriptName (HelveticaPS) /FontSize 12.0 /FauxBold true /FauxItalic true }"
				"{ /FontName (Courier New) /FontPostScriptName (CourierPS) /FontSize 10.5 }"
			"]"
			"/RunArray ["
				"{ /StyleSheet 0 }"
				"{ /StyleSheet 1 }"
			"]"
			"/RunLengthArray [5 3]"
			">>";

		MallocAllocator allocator;
		Layer layer = {};
		layer.text = memoryUtil::Allocate<LayerText>(&allocator);
		layer.text->styleRuns = nullptr;
		layer.text->styleRunCount = 0u;

		ParseEngineStyleRuns(engineData, &layer, &allocator);

		ASSERT_NE(layer.text->styleRuns, nullptr);
		ASSERT_EQ(layer.text->styleRunCount, 2u);

		const LayerText::StyleRun& first = layer.text->styleRuns[0];
		EXPECT_EQ(first.start, 0u);
		EXPECT_EQ(first.length, 5u);
		EXPECT_STREQ(first.fontName.c_str(), "Helvetica");
		EXPECT_STREQ(first.fontPostScriptName.c_str(), "HelveticaPS");
		EXPECT_NEAR(first.fontSize, 12.0f, 0.01f);
		EXPECT_TRUE(first.fauxBold);
		EXPECT_TRUE(first.fauxItalic);

		const LayerText::StyleRun& second = layer.text->styleRuns[1];
		EXPECT_EQ(second.start, 5u);
		EXPECT_EQ(second.length, 3u);
		EXPECT_STREQ(second.fontName.c_str(), "Courier New");
		EXPECT_STREQ(second.fontPostScriptName.c_str(), "CourierPS");
		EXPECT_NEAR(second.fontSize, 10.5f, 0.01f);
		EXPECT_FALSE(second.fauxBold);
		EXPECT_FALSE(second.fauxItalic);

		memoryUtil::FreeArray(&allocator, layer.text->styleRuns);
		memoryUtil::Free(&allocator, layer.text);
	}
}
