// Prototype TouchDesigner CPlusPlus POP geometry extraction benchmark.
// This measures POP CPU-buffer access for generic point/vertex/primitive
// attributes before deciding whether to build the production native backend.

#include "POP_CPlusPlusBase.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

const char* className(POP_AttributeClass c)
{
	switch (c)
	{
		case POP_AttributeClass::Point: return "point";
		case POP_AttributeClass::Vertex: return "vertex";
		case POP_AttributeClass::Primitive: return "primitive";
		default: return "unknown";
	}
}

const char* typeName(POP_AttributeType t)
{
	switch (t)
	{
		case POP_AttributeType::Float: return "float";
		case POP_AttributeType::Double: return "double";
		case POP_AttributeType::Int32: return "int32";
		case POP_AttributeType::UInt32: return "uint32";
		default: return "unknown";
	}
}
}

class TDPopProbe : public POP_CPlusPlusBase
{
public:
	TDPopProbe(const OP_NodeInfo*, POP_Context*) {}
	~TDPopProbe() override = default;

	void getGeneralInfo(POP_GeneralInfo* ginfo, const OP_Inputs*, void*) override
	{
		ginfo->cookEveryFrame = false;
		ginfo->cookEveryFrameIfAsked = false;
	}

	void execute(POP_Output*, const OP_Inputs* inputs, void*) override
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
		const OP_POPInput* in = inputs->getInputPOP(0);
		if (!in)
		{
			myTotalMs = msSince(totalStart);
			return;
		}

		POP_GetBufferInfo getInfo;
		getInfo.location = POP_BufferLocation::CPU;
		std::filesystem::path path = std::filesystem::temp_directory_path()
			/ "td_sop_usd_pop_probe.bin";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		auto t = std::chrono::high_resolution_clock::now();
		POP_InfoBuffers infoBuffers;
		in->getAllInfoBuffers(&infoBuffers, getInfo, nullptr);
		OP_SmartRef<POP_Buffer> indexBuf;
		const POP_IndexBuffer* index = in->getIndexBuffer(nullptr);
		if (index)
			indexBuf = index->getBuffer(getInfo, nullptr);
		myDownloadMs = msSince(t);

		t = std::chrono::high_resolution_clock::now();
		if (infoBuffers.pointInfo)
		{
			const POP_PointInfo* pointInfo = static_cast<const POP_PointInfo*>(
				infoBuffers.pointInfo->getData(nullptr));
			if (pointInfo)
				myPoints = pointInfo->numPoints;
			writeRaw(out, infoBuffers.pointInfo->getData(nullptr),
				static_cast<size_t>(infoBuffers.pointInfo->info.size),
				myBytesWritten);
		}
		if (infoBuffers.topoInfo)
		{
			const POP_TopologyInfo* topoInfo =
				static_cast<const POP_TopologyInfo*>(
					infoBuffers.topoInfo->getData(nullptr));
			if (topoInfo)
			{
				myPrims = topoInfo->getNumPrimitives();
				myVertices = topoInfo->getNumVerticies();
			}
			writeRaw(out, infoBuffers.topoInfo->getData(nullptr),
				static_cast<size_t>(infoBuffers.topoInfo->info.size),
				myBytesWritten);
		}
		if (indexBuf)
			writeRaw(out, indexBuf->getData(nullptr),
				static_cast<size_t>(indexBuf->info.size), myBytesWritten);

