// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "PsdPch.h"
#include "PsdParseLayerMaskSection.h"

#include "PsdDocument.h"
#include "PsdLayer.h"
#include "PsdChannel.h"
#include "PsdChannelType.h"
#include "PsdLayerMask.h"
#include "PsdVectorMask.h"
#include "PsdText.h"
#include "PsdTextParser.h"
#include "PsdPlacedLayer.h"
#include "PsdColorMode.h"
#include "PsdCompressionType.h"
#include "PsdLayerType.h"
#include "PsdFile.h"
#include "PsdLayerMaskSection.h"
#include "PsdKey.h"
#include "PsdBitUtil.h"
#include "PsdEndianConversion.h"
#include "PsdSyncFileReader.h"
#include "PsdSyncFileUtil.h"
#include "PsdMemoryUtil.h"
#include "PsdDecompressRle.h"
#include "PsdAllocator.h"
#include "Psdminiz.h"
#include "Psdinttypes.h"
#include "PsdLog.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


PSD_NAMESPACE_BEGIN

namespace
{
	struct TextDescriptorData
	{
		util::FixedSizeString text;
		std::string engineData;
	};

	struct StyleSheet
	{
		util::FixedSizeString fontName;
		util::FixedSizeString fontPostScriptName;
		float32_t fontSize;
		bool fauxBold;
		bool fauxItalic;
		float32_t fillColorR;
		float32_t fillColorG;
		float32_t fillColorB;
		float32_t fillColorA;
	};

