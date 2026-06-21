// Prototype TouchDesigner CPlusPlus SOP geometry extraction benchmark.
// This is not the exporter. It measures native SOP input access and raw chunk
// writes so we can compare against Python SOP traversal before changing design.

#include "SOP_CPlusPlusBase.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace TD;

namespace
{
double msSince(std::chrono::high_resolution_clock::time_point start)
{
	auto end = std::chrono::high_resolution_clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
}

void writeRaw(std::ofstream& out, const void* data, size_t bytes, uint64_t& total)
{
	if (!data || bytes == 0 || !out.good())
		return;
	out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
	total += static_cast<uint64_t>(bytes);
}

const char* attribTypeName(AttribType type)
{
	return type == AttribType::Int ? "int" : "float";
}

const char* attribSetName(AttribSet set)
{
	switch (set)
	{
		case AttribSet::Point: return "point";
		case AttribSet::Vertex: return "vertex";
		case AttribSet::Primitive: return "prim";
		default: return "invalid";
	}
}

std::vector<std::string> splitNames(const char* raw)
{
	std::vector<std::string> names;
	std::istringstream stream(raw ? raw : "");
	std::string name;
	while (stream >> name)
		names.push_back(name);
	return names;
}

std::string customSummary(const SOP_CustomAttribData* attr, size_t assumedElems)
{
	if (!attr)
		return "missing";
	std::ostringstream row;
	row << (attr->name ? attr->name : "<null>")
		<< ": comps=" << attr->numComponents
		<< " type=" << attribTypeName(attr->attribType)
		<< " floatPtr=" << (attr->floatData ? "yes" : "no")
		<< " intPtr=" << (attr->intData ? "yes" : "no")
		<< " assumedPointElems=" << assumedElems;
	if (attr->attribType == AttribType::Float && attr->floatData &&
		attr->numComponents > 0 && assumedElems > 0)
	{
		row << " first=(";
		for (int32_t c = 0; c < attr->numComponents; ++c)
		{
			if (c)
				row << ",";
			row << attr->floatData[c];
		}
		row << ")";
	}
	return row.str();
}
}

class TDSopProbe : public SOP_CPlusPlusBase
{
public:
	explicit TDSopProbe(const OP_NodeInfo*) {}
	~TDSopProbe() override = default;

	void getGeneralInfo(SOP_GeneralInfo* ginfo, const OP_Inputs*, void*) override
	{
		ginfo->cookEveryFrameIfAsked = false;
		ginfo->directToGPU = false;
		ginfo->winding = SOP_Winding::CCW;
	}