		writeAttributes(out, in, POP_AttributeClass::Point, getInfo);
		writeAttributes(out, in, POP_AttributeClass::Vertex, getInfo);
		writeAttributes(out, in, POP_AttributeClass::Primitive, getInfo);
		out.close();
		myWriteMs = msSince(t);
		myTotalMs = msSince(totalStart);
	}

	int32_t getNumInfoCHOPChans(void*) override { return 11; }

	void getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void*) override
	{
		static const char* names[] = {
			"total_ms", "download_ms", "write_ms",
			"points", "vertices", "prims",
			"point_attrs", "vertex_attrs", "prim_attrs",
			"bytes_written", "cook_count",
		};
		float vals[] = {
			static_cast<float>(myTotalMs),
			static_cast<float>(myDownloadMs),
			static_cast<float>(myWriteMs),
			static_cast<float>(myPoints),
			static_cast<float>(myVertices),
			static_cast<float>(myPrims),
			static_cast<float>(myPointAttrs),
			static_cast<float>(myVertexAttrs),
			static_cast<float>(myPrimAttrs),
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
			entries->values[0]->setString("schema");
			return;
		}
		const int32_t row = index - 1;
		if (row >= 0 && row < static_cast<int32_t>(myRows.size()))
			entries->values[0]->setString(myRows[row].c_str());
	}

private:
	void resetMetrics()
	{
		myTotalMs = myDownloadMs = myWriteMs = 0.0;
		myPoints = myVertices = myPrims = 0;
		myPointAttrs = myVertexAttrs = myPrimAttrs = 0;
		myBytesWritten = 0;
	}

	void writeAttributes(std::ofstream& out, const OP_POPInput* in,
		POP_AttributeClass klass, const POP_GetBufferInfo& getInfo)
	{
		uint32_t count = in->getNumAttributes(klass);
		if (klass == POP_AttributeClass::Point)
			myPointAttrs = count;
		else if (klass == POP_AttributeClass::Vertex)
			myVertexAttrs = count;
		else if (klass == POP_AttributeClass::Primitive)
			myPrimAttrs = count;

		for (uint32_t i = 0; i < count; ++i)
		{
			const POP_Attribute* attr = in->getAttribute(klass, i, nullptr);
			if (!attr)
				continue;
			OP_SmartRef<POP_Buffer> buf = attr->getBuffer(getInfo, nullptr);
			if (!buf)
				continue;
			writeRaw(out, buf->getData(nullptr),
				static_cast<size_t>(buf->info.size), myBytesWritten);
			const POP_AttributeInfo& info = attr->info;
			myRows.push_back(std::string(className(klass)) + ":" +
				(info.name ? info.name : "") + ":" +
				std::to_string(info.numComponents) + "x" +
				std::to_string(info.numColumns) + ":" +
				typeName(info.type) + ":bytes=" +
				std::to_string(static_cast<uint64_t>(buf->info.size)));
		}
	}

	std::vector<std::string> myRows;
	double myTotalMs = 0.0;
	double myDownloadMs = 0.0;
	double myWriteMs = 0.0;
	uint32_t myPoints = 0;
	uint32_t myVertices = 0;
	uint32_t myPrims = 0;
	uint32_t myPointAttrs = 0;
	uint32_t myVertexAttrs = 0;
	uint32_t myPrimAttrs = 0;
	uint64_t myBytesWritten = 0;
	int32_t myCookCount = 0;
};

extern "C"
{
DLLEXPORT void FillPOPPluginInfo(POP_PluginInfo* info)
{
	if (!info->setAPIVersion(POPCPlusPlusAPIVersion))
		return;
	info->customOPInfo.opType->setString("Tdpopprobe");
	info->customOPInfo.opLabel->setString("TD POP Probe");
	info->customOPInfo.opIcon->setString("TUP");
	info->customOPInfo.authorName->setString("TD-SOP-USD-Anim-Bridge");
	info->customOPInfo.authorEmail->setString("");
	info->customOPInfo.minInputs = 1;
	info->customOPInfo.maxInputs = 1;
}

DLLEXPORT POP_CPlusPlusBase* CreatePOPInstance(
	const OP_NodeInfo* info, POP_Context* context)
{
	return new TDPopProbe(info, context);
}

DLLEXPORT void DestroyPOPInstance(POP_CPlusPlusBase* instance)
{
	delete static_cast<TDPopProbe*>(instance);
}
}