	struct MaskData
	{
		int32_t top;
		int32_t left;
		int32_t bottom;
		int32_t right;
		uint8_t defaultColor;
		bool isVectorMask;
	};


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static int64_t ReadMaskRectangle(SyncFileReader& reader, MaskData& maskData)
	{
		maskData.top = fileUtil::ReadFromFileBE<int32_t>(reader);
		maskData.left = fileUtil::ReadFromFileBE<int32_t>(reader);
		maskData.bottom = fileUtil::ReadFromFileBE<int32_t>(reader);
		maskData.right = fileUtil::ReadFromFileBE<int32_t>(reader);

		return 4u*sizeof(int32_t);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static int64_t ReadMaskDensity(SyncFileReader& reader, uint8_t& density)
	{
		density = fileUtil::ReadFromFileBE<uint8_t>(reader);
		return sizeof(uint8_t);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static int64_t ReadMaskFeather(SyncFileReader& reader, float64_t& feather)
	{
		feather = fileUtil::ReadFromFileBE<float64_t>(reader);
		return sizeof(float64_t);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static int64_t ReadMaskParameters(SyncFileReader& reader, uint8_t& layerDensity, float64_t& layerFeather, uint8_t& vectorDensity, float64_t& vectorFeather)
	{
		int64_t bytesRead = 0;

		const uint8_t flags = fileUtil::ReadFromFileBE<uint8_t>(reader);
		bytesRead += sizeof(uint8_t);

		const bool hasUserDensity = (flags & (1u << 0)) != 0;
		const bool hasUserFeather = (flags & (1u << 1)) != 0;
		const bool hasVectorDensity = (flags & (1u << 2)) != 0;
		const bool hasVectorFeather = (flags & (1u << 3)) != 0;
		if (hasUserDensity)
		{
			bytesRead += ReadMaskDensity(reader, layerDensity);
		}
		if (hasUserFeather)
		{
			bytesRead += ReadMaskFeather(reader, layerFeather);
		}
		if (hasVectorDensity)
		{
			bytesRead += ReadMaskDensity(reader, vectorDensity);
		}
		if (hasVectorFeather)
		{
			bytesRead += ReadMaskFeather(reader, vectorFeather);
		}

		return bytesRead;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void ApplyMaskData(const MaskData& maskData, float64_t feather, uint8_t density, T* layerMask)
	{
		layerMask->top = maskData.top;
		layerMask->left = maskData.left;
		layerMask->bottom = maskData.bottom;
		layerMask->right = maskData.right;
		layerMask->feather = feather;
		layerMask->density = density;
		layerMask->defaultColor = maskData.defaultColor;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static unsigned int GetWidth(const T* data)
	{
		if (data->right > data->left)
			return static_cast<unsigned int>(data->right - data->left);

		return 0u;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static unsigned int GetHeight(const T* data)
	{
		if (data->bottom > data->top)
			return static_cast<unsigned int>(data->bottom - data->top);

		return 0u;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void GetExtents(const T* data, unsigned int& width, unsigned int& height)
	{
		width = GetWidth(data);
		height = GetHeight(data);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void GetChannelExtents(const Layer* layer, const Channel* channel, unsigned int& width, unsigned int& height)
	{
		if (channel->type == channelType::TRANSPARENCY_MASK)
		{
			// the channel is the transparency mask, which has the same size as the layer
			return GetExtents(layer, width, height);
		}
		else if (channel->type == channelType::LAYER_OR_VECTOR_MASK)
		{
			// the channel is either the layer or vector mask, depending on how many masks there are in the layer.
			if (layer->vectorMask)
			{
				// a vector mask exists, so this always denotes a vector mask
				return GetExtents(layer->vectorMask, width, height);
			}
			else if (layer->layerMask)
			{
				// no vector mask exists, so the layer mask is the only mask left
				return GetExtents(layer->layerMask, width, height);
			}

			PSD_ASSERT(false, "The code failed to create a mask for this type internally. This should never happen.");
			width = 0;
			height = 0;
			return;
		}
		else if (channel->type == channelType::LAYER_MASK)
		{
			// this type is only valid when there are two masks stored, in which case this always denotes the layer mask
			return GetExtents(layer->layerMask, width, height);
		}

		// this is a color channel which has the same size as the layer
		return GetExtents(layer, width, height);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint16_t ReadBEUint16(const uint8_t*& ptr, const uint8_t* end)
	{
		if (ptr + sizeof(uint16_t) > end)
			return 0u;

		uint16_t value;
		memcpy(&value, ptr, sizeof(uint16_t));
		ptr += sizeof(uint16_t);
		return endianUtil::BigEndianToNative(value);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t ReadBEUint32(const uint8_t*& ptr, const uint8_t* end)
	{
		if (ptr + sizeof(uint32_t) > end)
			return 0u;

		uint32_t value;
		memcpy(&value, ptr, sizeof(uint32_t));
		ptr += sizeof(uint32_t);
		return endianUtil::BigEndianToNative(value);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static int32_t ReadBEInt32(const uint8_t*& ptr, const uint8_t* end)
	{
		return static_cast<int32_t>(ReadBEUint32(ptr, end));
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static float64_t ReadBEDouble(const uint8_t*& ptr, const uint8_t* end)
	{
		if (ptr + sizeof(uint64_t) > end)
			return 0.0;

		uint64_t bits;
		memcpy(&bits, ptr, sizeof(uint64_t));
		ptr += sizeof(uint64_t);
		bits = endianUtil::BigEndianToNative(bits);

		float64_t value;
		memcpy(&value, &bits, sizeof(float64_t));
		return value;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void AppendChar(util::FixedSizeString& target, char c)
	{
		target.Append(&c, 1u);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void AssignPossiblyUtf16String(const char* src, size_t count, util::FixedSizeString& out)
	{
		out.Clear();
		if (count >= 2u && static_cast<uint8_t>(src[0]) == 0xFEu && static_cast<uint8_t>(src[1]) == 0xFFu)
		{
			for (size_t i = 2u; i + 1u < count; i += 2u)
			{
				const uint16_t codeUnit = static_cast<uint8_t>(src[i]) << 8 | static_cast<uint8_t>(src[i+1]);
				if (codeUnit <= 0x7Fu)
					AppendChar(out, static_cast<char>(codeUnit));
				else
					AppendChar(out, '?');
				if (out.GetLength() >= util::FixedSizeString::CAPACITY - 1u)
					break;
			}
		}
		else
		{
			const size_t capped = (count < util::FixedSizeString::CAPACITY) ? count : util::FixedSizeString::CAPACITY - 1u;
			out.Append(src, capped);
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static bool ExtractStringToken(const std::string& data, const char* token, size_t blockStart, size_t blockEnd, util::FixedSizeString& out)
	{
		const size_t pos = data.find(token, blockStart);
		if (pos == std::string::npos || pos >= blockEnd)
			return false;

		const size_t start = pos + strlen(token);
		const size_t end = data.find(')', start);
		if (end == std::string::npos || end > blockEnd || end <= start)
			return false;

		const size_t count = end - start;
		AssignPossiblyUtf16String(data.c_str() + start, count, out);
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ReadUnicodeString(const uint8_t*& ptr, const uint8_t* end, util::FixedSizeString& out)
	{
		out.Clear();
		const uint32_t length = ReadBEUint32(ptr, end);
		const uint32_t maxChars = static_cast<uint32_t>(util::FixedSizeString::CAPACITY - 1u);
		const uint32_t charsToStore = (length > maxChars) ? maxChars : length;

		for (uint32_t i=0; i < charsToStore && (ptr + sizeof(uint16_t) <= end); ++i)
		{
			const uint16_t ch = ReadBEUint16(ptr, end);
			if (ch <= 0x7fu)
			{
				AppendChar(out, static_cast<char>(ch));
			}
			else
			{
				AppendChar(out, '?');
			}
		}

		// skip the remainder if the string was longer than what we can store
		const uint32_t remaining = (length > charsToStore) ? (length - charsToStore) : 0u;
		const uint64_t bytesToSkip = static_cast<uint64_t>(remaining) * sizeof(uint16_t);
		if (ptr + bytesToSkip <= end)
		{
			ptr += bytesToSkip;
		}
		else
		{
			ptr = end;
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static std::string ReadClassId(const uint8_t*& ptr, const uint8_t* end)
	{
		const uint32_t length = ReadBEUint32(ptr, end);
		if (length == 0u)
		{
			if (ptr + 4u > end)
				return std::string();

			std::string id(reinterpret_cast<const char*>(ptr), 4u);
			ptr += 4u;
			return id;
		}

		if (ptr + length > end)
			return std::string();

		std::string id(reinterpret_cast<const char*>(ptr), length);
		ptr += length;
		return id;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void SkipUnitFloat(const uint8_t*& ptr, const uint8_t* end)
	{
		// units (4 bytes) + value (8 bytes)
		const uint64_t bytesLeft = static_cast<uint64_t>(end - ptr);
		if (bytesLeft >= 12u)
		{
			ptr += 12u;
		}
		else
		{
			ptr = end;
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static bool ExtractColorArray(const std::string& data, const char* token, size_t startSearch, float32_t* outColor, unsigned int maxComponents)
	{
		if (!outColor)
			return false;

		const size_t pos = data.find(token, startSearch);
		if (pos == std::string::npos)
			return false;

		const size_t bracket = data.find('[', pos);
		if (bracket == std::string::npos)
			return false;

		const size_t endBracket = data.find(']', bracket);
		if (endBracket == std::string::npos || endBracket <= bracket)
			return false;

		for (unsigned int i=0; i<maxComponents; ++i)
			outColor[i] = -1.0f;

		size_t cursor = bracket + 1u;
		unsigned int idx = 0u;
		while (cursor < endBracket && idx < maxComponents)
		{
			char* endPtr = nullptr;
			const float32_t value = static_cast<float32_t>(strtod(data.c_str() + cursor, &endPtr));
			if (endPtr == data.c_str() + cursor)
				break;
			outColor[idx++] = value;
			cursor = static_cast<size_t>(endPtr - data.c_str());
		}
		return idx > 0u;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ExtractFontData(const std::string& engineData, LayerText* text)
	{
		if (text == nullptr)
			return;

		const char* fontNameKey = "/FontName (";
		const char* postScriptKey = "/FontPostScriptName (";
		const char* fauxBoldKey = "/FauxBold true";
		const char* fauxItalicKey = "/FauxItalic true";

		const size_t namePos = engineData.find(fontNameKey);
		if (namePos != std::string::npos)
		{
			const size_t start = namePos + strlen(fontNameKey);
			const size_t endPos = engineData.find(')', start);
			if (endPos != std::string::npos)
			{
				const size_t count = endPos - start;
				if (count > 0u)
				{
					text->fontName.Clear();
					const size_t capped = (count < util::FixedSizeString::CAPACITY) ? count : util::FixedSizeString::CAPACITY - 1u;
					text->fontName.Append(engineData.c_str() + start, capped);
				}
			}
		}

		const size_t psPos = engineData.find(postScriptKey);
		if (psPos != std::string::npos)
		{
			const size_t start = psPos + strlen(postScriptKey);
			const size_t endPos = engineData.find(')', start);
			if (endPos != std::string::npos)
			{
				const size_t count = endPos - start;
				if (count > 0u)
				{
					text->fontPostScriptName.Clear();
					const size_t capped = (count < util::FixedSizeString::CAPACITY) ? count : util::FixedSizeString::CAPACITY - 1u;
					text->fontPostScriptName.Append(engineData.c_str() + start, capped);
				}
			}
		}

		text->fauxBold = (engineData.find(fauxBoldKey) != std::string::npos);
		text->fauxItalic = (engineData.find(fauxItalicKey) != std::string::npos);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ExtractTextData(const std::string& engineData, LayerText* text)
	{
		if (!text)
			return;

		// crude parse of EngineData's /Text (...) entry
		const char* textKey = "/Text (";
		const size_t pos = engineData.find(textKey);
		if (pos == std::string::npos)
			return;

		const size_t start = pos + strlen(textKey);
		const size_t endPos = engineData.find(')', start);
		if (endPos == std::string::npos || endPos <= start)
			return;

		const size_t count = endPos - start;
		text->text.Clear();
		const char* src = engineData.c_str() + start;
		if (count >= 2u && static_cast<uint8_t>(src[0]) == 0xFEu && static_cast<uint8_t>(src[1]) == 0xFFu)
		{
			// UTF-16 BE content inside parentheses
			for (size_t i = 2u; i + 1u < count; i += 2u)
			{
				const uint16_t codeUnit = static_cast<uint8_t>(src[i]) << 8 | static_cast<uint8_t>(src[i+1]);
				if (codeUnit <= 0x7Fu)
				{
					AppendChar(text->text, static_cast<char>(codeUnit));
				}
				else
				{
					AppendChar(text->text, '?');
				}
				if (text->text.GetLength() >= util::FixedSizeString::CAPACITY - 1u)
					break;
			}
		}
		else
		{
			const size_t capped = (count < util::FixedSizeString::CAPACITY) ? count : util::FixedSizeString::CAPACITY - 1u;
			text->text.Append(src, capped);
		}
	}


	static bool ExtractBoolToken(const std::string& data, const char* token, size_t blockStart, size_t blockEnd)
	{
		const size_t pos = data.find(token, blockStart);
		return (pos != std::string::npos && pos < blockEnd);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static bool ExtractFloatToken(const std::string& data, const char* token, size_t blockStart, size_t blockEnd, float32_t& out)
	{
		const size_t pos = data.find(token, blockStart);
		if (pos == std::string::npos || pos >= blockEnd)
			return false;

		const size_t start = pos + strlen(token);
		if (start >= data.size())
			return false;

		char* endPtr = nullptr;
		out = static_cast<float32_t>(strtod(data.c_str() + start, &endPtr));
		const size_t parsedPos = static_cast<size_t>(endPtr - data.c_str());
		return (parsedPos > start && parsedPos <= blockEnd);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static bool ExtractIntToken(const std::string& data, const char* token, size_t blockStart, size_t blockEnd, int32_t& out)
	{
		const size_t pos = data.find(token, blockStart);
		if (pos == std::string::npos || pos >= blockEnd)
			return false;

		const size_t start = pos + strlen(token);
		if (start >= data.size())
			return false;

		char* endPtr = nullptr;
		out = static_cast<int32_t>(strtol(data.c_str() + start, &endPtr, 10));
		const size_t parsedPos = static_cast<size_t>(endPtr - data.c_str());
		return (parsedPos > start && parsedPos <= blockEnd);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseStyleSheetSet(const std::string& engineData, std::vector<StyleSheet>& sheets)
	{
		const size_t tag = engineData.find("/StyleSheetSet");
		if (tag == std::string::npos)
		{
			return;
		}

		const size_t arrayStart = engineData.find('[', tag);
		const size_t arrayEnd = engineData.find(']', arrayStart == std::string::npos ? tag : arrayStart);
		if (arrayStart == std::string::npos || arrayEnd == std::string::npos)
		{
			return;
		}

		size_t cur = engineData.find('{', arrayStart);
		int sheetIndex = 0;
		while (cur != std::string::npos && cur < arrayEnd)
		{
			const size_t blockEnd = engineData.find('}', cur);
			if (blockEnd == std::string::npos || blockEnd > arrayEnd)
				break;

			StyleSheet sheet = {};
			sheet.fontSize = 0.0f;
			sheet.fauxBold = false;
			sheet.fauxItalic = false;
			sheet.fillColorR = -1.0f;
			sheet.fillColorG = -1.0f;
			sheet.fillColorB = -1.0f;
			sheet.fillColorA = -1.0f;

			ExtractStringToken(engineData, "/FontName (", cur, blockEnd, sheet.fontName);
			ExtractStringToken(engineData, "/FontPostScriptName (", cur, blockEnd, sheet.fontPostScriptName);
			ExtractFloatToken(engineData, "/FontSize ", cur, blockEnd, sheet.fontSize);
			sheet.fauxBold = ExtractBoolToken(engineData, "/FauxBold true", cur, blockEnd);
			sheet.fauxItalic = ExtractBoolToken(engineData, "/FauxItalic true", cur, blockEnd);

			// Extract fill color from FillColor dictionary
			// Format: /FillColor << ... /Values [ R G B ] ... >>
			const size_t fillColorPos = engineData.find("/FillColor", cur);
			if (fillColorPos != std::string::npos && fillColorPos < blockEnd)
			{
				// Try with space: "/Values ["
				size_t valuesPos = engineData.find("/Values [", fillColorPos);
				// Try without space: "/Values["
				if (valuesPos == std::string::npos || valuesPos >= blockEnd)
					valuesPos = engineData.find("/Values[", fillColorPos);

				if (valuesPos != std::string::npos && valuesPos < blockEnd)
				{
					// Skip past "/Values[" or "/Values ["
					const char* ptr = engineData.c_str() + valuesPos;
					while (*ptr && *ptr != '[') ptr++;
					if (*ptr == '[') ptr++;  // Skip '['

					char* endPtr = nullptr;
					sheet.fillColorR = static_cast<float32_t>(strtod(ptr, &endPtr));
					if (endPtr && endPtr > ptr)
					{
						ptr = endPtr;
						sheet.fillColorG = static_cast<float32_t>(strtod(ptr, &endPtr));
						if (endPtr && endPtr > ptr)
						{
							ptr = endPtr;
							sheet.fillColorB = static_cast<float32_t>(strtod(ptr, &endPtr));
							sheet.fillColorA = 1.0f; // Default to opaque
						}
					}
				}
			}

			sheets.push_back(sheet);
			sheetIndex++;
			cur = engineData.find('{', blockEnd);
		}

	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseRunArray(const std::string& engineData, std::vector<uint32_t>& runSheetIndices)
	{
		const size_t tag = engineData.find("/RunArray");
		if (tag == std::string::npos)
			return;

		const size_t arrayStart = engineData.find('[', tag);
		const size_t arrayEnd = engineData.find(']', arrayStart == std::string::npos ? tag : arrayStart);
		if (arrayStart == std::string::npos || arrayEnd == std::string::npos)
			return;

		size_t cur = engineData.find('{', arrayStart);
		while (cur != std::string::npos && cur < arrayEnd)
		{
			const size_t blockEnd = engineData.find('}', cur);
			if (blockEnd == std::string::npos || blockEnd > arrayEnd)
				break;

			const size_t stylePos = engineData.find("/StyleSheet ", cur);
			if (stylePos != std::string::npos && stylePos < blockEnd)
			{
				const size_t start = stylePos + strlen("/StyleSheet ");
				char* endPtr = nullptr;
				const uint32_t value = static_cast<uint32_t>(strtoul(engineData.c_str() + start, &endPtr, 10));
				if (endPtr && static_cast<size_t>(endPtr - engineData.c_str()) <= blockEnd)
				{
					runSheetIndices.push_back(value);
				}
			}

			cur = engineData.find('{', blockEnd);
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseRunLengthArray(const std::string& engineData, std::vector<uint32_t>& runLengths)
	{
		const size_t tag = engineData.find("/RunLengthArray");
		if (tag == std::string::npos)
			return;

		const size_t arrayStart = engineData.find('[', tag);
		const size_t arrayEnd = engineData.find(']', arrayStart == std::string::npos ? tag : arrayStart);
		if (arrayStart == std::string::npos || arrayEnd == std::string::npos)
			return;

		size_t pos = arrayStart + 1u;
		while (pos < arrayEnd)
		{
			char* endPtr = nullptr;
			const uint32_t value = static_cast<uint32_t>(strtoul(engineData.c_str() + pos, &endPtr, 10));
			if (!endPtr || endPtr == engineData.c_str() + pos)
				break;

			runLengths.push_back(value);
			pos = static_cast<size_t>(endPtr - engineData.c_str());
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseParagraphJustification(const std::string& engineData, Layer* layer)
	{
		if (!layer || !layer->text)
			return;

		const size_t tag = engineData.find("/ParagraphSheetSet");
		if (tag == std::string::npos)
			return;

		const size_t arrayStart = engineData.find('[', tag);
		const size_t arrayEnd = engineData.find(']', arrayStart == std::string::npos ? tag : arrayStart);
		if (arrayStart == std::string::npos || arrayEnd == std::string::npos)
			return;

		const size_t blockStart = engineData.find('{', arrayStart);
		const size_t blockEnd = engineData.find('}', blockStart == std::string::npos ? arrayStart : blockStart);
		if (blockStart == std::string::npos || blockEnd == std::string::npos || blockEnd > arrayEnd)
			return;

		int32_t justification = -1;
		if (ExtractIntToken(engineData, "/Justification ", blockStart, blockEnd, justification))
		{
			layer->text->paragraphJustification = justification;
		}
		else
		{
			// fallback: search globally
			int32_t globalJust = -1;
			if (ExtractIntToken(engineData, "/Justification ", 0u, engineData.size(), globalJust))
			{
				layer->text->paragraphJustification = globalJust;
			}
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseFontSet(const std::string& engineData, LayerText* text)
	{
		if (!text || text->fontName.GetLength() > 0u)
			return;

		const size_t tag = engineData.find("/FontSet");
		if (tag == std::string::npos)
			return;

		const size_t namePos = engineData.find("/Name (", tag);
		if (namePos == std::string::npos)
			return;

		// find closing ')'
		const size_t start = namePos + strlen("/Name (");
		const size_t end = engineData.find(')', start);
		if (end == std::string::npos || end <= start)
			return;

		const size_t count = end - start;
		AssignPossiblyUtf16String(engineData.c_str() + start, count, text->fontName);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseEngineStyleRunsInternal(const std::string& engineData, Layer* layer, Allocator* allocator)
	{
		if (!layer || !layer->text || !allocator)
			return;

		// reset previous runs, if any
		memoryUtil::FreeArray(allocator, layer->text->styleRuns);
		layer->text->styleRuns = nullptr;
		layer->text->styleRunCount = 0u;

		std::vector<StyleSheet> sheets;
		ParseStyleSheetSet(engineData, sheets);

		std::vector<uint32_t> runSheetIndices;
		ParseRunArray(engineData, runSheetIndices);

		std::vector<uint32_t> runLengths;
		ParseRunLengthArray(engineData, runLengths);

		const size_t runCount = runSheetIndices.size();
		if (runCount == 0u || runCount != runLengths.size())
		{
			// Even if we can't create style runs, copy first sheet to layer level as fallback
			if (!sheets.empty() && layer->text)
			{
				const StyleSheet& firstSheet = sheets[0];

				if (layer->text->fontName.GetLength() == 0u)
					layer->text->fontName = firstSheet.fontName;
				if (layer->text->fontPostScriptName.GetLength() == 0u)
					layer->text->fontPostScriptName = firstSheet.fontPostScriptName;
				// Copy color to layer level if not already set
				if (layer->text->fillColorR < 0.0f && firstSheet.fillColorR >= 0.0f)
				{
					layer->text->fillColorR = firstSheet.fillColorR;
					layer->text->fillColorG = firstSheet.fillColorG;
					layer->text->fillColorB = firstSheet.fillColorB;
					layer->text->fillColorA = firstSheet.fillColorA;
				}
			}
			return;
		}

		if (sheets.empty())
			return;

		if (!layer->text)
			return;

		layer->text->styleRunCount = static_cast<uint32_t>(runCount);
		layer->text->styleRuns = memoryUtil::AllocateArray<LayerText::StyleRun>(allocator, runCount);

		uint32_t cursor = 0u;
		for (size_t i=0; i < runCount; ++i)
		{
			const uint32_t sheetIndex = runSheetIndices[i];
			const StyleSheet& sheet = (sheetIndex < sheets.size()) ? sheets[sheetIndex] : sheets.back();

			LayerText::StyleRun* run = &layer->text->styleRuns[i];
			run->start = cursor;
			run->length = runLengths[i];
			run->fontName = sheet.fontName;
			run->fontPostScriptName = sheet.fontPostScriptName;
			run->fontSize = sheet.fontSize;
			run->fauxBold = sheet.fauxBold;
			run->fauxItalic = sheet.fauxItalic;
			run->fillColorR = sheet.fillColorR;
			run->fillColorG = sheet.fillColorG;
			run->fillColorB = sheet.fillColorB;
			run->fillColorA = sheet.fillColorA;

			cursor += run->length;
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseDescriptorValue(const uint8_t*& ptr, const uint8_t* end, uint32_t type, const std::string& key, TextDescriptorData& out)
	{
		switch (type)
		{
			case util::Key<'T', 'E', 'X', 'T'>::VALUE:
			{
				if (key == "Txt ")
				{
					ReadUnicodeString(ptr, end, out.text);
				}
				else
				{
					util::FixedSizeString unusedText;
					ReadUnicodeString(ptr, end, unusedText);
				}
				break;
			}
			case util::Key<'t', 'd', 't', 'a'>::VALUE:
			{
				const uint32_t len = ReadBEUint32(ptr, end);
				const uint64_t remaining = static_cast<uint64_t>(end - ptr);
				const uint32_t toCopy = static_cast<uint32_t>(len > remaining ? remaining : len);
				if (key == "EngineData")
				{
					out.engineData.assign(reinterpret_cast<const char*>(ptr), toCopy);
				}
				ptr += len;
				if (ptr > end)
				{
					ptr = end;
				}
				break;
			}
			case util::Key<'l', 'o', 'n', 'g'>::VALUE:
			{
				ReadBEInt32(ptr, end);
				break;
			}
			case util::Key<'b', 'o', 'o', 'l'>::VALUE:
			{
				if (ptr < end)
					++ptr;
				break;
			}
			case util::Key<'d', 'o', 'u', 'b'>::VALUE:
			{
				ReadBEDouble(ptr, end);
				break;
			}
			case util::Key<'U', 'n', 't', 'F'>::VALUE:
			{
				SkipUnitFloat(ptr, end);
				break;
			}
			case util::Key<'e', 'n', 'u', 'm'>::VALUE:
			{
				ReadClassId(ptr, end); // type ID
				ReadClassId(ptr, end); // enum
				break;
			}
			case util::Key<'o', 'b', 'j', ' '>::VALUE:
			{
				ReadClassId(ptr, end);
				ParseDescriptorValue(ptr, end, util::Key<'D', 'e', 's', 'c'>::VALUE, key, out);
				break;
			}
			case util::Key<'V', 'l', 'L', 's'>::VALUE:
			{
				const uint32_t count = ReadBEUint32(ptr, end);
				for (uint32_t i=0; i < count && (ptr < end); ++i)
				{
					const uint32_t listType = ReadBEUint32(ptr, end);
					ParseDescriptorValue(ptr, end, listType, std::string(), out);
				}
				break;
			}
			case util::Key<'D', 'e', 's', 'c'>::VALUE:
			{
				util::FixedSizeString unused;
				ReadUnicodeString(ptr, end, unused);
				ReadClassId(ptr, end);
				const uint32_t itemCount = ReadBEUint32(ptr, end);
				for (uint32_t i=0; i < itemCount && (ptr < end); ++i)
				{
					const std::string itemKey = ReadClassId(ptr, end);
					const uint32_t itemType = ReadBEUint32(ptr, end);
					ParseDescriptorValue(ptr, end, itemType, itemKey, out);
				}
				break;
			}
			default:
			{
				// bail out if we don't know how to skip this type
				ptr = end;
				break;
			}
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseDescriptor(const uint8_t*& ptr, const uint8_t* end, TextDescriptorData& out)
	{
		util::FixedSizeString unused;
		ReadUnicodeString(ptr, end, unused);
		ReadClassId(ptr, end);

		const uint32_t itemCount = ReadBEUint32(ptr, end);
		for (uint32_t i=0; i < itemCount && (ptr < end); ++i)
		{
			const std::string key = ReadClassId(ptr, end);
			const uint32_t type = ReadBEUint32(ptr, end);
			ParseDescriptorValue(ptr, end, type, key, out);
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParseTypeTool(const uint8_t* data, uint32_t length, Layer* layer, Allocator* allocator)
	{
		if (length < sizeof(uint16_t))
			return;

		const uint8_t* ptr = data;
		const uint8_t* end = data + length;

		const uint16_t version = ReadBEUint16(ptr, end);
		PSD_UNUSED(version);

		// Save transform matrix before allocating layer->text
		float64_t transformMatrix[6];
		for (unsigned int i=0; i < 6u; ++i)
		{
			transformMatrix[i] = ReadBEDouble(ptr, end);
		}

		ReadBEUint16(ptr, end);

		TextDescriptorData descData;
		descData.text.Clear();
		ParseDescriptor(ptr, end, descData);

		ReadBEUint16(ptr, end);
		ParseDescriptor(ptr, end, descData);

		const int32_t boxLeft = ReadBEInt32(ptr, end);
		const int32_t boxTop = ReadBEInt32(ptr, end);
		const int32_t boxRight = ReadBEInt32(ptr, end);
		const int32_t boxBottom = ReadBEInt32(ptr, end);

		if (!layer->text)
		{
			layer->text = memoryUtil::Allocate<LayerText>(allocator);
			layer->text->text.Clear();
			layer->text->fontName.Clear();
			layer->text->fontPostScriptName.Clear();
			layer->text->fauxBold = false;
			layer->text->fauxItalic = false;
			layer->text->paragraphJustification = -1;
			for (unsigned int i=0; i<6u; ++i) layer->text->transform[i] = 0.0;
			layer->text->colorSpace = colorMode::RGB;
			for (unsigned int i=0; i<4u; ++i) layer->text->color[i] = -1.0f;
			layer->text->fillColorR = -1.0f;
			layer->text->fillColorG = -1.0f;
			layer->text->fillColorB = -1.0f;
			layer->text->fillColorA = -1.0f;
			layer->text->styleRuns = nullptr;
			layer->text->styleRunCount = 0u;
		}

		// Copy saved transform matrix
		for (unsigned int i=0; i<6u; ++i)
		{
			layer->text->transform[i] = transformMatrix[i];
		}

		if (descData.text.GetLength() > 0u)
		{
			layer->text->text = descData.text;
		}

		ExtractFontData(descData.engineData, layer->text);
		ParseFontSet(descData.engineData, layer->text);
		ExtractIntToken(descData.engineData, "/ColorSpace ", 0u, descData.engineData.size(), layer->text->colorSpace);
		ExtractColorArray(descData.engineData, "/FillColor", 0u, layer->text->color, 4u);
		if (layer->text->text.GetLength() == 0u)
		{
			ExtractTextData(descData.engineData, layer->text);
		}

		ParseEngineStyleRunsInternal(descData.engineData, layer, allocator);
		ParseParagraphJustification(descData.engineData, layer);

		if (layer->text->fontName.GetLength() == 0u && layer->text->styleRunCount > 0u)
		{
			layer->text->fontName = layer->text->styleRuns[0].fontName;
		}
		if (layer->text->fontPostScriptName.GetLength() == 0u && layer->text->styleRunCount > 0u)
		{
			layer->text->fontPostScriptName = layer->text->styleRuns[0].fontPostScriptName;
		}

		// Copy color from first style run to layer level for fallback
		if (layer->text->fillColorR < 0.0f && layer->text->styleRunCount > 0u)
		{
			if (layer->text->styleRuns[0].fillColorR >= 0.0f)
			{
				layer->text->fillColorR = layer->text->styleRuns[0].fillColorR;
				layer->text->fillColorG = layer->text->styleRuns[0].fillColorG;
				layer->text->fillColorB = layer->text->styleRuns[0].fillColorB;
				layer->text->fillColorA = layer->text->styleRuns[0].fillColorA;
			}
		}

		// fallback scan over raw buffer in case descriptor parsing missed fields
		if (layer->text->text.GetLength() == 0u || layer->text->fontName.GetLength() == 0u || layer->text->paragraphJustification < 0 || layer->text->color[0] < 0.0f)
		{
			std::string raw(reinterpret_cast<const char*>(data), length);
			if (layer->text->text.GetLength() == 0u)
				ExtractTextData(raw, layer->text);
			if (layer->text->fontName.GetLength() == 0u)
			{
				ExtractFontData(raw, layer->text);
				ParseFontSet(raw, layer->text);
			}
			if (layer->text->color[0] < 0.0f)
			{
				if (!ExtractIntToken(raw, "/ColorSpace ", 0u, raw.size(), layer->text->colorSpace))
					layer->text->colorSpace = colorMode::RGB;
				ExtractColorArray(raw, "/FillColor", 0u, layer->text->color, 4u);
			}
			if (layer->text->paragraphJustification < 0)
			{
				int32_t just = -1;
				if (ExtractIntToken(raw, "/Justification ", 0u, raw.size(), just))
				{
					layer->text->paragraphJustification = just;
				}
			}
		}

		layer->text->boxLeft = boxLeft;
		layer->text->boxTop = boxTop;
		layer->text->boxRight = boxRight;
		layer->text->boxBottom = boxBottom;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void ParsePlacedLayer(const uint8_t* data, uint32_t length, Layer* layer, Allocator* allocator)
	{
		if (!layer || !allocator || length == 0u)
			return;

		if (!layer->placed)
		{
			layer->placed = memoryUtil::Allocate<PlacedLayer>(allocator);
			layer->placed->type = 0u;
			layer->placed->uid.Clear();
			layer->placed->data = nullptr;
			layer->placed->dataSize = 0u;
		}

		// best effort parse: Photoshop stores a type (int32), version (int32), and a Pascal/Unicode ID before the payload.
		const uint8_t* ptr = data;
		const uint8_t* end = data + length;

		if (ptr + sizeof(uint32_t) <= end)
		{
			layer->placed->type = ReadBEUint32(ptr, end);
		}

		// skip version
		ReadBEUint32(ptr, end);

		// Attempt to read a Unicode string UID (count + chars)
		if (ptr + sizeof(uint32_t) <= end)
		{
			const uint32_t uidLength = ReadBEUint32(ptr, end);
			const uint32_t capped = (uidLength < util::FixedSizeString::CAPACITY) ? uidLength : util::FixedSizeString::CAPACITY - 1u;
			layer->placed->uid.Clear();
			for (uint32_t i=0; i < capped && (ptr + sizeof(uint16_t) <= end); ++i)
			{
				const uint16_t ch = ReadBEUint16(ptr, end);
				if (ch <= 0x7fu)
					AppendChar(layer->placed->uid, static_cast<char>(ch));
				else
					AppendChar(layer->placed->uid, '?');
			}

			// skip remaining characters if UID was longer
			const uint64_t remainingUidBytes = static_cast<uint64_t>(uidLength > capped ? uidLength - capped : 0u) * sizeof(uint16_t);
			if (ptr + remainingUidBytes <= end)
				ptr += remainingUidBytes;
			else
				ptr = end;
		}

		// Remaining bytes are treated as raw placed data.
		const uint32_t payloadSize = static_cast<uint32_t>(end - ptr);
		if (payloadSize > 0u)
		{
			layer->placed->data = static_cast<uint8_t*>(allocator->Allocate(payloadSize, 1u));
			layer->placed->dataSize = payloadSize;
			memcpy(layer->placed->data, ptr, payloadSize);
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint8_t GetChannelDefaultColor(const Layer* layer, const Channel* channel)
	{
		if (channel->type == channelType::TRANSPARENCY_MASK)
		{
			return 0u;
		}
		else if (channel->type == channelType::LAYER_OR_VECTOR_MASK)
		{
			if (layer->vectorMask)
			{
				return layer->vectorMask->defaultColor;
			}
			else if (layer->layerMask)
			{
				return layer->layerMask->defaultColor;
			}

			PSD_ASSERT(false, "The code failed to create a mask for this type internally. This should never happen.");
			return 0u;
		}
		else if (channel->type == channelType::LAYER_MASK)
		{
			return layer->layerMask->defaultColor;
		}

		return 0u;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void MoveChannelToMask(Channel* channel, T* mask)
	{
		mask->data = channel->data;
		mask->fileOffset = channel->fileOffset;

		channel->data = nullptr;
		channel->type = channelType::INVALID;
		channel->fileOffset = 0ull;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	void EndianConvert(void* src, unsigned int width, unsigned int height)
	{
		PSD_ASSERT_NOT_NULL(src);

		T* data = static_cast<T*>(src);
		const unsigned int size = width*height;

		for (unsigned int i=0; i < size; ++i)
		{
			data[i] = endianUtil::BigEndianToNative(data[i]);
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void* ReadChannelDataRaw(SyncFileReader& reader, Allocator* allocator, unsigned int width, unsigned int height)
	{
		const unsigned int size = width*height;
		if (size > 0)
		{
			void* planarData = allocator->Allocate(size*sizeof(T), 16u);
			reader.Read(planarData, size*sizeof(T));

			EndianConvert<T>(planarData, width, height);

			return planarData;
		}

		return nullptr;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void* ReadChannelDataRLE(SyncFileReader& reader, Allocator* allocator, unsigned int width, unsigned int height)
	{
		// the RLE-compressed data is preceded by a 2-byte data count for each scan line
		const unsigned int size = width*height;

		unsigned int rleDataSize = 0u;
		for (unsigned int i=0; i < height; ++i)
		{
			const uint16_t dataCount = fileUtil::ReadFromFileBE<uint16_t>(reader);
			rleDataSize += dataCount;
		}

		if (rleDataSize > 0)
		{
			void* planarData = allocator->Allocate(size*sizeof(T), 16u);

			// decompress RLE
			void* rleData = allocator->Allocate(rleDataSize, 4u);
			{
				reader.Read(rleData, rleDataSize);
				imageUtil::DecompressRle(static_cast<const uint8_t*>(rleData), rleDataSize, static_cast<uint8_t*>(planarData), width*height*sizeof(T));
			}
			allocator->Free(rleData);

			EndianConvert<T>(planarData, width, height);

			return planarData;
		}

		return nullptr;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void* ReadChannelDataZip(SyncFileReader& reader, Allocator* allocator, unsigned int width, unsigned int height, uint32_t channelSize)
	{
		if (channelSize > 0)
		{
			const unsigned int size = width*height;

			T* planarData = static_cast<T*>(allocator->Allocate(size*sizeof(T), 16));

			void* zipData = allocator->Allocate(channelSize, 4u);
			reader.Read(zipData, channelSize);

			// the zipped data stream has a zlib-header
			const size_t status = tinfl_decompress_mem_to_mem(planarData, size*sizeof(T), zipData, channelSize, TINFL_FLAG_PARSE_ZLIB_HEADER);
			if (status == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
			{
				PSD_ERROR("PsdExtract", "Error while unzipping channel data.");
			}

			allocator->Free(zipData);

			EndianConvert<T>(planarData, width, height);

			return planarData;
		}

		return nullptr;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void ApplyPrediction(Allocator* allocator, void* PSD_RESTRICT planarData, unsigned int width, unsigned int height)
	{
		static_assert(sizeof(T) == -1, "Unknown data type.");
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <>
	void ApplyPrediction<uint8_t>(Allocator*, void* PSD_RESTRICT planarData, unsigned int width, unsigned int height)
	{
		uint8_t* buffer = static_cast<uint8_t*>(planarData);
		for (unsigned int y = 0; y < height; ++y)
		{
			++buffer;
			for (unsigned int x = 1; x < width; ++x)
			{
				const uint32_t previous = buffer[-1];
				const uint32_t current = buffer[0];
				const uint32_t value = current + previous;

				*buffer++ = static_cast<uint8_t>(value & 0xFFu);
			}
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <>
	void ApplyPrediction<uint16_t>(Allocator*, void* PSD_RESTRICT planarData, unsigned int width, unsigned int height)
	{
		// 16-bit images are delta-encoded word-by-word.
		// the deltas are big-endian and must be reversed first for further processing. note that this is done
		// in-place with the delta-decoding.
		{
			uint16_t* buffer = static_cast<uint16_t*>(planarData);
			for (unsigned int y=0; y < height; ++y)
			{
				const uint16_t first = *buffer;
				*buffer++ = endianUtil::BigEndianToNative(first);
				for (unsigned int x=1; x < width; ++x)
				{
					buffer[0] = endianUtil::BigEndianToNative(buffer[0]);

					const uint32_t previous = buffer[-1];
					const uint32_t current = buffer[0];
					const uint32_t value = current + previous;

					// note that the data written here is now in little-endian format
					*buffer++ = static_cast<uint16_t>(value & 0xFFFFu);
				}
			}
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <>
	void ApplyPrediction<float32_t>(Allocator* allocator, void* PSD_RESTRICT planarData, unsigned int width, unsigned int height)
	{
		// delta-decode row by row first
		{
			uint8_t* buffer = static_cast<uint8_t*>(planarData);
			for (unsigned int y=0; y < height; ++y)
			{
				++buffer;
				for (unsigned int x=1; x < width*4; ++x)
				{
					const uint32_t previous = buffer[-1];
					const uint32_t current = buffer[0];
					const uint32_t value = current + previous;

					*buffer++ = static_cast<uint8_t>(value & 0xFFu);
				}
			}
		}

		// the bytes of the 32-bit float are stored in planar fashion per row, big-endian format.
		// interleave the bytes, and store them in little-endian format at the same time.
		uint8_t* rowData = static_cast<uint8_t*>(allocator->Allocate(width*sizeof(float32_t), 16));
		{
			uint8_t* dest = static_cast<uint8_t*>(planarData);
			for (unsigned int y=0; y < height; ++y)
			{
				// copy first row of data to backup storage, because it will be overwritten inside our loop.
				// note that this operation cannot be done in-place, that's why we work row by row.
				memcpy(rowData, dest, width*sizeof(float32_t));

				const uint8_t* src0 = rowData;
				const uint8_t* src1 = rowData + 1*width;
				const uint8_t* src2 = rowData + 2*width;
				const uint8_t* src3 = rowData + 3*width;

				for (unsigned int x=0; x < width; ++x)
				{
					// write data in little-endian format
					dest[0] = *src3++;
					dest[1] = *src2++;
					dest[2] = *src1++;
					dest[3] = *src0++;
					dest += 4u;
				}
			}
		}

		allocator->Free(rowData);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	template <typename T>
	static void* ReadChannelDataZipPrediction(SyncFileReader& reader, Allocator* allocator, unsigned int width, unsigned int height, uint32_t channelSize)
	{
		if (channelSize > 0)
		{
			const unsigned int size = width*height;

			T* planarData = static_cast<T*>(allocator->Allocate(size*sizeof(T), 16));

			void* zipData = allocator->Allocate(channelSize, 4u);
			reader.Read(zipData, channelSize);

			// the zipped data stream has a zlib-header
			const size_t status = tinfl_decompress_mem_to_mem(planarData, size*sizeof(T), zipData, channelSize, TINFL_FLAG_PARSE_ZLIB_HEADER);
			if (status == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
			{
				PSD_ERROR("PsdExtract", "Error while unzipping channel data.");
			}

			allocator->Free(zipData);

			// the data generated by applying the prediction data is already in little-endian format, so it doesn't have to be
			// endian converted further.
			ApplyPrediction<T>(allocator, planarData, width, height);

			return planarData;
		}

		return nullptr;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static LayerMaskSection* ParseLayer(const Document* document, SyncFileReader& reader, Allocator* allocator, uint64_t sectionOffset, uint32_t sectionLength, uint32_t layerLength)
	{
		LayerMaskSection* layerMaskSection = memoryUtil::Allocate<LayerMaskSection>(allocator);
		layerMaskSection->layers = nullptr;
		layerMaskSection->layerCount = 0u;
		layerMaskSection->overlayColorSpace = 0u;
		layerMaskSection->opacity = 0u;
		layerMaskSection->kind = 128u;
		layerMaskSection->hasTransparencyMask = false;

		if (layerLength != 0)
		{
			// read the layer count. if it is a negative number, its absolute value is the number of layers and the 
			// first alpha channel contains the transparency data for the merged result.
			// this will also be reflected in the channelCount of the document.
			int16_t layerCount = fileUtil::ReadFromFileBE<int16_t>(reader);
			layerMaskSection->hasTransparencyMask = (layerCount < 0);
			if (layerCount < 0)
				layerCount = -layerCount;

			layerMaskSection->layerCount = static_cast<unsigned int>(layerCount);
			layerMaskSection->layers = memoryUtil::AllocateArray<Layer>(allocator, layerMaskSection->layerCount);

			// read layer record for each layer
			for (unsigned int i=0; i < layerMaskSection->layerCount; ++i)
			{
				Layer* layer = &layerMaskSection->layers[i];
				layer->parent = nullptr;
				layer->utf16Name = nullptr;
				layer->text = nullptr;
				layer->layerMask = nullptr;
				layer->vectorMask = nullptr;
				layer->type = layerType::ANY;
				layer->isPassThrough = false;
				layer->text = nullptr;
				layer->placed = nullptr;

				layer->top = fileUtil::ReadFromFileBE<int32_t>(reader);
				layer->left = fileUtil::ReadFromFileBE<int32_t>(reader);
				layer->bottom = fileUtil::ReadFromFileBE<int32_t>(reader);
				layer->right = fileUtil::ReadFromFileBE<int32_t>(reader);

				// number of channels in the layer.
				// this includes channels for transparency, layer, and vector masks, if any.
				const uint16_t channelCount = fileUtil::ReadFromFileBE<uint16_t>(reader);
				layer->channelCount = channelCount;
				layer->channels = memoryUtil::AllocateArray<Channel>(allocator, channelCount);

				// parse each channel
				for (unsigned int j=0; j < channelCount; ++j)
				{
					Channel* channel = &layer->channels[j];
					channel->fileOffset = 0ull;
					channel->data = nullptr;
					channel->type = fileUtil::ReadFromFileBE<int16_t>(reader);
					channel->size = fileUtil::ReadFromFileBE<uint32_t>(reader);
				}

				// blend mode signature must be '8BIM'
				const uint32_t blendModeSignature = fileUtil::ReadFromFileBE<uint32_t>(reader);
				if (blendModeSignature != util::Key<'8', 'B', 'I', 'M'>::VALUE)
				{
					PSD_ERROR("LayerMaskSection", "Layer mask info section seems to be corrupt, signature does not match \"8BIM\".");
					return layerMaskSection;
				}

				layer->blendModeKey = fileUtil::ReadFromFileBE<uint32_t>(reader);
				layer->opacity = fileUtil::ReadFromFileBE<uint8_t>(reader);
				layer->clipping = fileUtil::ReadFromFileBE<uint8_t>(reader);

				// extract flag information into layer struct
				{
					const uint8_t flags = fileUtil::ReadFromFileBE<uint8_t>(reader);
					layer->isVisible = !((flags & (1u << 1)) != 0);
				}

				// skip filler byte
				{
					const uint8_t filler = fileUtil::ReadFromFileBE<uint8_t>(reader);
					PSD_UNUSED(filler);
				}

				const uint32_t extraDataLength = fileUtil::ReadFromFileBE<uint32_t>(reader);
				const uint32_t layerMaskDataLength = fileUtil::ReadFromFileBE<uint32_t>(reader);

				// the layer mask data section is weird. it may contain extra data for masks, such as density and feather parameters.
				// there are 3 main possibilities:
				//	*) length == zero		->	skip this section
				//	*) length == [20, 28]	->	there is one mask, and that could be either a layer or vector mask.
				//								the mask flags give rise to mask parameters. they store the mask type, and additional parameters, if any.
				//								there might be some padding at the end of this section, and its size depends on which parameters are there.
				//	*) length == [36, 56]	->	there are two masks. the first mask has parameters, but does NOT store flags yet.
				//								instead, there comes a second section with the same info (flags, default color, rectangle), and
				//								the parameters follow after that. there is also padding at the end of this second section.
				if (layerMaskDataLength != 0)
				{
					// there can be at most two masks, one layer and one vector mask
					MaskData maskData[2] = {};
					unsigned int maskCount = 1u;

					float64_t layerFeather = 0.0;
					float64_t vectorFeather = 0.0;
					uint8_t layerDensity = 0;
					uint8_t vectorDensity = 0;

					int64_t toRead = layerMaskDataLength;

					// enclosing rectangle
					toRead -= ReadMaskRectangle(reader, maskData[0]);

					maskData[0].defaultColor = fileUtil::ReadFromFileBE<uint8_t>(reader);
					toRead -= sizeof(uint8_t);

					const uint8_t maskFlags = fileUtil::ReadFromFileBE<uint8_t>(reader);
					toRead -= sizeof(uint8_t);

					maskData[0].isVectorMask = (maskFlags & (1u << 3)) != 0;
					bool maskHasParameters = (maskFlags & (1u << 4)) != 0;
					if (maskHasParameters && (layerMaskDataLength <= 28))
					{
						toRead -= ReadMaskParameters(reader, layerDensity, layerFeather, vectorDensity, vectorFeather);
					}

					// check if there is enough data left for another section of mask data
					if (toRead >= 18)
					{
						// in case there is still data left to read, the following values are for the real layer mask.
						// the data we just read was for the vector mask.
						maskCount = 2u;

						const uint8_t realFlags = fileUtil::ReadFromFileBE<uint8_t>(reader);
						toRead -= sizeof(uint8_t);

						maskData[1].defaultColor = fileUtil::ReadFromFileBE<uint8_t>(reader);
						toRead -= sizeof(uint8_t);

						toRead -= ReadMaskRectangle(reader, maskData[1]);

						maskData[1].isVectorMask = (realFlags & (1u << 3)) != 0;

						// note the OR here. whether the following section has mask parameter data or not is influenced by
						// the availability of parameter data of the previous mask!
						maskHasParameters |= ((realFlags & (1u << 4)) != 0);
						if (maskHasParameters)
						{
							toRead -= ReadMaskParameters(reader, layerDensity, layerFeather, vectorDensity, vectorFeather);
						}
					}

					// skip the remaining padding bytes, if any
					PSD_ASSERT(toRead >= 0, "Parsing failed, %" PRId64 "bytes left.", toRead);
					reader.Skip(static_cast<uint64_t>(toRead));

					// apply mask data to our own data structures
					for (unsigned int mask=0; mask < maskCount; ++mask)
					{
						const bool isVectorMask = maskData[mask].isVectorMask;
						if (isVectorMask)
						{
							PSD_ASSERT(layer->vectorMask == nullptr, "A vector mask already exists.");
							layer->vectorMask = memoryUtil::Allocate<VectorMask>(allocator);
							layer->vectorMask->data = nullptr;
							layer->vectorMask->fileOffset = 0ull;
							ApplyMaskData(maskData[mask], vectorFeather, vectorDensity, layer->vectorMask);
						}
						else
						{
							PSD_ASSERT(layer->layerMask == nullptr, "A layer mask already exists.");
							layer->layerMask = memoryUtil::Allocate<LayerMask>(allocator);
							layer->layerMask->data = nullptr;
							layer->layerMask->fileOffset = 0ull;
							ApplyMaskData(maskData[mask], layerFeather, layerDensity, layer->layerMask);
						}
					}
				}

				// skip blending ranges data, we are not interested in that for now
				const uint32_t layerBlendingRangesDataLength = fileUtil::ReadFromFileBE<uint32_t>(reader);
				reader.Skip(layerBlendingRangesDataLength);

				// the layer name is stored as pascal string, padded to a multiple of 4
				char layerName[512] = {};
				const uint8_t nameLength = fileUtil::ReadFromFileBE<uint8_t>(reader);
				const uint32_t paddedNameLength = bitUtil::RoundUpToMultiple(nameLength + 1u, 4u);
				reader.Read(layerName, paddedNameLength - 1u);

				layer->name.Assign(layerName);

				// read Additional Layer Information that exists since Photoshop 4.0.
				// getting the size of this data is a bit awkward, because it's not stored explicitly somewhere. furthermore,
				// the PSD format sometimes includes the 4-byte length in its section size, and sometimes not.
				const uint32_t additionalLayerInfoSize = extraDataLength - layerMaskDataLength - layerBlendingRangesDataLength - paddedNameLength - 8u;
				int64_t toRead = additionalLayerInfoSize;
				while (toRead > 0)
				{
					const uint32_t signature = fileUtil::ReadFromFileBE<uint32_t>(reader);
					if (signature != util::Key<'8', 'B', 'I', 'M'>::VALUE)
					{
						PSD_ERROR("LayerMaskSection", "Additional Layer Information section seems to be corrupt, signature does not match \"8BIM\".");
						return layerMaskSection;
					}

					const uint32_t key = fileUtil::ReadFromFileBE<uint32_t>(reader);

					// length needs to be rounded to an even number
					uint32_t length = fileUtil::ReadFromFileBE<uint32_t>(reader);
					length = bitUtil::RoundUpToMultiple(length, 2u);

					// read "Section divider setting" to identify whether a layer is a group, or a section divider
					if (key == util::Key<'l', 's', 'c', 't'>::VALUE)
					{
						layer->type = fileUtil::ReadFromFileBE<uint32_t>(reader);

						// there may be another blend mode here to tell us if the group is pass-through
						if(length >= 12u)
						{
							const uint32_t lsctKey = fileUtil::ReadFromFileBE<uint32_t>(reader);
							const uint32_t modeKey = fileUtil::ReadFromFileBE<uint32_t>(reader);
							if (lsctKey == util::Key<'8', 'B', 'I', 'M'>::VALUE && modeKey == util::Key<'p', 'a', 's', 's'>::VALUE)
							{
								layer->isPassThrough = true;
							}
							reader.Skip(length - 12u);
						}
						else
						{
							// skip the rest of the data
							reader.Skip(length - 4u);
						}
					}
					// read Unicode layer name
					else if (key == util::Key<'l', 'u', 'n', 'i'>::VALUE)
					{
						// PSD Unicode strings store 4 bytes for the number of characters, NOT bytes, followed by
						// 2-byte UTF16 Unicode data without the terminating null.
						const uint32_t characterCountWithoutNull = fileUtil::ReadFromFileBE<uint32_t>(reader);
						layer->utf16Name = memoryUtil::AllocateArray<uint16_t>(allocator, characterCountWithoutNull + 1u);

						for (uint32_t c = 0u; c < characterCountWithoutNull; ++c)
						{
							layer->utf16Name[c] = fileUtil::ReadFromFileBE<uint16_t>(reader);
						}
						layer->utf16Name[characterCountWithoutNull] = 0u;

						// skip possible padding bytes
						reader.Skip(length - 4u - characterCountWithoutNull * sizeof(uint16_t));
					}
					else if (key == util::Key<'T', 'y', 'S', 'h'>::VALUE || key == util::Key<'t', 'y', 'S', 'h'>::VALUE)
					{
						std::vector<uint8_t> buffer(length);
						reader.Read(buffer.data(), length);
						ParseTypeTool(buffer.data(), length, layer, allocator);
					}
					else if (key == util::Key<'S', 'o', 'L', 'd'>::VALUE || key == util::Key<'p', 'l', 'L', 'd'>::VALUE || key == util::Key<'P', 'l', 'L', 'd'>::VALUE)
					{
						std::vector<uint8_t> buffer(length);
						reader.Read(buffer.data(), length);
						ParsePlacedLayer(buffer.data(), length, layer, allocator);
					}
					else
					{
						reader.Skip(length);
					}

					toRead -= 3*sizeof(uint32_t) + length;
				}
			}

			// walk through the layers and channels, but don't extract their data just yet. only save the file offset for extracting the
			// data later.
			for (unsigned int i=0; i < layerMaskSection->layerCount; ++i)
			{
				Layer* layer = &layerMaskSection->layers[i];
				const unsigned int channelCount = layer->channelCount;
				for (unsigned int j=0; j < channelCount; ++j)
				{
					Channel* channel = &layer->channels[j];
					channel->fileOffset = reader.GetPosition();
					reader.Skip(channel->size);
				}
			}
		}

		if (sectionLength > 0)
		{
			// start loading at the global layer mask info section, located after the Layer Information Section.
			// note that the 4 bytes that stored the length of the section are not included in the length itself.
			const uint64_t globalInfoSectionOffset = sectionOffset + layerLength + 4u;
			reader.SetPosition(globalInfoSectionOffset);

			// work out how many bytes are left to read at this point. we need that to figure out the size of the last
			// optional section, the Additional Layer Information.
			if (sectionOffset + sectionLength > globalInfoSectionOffset)
			{
				int64_t toRead = static_cast<int64_t>(sectionOffset + sectionLength - globalInfoSectionOffset);
				const uint32_t globalLayerMaskLength = fileUtil::ReadFromFileBE<uint32_t>(reader);
				toRead -= sizeof(uint32_t);

				if (globalLayerMaskLength != 0)
				{
					layerMaskSection->overlayColorSpace = fileUtil::ReadFromFileBE<uint16_t>(reader);

					// 4*2 byte color components
					reader.Skip(8);

					layerMaskSection->opacity = fileUtil::ReadFromFileBE<uint16_t>(reader);
					layerMaskSection->kind = fileUtil::ReadFromFileBE<uint8_t>(reader);

					toRead -= 2u*sizeof(uint16_t) + sizeof(uint8_t) + 8u;

					// filler bytes (zeroes)
					const uint32_t remaining = globalLayerMaskLength - 2u*sizeof(uint16_t) - sizeof(uint8_t) - 8u;
					reader.Skip(remaining);

					toRead -= remaining;
				}

				// are there still bytes left to read? then this is the Additional Layer Information that exists since Photoshop 4.0.
				while (toRead > 0)
				{
					const uint32_t signature = fileUtil::ReadFromFileBE<uint32_t>(reader);
					if (signature != util::Key<'8', 'B', 'I', 'M'>::VALUE)
					{
						PSD_ERROR("AdditionalLayerInfo", "Additional Layer Information section seems to be corrupt, signature does not match \"8BIM\".");
						return layerMaskSection;
					}

					const uint32_t key = fileUtil::ReadFromFileBE<uint32_t>(reader);

					// again, length is rounded to a multiple of 4
					uint32_t length = fileUtil::ReadFromFileBE<uint32_t>(reader);
					length = bitUtil::RoundUpToMultiple(length, 4u);

					if (key == util::Key<'L', 'r', '1', '6'>::VALUE)
					{
						const uint64_t offset = reader.GetPosition();
						DestroyLayerMaskSection(layerMaskSection, allocator);
						layerMaskSection = ParseLayer(document, reader, allocator, 0u, 0u, length);
						reader.SetPosition(offset + length);
					}
					else if (key == util::Key<'L', 'r', '3', '2'>::VALUE)
					{
						const uint64_t offset = reader.GetPosition();
						DestroyLayerMaskSection(layerMaskSection, allocator);
						layerMaskSection = ParseLayer(document, reader, allocator, 0u, 0u, length);
						reader.SetPosition(offset + length);
					}
					else if (key == util::Key<'v', 'm', 's', 'k'>::VALUE)
					{
						// TODO: could read extra vector mask data here
						reader.Skip(length);
					}
					else if (key == util::Key<'l', 'n', 'k', '2'>::VALUE)
					{
						// TODO: could read individual smart object layer data here
						reader.Skip(length);
					}
					else
					{
						reader.Skip(length);
					}

					toRead -= 3u*sizeof(uint32_t) + length;
				}
			}
		}

		return layerMaskSection;
	}
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
LayerMaskSection* ParseLayerMaskSection(const Document* document, File* file, Allocator* allocator)
{
	PSD_ASSERT_NOT_NULL(file);
	PSD_ASSERT_NOT_NULL(allocator);

	// if there are no layers or masks, this section is just 4 bytes: the length field, which is set to zero.
	const Section& section = document->layerMaskInfoSection;
	if (section.length == 0)
	{
		PSD_ERROR("PSD", "Document does not contain a layer mask section.");
		return nullptr;
	}

	SyncFileReader reader(file);
	reader.SetPosition(section.offset);

	const uint32_t layerInfoSectionLength = fileUtil::ReadFromFileBE<uint32_t>(reader);
	LayerMaskSection* layerMaskSection = ParseLayer(document, reader, allocator, section.offset, section.length, layerInfoSectionLength);

	// build the layer hierarchy
	if (layerMaskSection && layerMaskSection->layers)
	{
		Layer* layerStack[256] = {};
		layerStack[0] = nullptr;
		int stackIndex = 0;

		for (unsigned int i=0; i < layerMaskSection->layerCount; ++i)
		{
			// note that it is much easier to build the hierarchy by traversing the layers backwards
			Layer* layer = &layerMaskSection->layers[layerMaskSection->layerCount - i - 1u];

			PSD_ASSERT(stackIndex >= 0 && stackIndex < 256, "Stack index is out of bounds.");
			layer->parent = layerStack[stackIndex];

			unsigned int width = 0u;
			unsigned int height = 0u;
			GetExtents(layer, width, height);

			const bool isGroupStart = (layer->type == layerType::OPEN_FOLDER) || (layer->type == layerType::CLOSED_FOLDER);
			const bool isGroupEnd = (layer->type == layerType::SECTION_DIVIDER);
			if (isGroupEnd)
			{
				--stackIndex;
			}
			else if (isGroupStart)
			{
				++stackIndex;
				layerStack[stackIndex] = layer;
			}
		}
	}

	return layerMaskSection;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void ExtractLayer(const Document* document, File* file, Allocator* allocator, Layer* layer)
{
	PSD_ASSERT_NOT_NULL(file);
	PSD_ASSERT_NOT_NULL(allocator);
	PSD_ASSERT_NOT_NULL(layer);

	SyncFileReader reader(file);

	const unsigned int channelCount = layer->channelCount;
	for (unsigned int i=0; i < channelCount; ++i)
	{
		Channel* channel = &layer->channels[i];
		reader.SetPosition(channel->fileOffset);

		unsigned int width = 0u;
		unsigned int height = 0u;
		GetChannelExtents(layer, channel, width, height);

		// channel data is stored in 4 different formats, which is denoted by a 2-byte integer
		PSD_ASSERT(channel->data == nullptr, "Channel data has already been loaded.");
		const uint16_t compressionType = fileUtil::ReadFromFileBE<uint16_t>(reader);
		if (compressionType == compressionType::RAW)
		{
			if (document->bitsPerChannel == 8)
			{
				channel->data = ReadChannelDataRaw<uint8_t>(reader, allocator, width, height);
			}
			else if (document->bitsPerChannel == 16)
			{
				channel->data = ReadChannelDataRaw<uint16_t>(reader, allocator, width, height);
			}
			else if (document->bitsPerChannel == 32)
			{
				channel->data = ReadChannelDataRaw<float32_t>(reader, allocator, width, height);
			}
		}
		else if (compressionType == compressionType::RLE)
		{
			if (document->bitsPerChannel == 8)
			{
				channel->data = ReadChannelDataRLE<uint8_t>(reader, allocator, width, height);
			}
			else if (document->bitsPerChannel == 16)
			{
				channel->data = ReadChannelDataRLE<uint16_t>(reader, allocator, width, height);
			}
			else if (document->bitsPerChannel == 32)
			{
				channel->data = ReadChannelDataRLE<float32_t>(reader, allocator, width, height);
			}
		}
		else if (compressionType == compressionType::ZIP)
		{
			// note that we need to subtract 2 bytes from the channel data size because we already read the uint16_t
			// for the compression type.
			PSD_ASSERT(channel->size >= 2, "Invalid channel data size %d.", channel->size);
			const uint32_t channelDataSize = channel->size - 2u;
			if (document->bitsPerChannel == 8)
			{
				channel->data = ReadChannelDataZip<uint8_t>(reader, allocator, width, height, channelDataSize);
			}
			else if (document->bitsPerChannel == 16)
			{
				channel->data = ReadChannelDataZip<uint16_t>(reader, allocator, width, height, channelDataSize);
			}
			else if (document->bitsPerChannel == 32)
			{
				// note that this is NOT a bug.
				// in 32-bit mode, Photoshop always interprets ZIP compression as being ZIP_WITH_PREDICTION, presumably to get better compression when writing files.
				channel->data = ReadChannelDataZipPrediction<float32_t>(reader, allocator, width, height, channelDataSize);
			}
		}
		else if (compressionType == compressionType::ZIP_WITH_PREDICTION)
		{
			// note that we need to subtract 2 bytes from the channel data size because we already read the uint16_t
			// for the compression type.
			PSD_ASSERT(channel->size >= 2, "Invalid channel data size %d.", channel->size);
			const uint32_t channelDataSize = channel->size - 2u;
			if (document->bitsPerChannel == 8)
			{
				channel->data = ReadChannelDataZipPrediction<uint8_t>(reader, allocator, width, height, channelDataSize);
			}
			else if (document->bitsPerChannel == 16)
			{
				channel->data = ReadChannelDataZipPrediction<uint16_t>(reader, allocator, width, height, channelDataSize);
			}
			else if (document->bitsPerChannel == 32)
			{
				channel->data = ReadChannelDataZipPrediction<float32_t>(reader, allocator, width, height, channelDataSize);
			}
		}
		else
		{
			PSD_ASSERT(false, "Unsupported compression type %d", compressionType);
			return;
		}

		// if the channel doesn't have any data assigned to it, check if it is a mask channel of any kind.
		// layer masks sometimes don't have any planar data stored for them, because they are
		// e.g. pure black or white, which means they only get assigned a default color.
		if (!channel->data)
		{
			if (channel->type < 0)
			{
				// this is a layer mask, so create planar data for it
				const size_t dataSize = width * height * document->bitsPerChannel / 8u;
				void* channelData = allocator->Allocate(dataSize, 16u);
				memset(channelData, GetChannelDefaultColor(layer, channel), dataSize);
				channel->data = channelData;
			}
			else
			{
				// for layers like groups and group end markers ("</Layer group>") it is ok to not store any data
			}
		}
	}

	// now move channel data to our own data structures for layer and vector masks, invalidating the info stored in
	// that channel.
	for (unsigned int i=0; i < channelCount; ++i)
	{
		Channel* channel = &layer->channels[i];
		if (channel->type == channelType::LAYER_OR_VECTOR_MASK)
		{
			if (layer->vectorMask)
			{
				// layer has a vector mask, so this type always denotes the vector mask
				PSD_ASSERT(!layer->vectorMask->data, "Vector mask data has already been assigned.");
				MoveChannelToMask(channel, layer->vectorMask);
			}
			else if (layer->layerMask)
			{
				// we don't have a vector but a layer mask, so this type denotes the layer mask
				PSD_ASSERT(!layer->layerMask->data, "Layer mask data has already been assigned.");
				MoveChannelToMask(channel, layer->layerMask);
			}
			else
			{
				PSD_ASSERT(false, "The code failed to create a mask for this type internally. This should never happen.");
			}
		}
		else if (channel->type == channelType::LAYER_MASK)
		{
			PSD_ASSERT(layer->layerMask, "Layer mask must already exist.");
			PSD_ASSERT(!layer->layerMask->data, "Layer mask data has already been assigned.");
			MoveChannelToMask(channel, layer->layerMask);
		}
		else
		{
			// this channel is either a color channel, or the transparency mask. those should be stored in our channel array,
			// so there's nothing to do.
		}
	}
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void ParseEngineStyleRuns(const std::string& engineData, Layer* layer, Allocator* allocator)
{
	ParseEngineStyleRunsInternal(engineData, layer, allocator);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void DestroyLayerMaskSection(LayerMaskSection*& section, Allocator* allocator)
{
	PSD_ASSERT_NOT_NULL(section);
	PSD_ASSERT_NOT_NULL(allocator);

	for (unsigned int i=0; i < section->layerCount; ++i)
	{
		Layer* layer = &section->layers[i];
		for (unsigned int j=0; j < layer->channelCount; ++j)
		{
			Channel* channel = &layer->channels[j];
			allocator->Free(channel->data);
		}

		memoryUtil::FreeArray(allocator, layer->utf16Name);

		memoryUtil::FreeArray(allocator, layer->channels);

		if (layer->layerMask)
		{
			allocator->Free(layer->layerMask->data);
		}
		memoryUtil::Free(allocator, layer->layerMask);

		if (layer->vectorMask)
		{
			allocator->Free(layer->vectorMask->data);
		}
		memoryUtil::Free(allocator, layer->vectorMask);

		if (layer->text)
		{
			memoryUtil::FreeArray(allocator, layer->text->styleRuns);
		}
		memoryUtil::Free(allocator, layer->text);

		if (layer->placed)
		{
			allocator->Free(layer->placed->data);
		}
		memoryUtil::Free(allocator, layer->placed);
	}
	memoryUtil::FreeArray(allocator, section->layers);
	memoryUtil::Free(allocator, section);
}

PSD_NAMESPACE_END