	void execute(SOP_Output*, const OP_Inputs* inputs, void*) override
	{
		++myCookCount;
		resetMetrics();
		myRows.clear();

		const auto totalStart = std::chrono::high_resolution_clock::now();
		if (inputs->getNumInputs() <= 0)
		{
			myTotalMs = msSince(totalStart);
			return;
		}
		const OP_SOPInput* in = inputs->getInputSOP(0);
		if (!in)
		{
			myTotalMs = msSince(totalStart);
			return;
		}

		myPoints = in->getNumPoints();
		myPrims = in->getNumPrimitives();
		myCustomAttribs = in->getNumCustomAttributes();
		myRows.push_back("input: path=" + std::string(in->opPath ? in->opPath : "")
			+ " points=" + std::to_string(myPoints)
			+ " rawVertices=" + std::to_string(in->getNumVertices())
			+ " prims=" + std::to_string(myPrims)
			+ " customCount=" + std::to_string(myCustomAttribs));

		std::filesystem::path path = std::filesystem::temp_directory_path()
			/ "td_sop_usd_native_probe.bin";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		auto t = std::chrono::high_resolution_clock::now();
		const Position* positions = in->getPointPositions();
		writeRaw(out, positions,
			static_cast<size_t>(myPoints) * sizeof(Position), myBytesWritten);
		myPointMs = msSince(t);

		t = std::chrono::high_resolution_clock::now();
		std::vector<int32_t> counts;
		counts.reserve(static_cast<size_t>(myPrims));
		int32_t summedVertices = 0;
		for (int32_t i = 0; i < myPrims; ++i)
		{
			const SOP_PrimitiveInfo& prim = in->getPrimitive(i);
			counts.push_back(prim.numVertices);
			summedVertices += prim.numVertices;
		}
		myVertices = summedVertices;
		writeRaw(out, counts.data(), counts.size() * sizeof(int32_t),
			myBytesWritten);
		const int32_t* allIndices =
			const_cast<OP_SOPInput*>(in)->getAllPrimPointIndices();
		writeRaw(out, allIndices,
			static_cast<size_t>(myVertices) * sizeof(int32_t), myBytesWritten);
		myTopologyMs = msSince(t);

		t = std::chrono::high_resolution_clock::now();
		const SOP_NormalInfo* normals = in->getNormals();
		if (normals && normals->normals && normals->numNormals > 0)
		{
			myRows.push_back("standard:N set=" +
				std::string(attribSetName(normals->attribSet)) +
				" count=" + std::to_string(normals->numNormals));
			writeRaw(out, normals->normals,
				static_cast<size_t>(normals->numNormals) * sizeof(Vector),
				myBytesWritten);
		}
		else
		{
			myRows.push_back("standard:N missing");
		}
		const SOP_ColorInfo* colors = in->getColors();
		if (colors && colors->colors && colors->numColors > 0)
		{
			myRows.push_back("standard:Cd point set=" +
				std::string(attribSetName(colors->attribSet)) +
				" count=" + std::to_string(colors->numColors));
			writeRaw(out, colors->colors,
				static_cast<size_t>(colors->numColors) * sizeof(Color),
				myBytesWritten);
		}
		const SOP_TextureInfo* textures = in->getTextures();
		if (textures && textures->textures && textures->numTextureLayers > 0)
		{
			myRows.push_back("standard:uv point set=" +
				std::string(attribSetName(textures->attribSet)) +
				" count=" + std::to_string(textures->numTextures) +
				" layers=" + std::to_string(textures->numTextureLayers));
			writeRaw(out, textures->textures,
				static_cast<size_t>(myPoints) *
					static_cast<size_t>(textures->numTextureLayers) *
					sizeof(TexCoord),
				myBytesWritten);
		}
		const SOP_ColorInfo* vtxColors = in->getVtxColors();
		if (vtxColors && vtxColors->colors && vtxColors->numColors > 0)
		{
			myRows.push_back("standard:Cd vertex set=" +
				std::string(attribSetName(vtxColors->attribSet)) +
				" count=" + std::to_string(vtxColors->numColors));
			writeRaw(out, vtxColors->colors,
				static_cast<size_t>(vtxColors->numColors) * sizeof(Color),
				myBytesWritten);
		}
		const SOP_TextureInfo* vtxTextures = in->getVtxTextures();
		if (vtxTextures && vtxTextures->textures &&
			vtxTextures->numTextureLayers > 0)
		{
			myRows.push_back("standard:uv vertex set=" +
				std::string(attribSetName(vtxTextures->attribSet)) +
				" count=" + std::to_string(vtxTextures->numTextures) +
				" layers=" + std::to_string(vtxTextures->numTextureLayers));
			writeRaw(out, vtxTextures->textures,
				static_cast<size_t>(myVertices) *
					static_cast<size_t>(vtxTextures->numTextureLayers) *
					sizeof(TexCoord),
				myBytesWritten);
		}
		const SOP_ColorInfo* primColors = in->getPrimColors();
		if (primColors && primColors->colors && primColors->numColors > 0)
		{
			myRows.push_back("standard:Cd prim set=" +
				std::string(attribSetName(primColors->attribSet)) +
				" count=" + std::to_string(primColors->numColors));
			writeRaw(out, primColors->colors,
				static_cast<size_t>(primColors->numColors) * sizeof(Color),
				myBytesWritten);
		}
		for (int32_t i = 0; i < myCustomAttribs; ++i)
		{
			const SOP_CustomAttribData* attr = in->getCustomAttribute(i);
			if (!attr || attr->numComponents <= 0)
				continue;
			myRows.push_back("custom:index[" + std::to_string(i) + "] " +
				customSummary(attr, static_cast<size_t>(myPoints)));
			size_t values = static_cast<size_t>(myPoints) *
				static_cast<size_t>(attr->numComponents);
			if (attr->attribType == AttribType::Float)
				writeRaw(out, attr->floatData, values * sizeof(float),
					myBytesWritten);
			else
				writeRaw(out, attr->intData, values * sizeof(int32_t),
					myBytesWritten);
		}
		for (const std::string& name : splitNames(inputs->getParString("Names")))
		{
			const SOP_CustomAttribData* attr = in->getCustomAttribute(name.c_str());
			myRows.push_back("custom:lookup[" + name + "] " +
				customSummary(attr, static_cast<size_t>(myPoints)));
		}
		out.close();
		myAttrMs = msSince(t);
		myTotalMs = msSince(totalStart);
	}

	void executeVBO(SOP_VBOOutput*, const OP_Inputs*, void*) override {}

	int32_t getNumInfoCHOPChans(void*) override { return 10; }

	void getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void*) override
	{
		static const char* names[] = {
			"total_ms", "points_ms", "topology_ms", "attrs_ms",
			"points", "vertices", "prims", "custom_attribs",
			"bytes_written", "cook_count",
		};
		float vals[] = {
			static_cast<float>(myTotalMs),
			static_cast<float>(myPointMs),
			static_cast<float>(myTopologyMs),
			static_cast<float>(myAttrMs),
			static_cast<float>(myPoints),
			static_cast<float>(myVertices),
			static_cast<float>(myPrims),
			static_cast<float>(myCustomAttribs),
			static_cast<float>(myBytesWritten),
			static_cast<float>(myCookCount),
		};
		chan->name->setString(names[index]);
		chan->value = vals[index];
	}

	bool getInfoDATSize(OP_InfoDATSize* infoSize, void*) override
	{
		infoSize->rows = static_cast<int32_t>(myRows.size()) + 1;
		infoSize->cols = 1;
		infoSize->byColumn = false;
		return true;
	}

	void getInfoDATEntries(int32_t index, int32_t,
		OP_InfoDATEntries* entries, void*) override
	{
		if (index == 0)
		{
			entries->values[0]->setString("sop_probe");
			return;
		}
		const int32_t row = index - 1;
		if (row >= 0 && row < static_cast<int32_t>(myRows.size()))
			entries->values[0]->setString(myRows[row].c_str());
	}

	void setupParameters(OP_ParameterManager* manager, void*) override
	{
		OP_StringParameter sp;
		sp.name = "Names";
		sp.label = "Lookup Names";
		sp.defaultValue = "test T uv N Cd";
		manager->appendString(sp);
	}

private:
	void resetMetrics()
	{
		myTotalMs = myPointMs = myTopologyMs = myAttrMs = 0.0;
		myPoints = myVertices = myPrims = myCustomAttribs = 0;
		myBytesWritten = 0;
	}

	std::vector<std::string> myRows;
	double myTotalMs = 0.0;
	double myPointMs = 0.0;
	double myTopologyMs = 0.0;
	double myAttrMs = 0.0;
	int32_t myPoints = 0;
	int32_t myVertices = 0;
	int32_t myPrims = 0;
	int32_t myCustomAttribs = 0;
	uint64_t myBytesWritten = 0;
	int32_t myCookCount = 0;
};

extern "C"
{
DLLEXPORT void FillSOPPluginInfo(SOP_PluginInfo* info)
{
	if (!info->setAPIVersion(SOPCPlusPlusAPIVersion))
		return;
	info->customOPInfo.opType->setString("Tdsopusdprobe");
	info->customOPInfo.opLabel->setString("TD SOP USD Probe");
	info->customOPInfo.opIcon->setString("TUP");
	info->customOPInfo.authorName->setString("TD-SOP-USD-Anim-Bridge");
	info->customOPInfo.authorEmail->setString("");
	info->customOPInfo.minInputs = 1;
	info->customOPInfo.maxInputs = 1;
}

DLLEXPORT SOP_CPlusPlusBase* CreateSOPInstance(const OP_NodeInfo* info)
{
	return new TDSopProbe(info);
}

DLLEXPORT void DestroySOPInstance(SOP_CPlusPlusBase* instance)
{
	delete static_cast<TDSopProbe*>(instance);
}
}
