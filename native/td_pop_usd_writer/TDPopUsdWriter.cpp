// Native animated .usdc chunk writer for TD-SOP-USD-Anim-Bridge.
//
// This CPlusPlus POP reads a POP input, preserving generic point/vertex/
// primitive attributes, and writes the binary chunk manifest used by
// tools/build_usdc_from_chunks.py. Python remains the playback orchestrator.

#include "POP_CPlusPlusBase.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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

std::string normPath(std::filesystem::path path)
{
	return path.lexically_normal().generic_string();
}

std::string jsonEscape(const std::string& value)
{
	std::string out;
	out.reserve(value.size() + 8);
	for (char c : value)
	{
		switch (c)
		{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out += c; break;
		}
	}
	return out;
}

const char* storageFloat()
{
#if defined(_WIN32)
	return "<f4";
#else
	const uint16_t one = 1;
	return *reinterpret_cast<const uint8_t*>(&one) ? "<f4" : ">f4";
#endif
}

const char* storageInt()
{
#if defined(_WIN32)
	return "<i4";
#else
	const uint16_t one = 1;
	return *reinterpret_cast<const uint8_t*>(&one) ? "<i4" : ">i4";
#endif
}

uint32_t componentBytes(POP_AttributeType type)
{
	switch (type)
	{
		case POP_AttributeType::Float: return 4;
		case POP_AttributeType::Double: return 8;
		case POP_AttributeType::Int32: return 4;
		case POP_AttributeType::UInt32: return 4;
		default: return 0;
	}
}

const char* classLabel(POP_AttributeClass klass)
{
	switch (klass)
	{
		case POP_AttributeClass::Point: return "point";
		case POP_AttributeClass::Vertex: return "vertex";
		case POP_AttributeClass::Primitive: return "prim";
		default: return "unknown";
	}
}

std::string interpolationFor(POP_AttributeClass klass)
{
	switch (klass)
	{
		case POP_AttributeClass::Point: return "vertex";
		case POP_AttributeClass::Vertex: return "faceVarying";
		case POP_AttributeClass::Primitive: return "uniform";
		default: return "";
	}
}

std::string scalarUsdType(uint32_t size)
{
	switch (size)
	{
		case 1: return "float";
		case 2: return "float2";
		case 3: return "float3";
		case 4: return "float4";
		default: return "";
	}
}

std::string maybeHalf(const std::string& usdType, bool eligible, int halfMode)
{
	if (halfMode == 0 || (halfMode == 1 && !eligible))
		return usdType;
	if (usdType == "point3f") return "point3h";
	if (usdType == "normal3f") return "normal3h";
	if (usdType == "texCoord2f") return "texCoord2h";
	if (usdType == "vector3f") return "vector3h";
	if (usdType == "color3f") return "color3h";
	if (usdType == "float") return "half";
	if (usdType == "float2") return "half2";
	if (usdType == "float3") return "half3";
	if (usdType == "float4") return "half4";
	return usdType;
}

int halfModeFromString(const char* value)
{
	std::string mode = value ? value : "off";
	if (mode == "safe") return 1;
	if (mode == "all") return 2;
	return 0;
}

enum class GeometryKind
{
	Points,
	Mesh,
	Curves,
};

const char* geometryKindName(GeometryKind kind)
{
	switch (kind)
	{
		case GeometryKind::Mesh: return "mesh";
		case GeometryKind::Curves: return "curves";
		default: return "points";
	}
}

constexpr uint32_t POPRestartIndex = 0xFFFFFFFFu;
}

class TDPopUsdWriter : public POP_CPlusPlusBase
{
public:
	TDPopUsdWriter(const OP_NodeInfo*, POP_Context*) {}
	~TDPopUsdWriter() override = default;

	void getGeneralInfo(POP_GeneralInfo* ginfo, const OP_Inputs*, void*) override
	{
		ginfo->cookEveryFrame = false;
		ginfo->cookEveryFrameIfAsked = false;
	}

	void setupParameters(OP_ParameterManager* manager, void*) override
	{
		{
			OP_StringParameter sp;
			sp.name = "Command";
			sp.label = "Command";
			sp.defaultValue = "idle";
			const char* names[] = {"idle", "reset", "append", "finish"};
			const char* labels[] = {"Idle", "Reset", "Append", "Finish"};
			manager->appendMenu(sp, 4, names, labels);
		}
		{
			OP_StringParameter sp;
			sp.name = "Tmpdir";
			sp.label = "Temp Folder";
			manager->appendFolder(sp);
		}
		{
			OP_StringParameter sp;
			sp.name = "Statuspath";
			sp.label = "Status Path";
			manager->appendFile(sp);
		}
		{
			OP_NumericParameter np;
			np.name = "Sequence";
			np.label = "Sequence";
			manager->appendInt(np);
		}
		{
			OP_NumericParameter np;
			np.name = "Frame";
			np.label = "Frame";
			manager->appendInt(np);
		}
		{
			OP_NumericParameter np;
			np.name = "Topovaries";
			np.label = "Topology Changes";
			manager->appendToggle(np);
		}
		{
			OP_StringParameter sp;
			sp.name = "Halfmode";
			sp.label = "Half Mode";
			sp.defaultValue = "off";
			const char* names[] = {"off", "safe", "all"};
			const char* labels[] = {"Off", "Safe", "All"};
			manager->appendMenu(sp, 3, names, labels);
		}
		{
			OP_NumericParameter np;
			np.name = "Fps";
			np.label = "FPS";
			np.defaultValues[0] = 60.0;
			manager->appendFloat(np);
		}
		{
			OP_NumericParameter np;
			np.name = "Starttime";
			np.label = "Start Time";
			manager->appendInt(np);
		}
		{
			OP_NumericParameter np;
			np.name = "Endtime";
			np.label = "End Time";
			manager->appendInt(np);
		}
	}

	void execute(POP_Output*, const OP_Inputs* inputs, void*) override
	{
		++myCookCount;
		myError.clear();
		myLastMs = 0.0;
		const auto start = std::chrono::high_resolution_clock::now();
		try
		{
			const std::string command = inputs->getParString("Command");
			const int32_t seq = inputs->getParInt("Sequence");
			updatePaths(inputs);

			if (command == "idle" || seq == myLastSequence)
			{
				myLastMs = msSince(start);
				writeStatus("idle");
				return;
			}
			myLastSequence = seq;

			if (command == "reset")
			{
				resetState(inputs);
				myLastMs = msSince(start);
				writeStatus("reset");
				return;
			}
			if (command == "finish")
			{
				finish();
				myLastMs = msSince(start);
				writeStatus("finished");
				return;
			}
			if (command != "append")
				throw std::runtime_error("Unsupported native command: " + command);

			const OP_POPInput* input = nullptr;
			if (inputs->getNumInputs() > 0)
				input = inputs->getInputPOP(0);
			if (!input)
				throw std::runtime_error("Native writer has no POP input");

			append(inputs, input);
			myLastMs = msSince(start);
			writeStatus("appended");
		}
		catch (const std::exception& e)
		{
			myError = e.what();
			myLastMs = msSince(start);
			writeStatus("error");
		}
	}

	int32_t getNumInfoCHOPChans(void*) override { return 8; }

	void getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void*) override
	{
		static const char* names[] = {
			"ok", "last_ms", "frames", "sections",
			"points", "vertices", "prims", "cook_count",
		};
		float values[] = {
			myError.empty() ? 1.0f : 0.0f,
			static_cast<float>(myLastMs),
			static_cast<float>(myFramesWritten),
			static_cast<float>(mySections.size()),
			static_cast<float>(myLastPoints),
			static_cast<float>(myLastVertices),
			static_cast<float>(myLastPrims),
			static_cast<float>(myCookCount),
		};
		chan->name->setString(names[index]);
		chan->value = values[index];
	}

private:
	struct Sample
	{
		int32_t time = 0;
		uint64_t offset = 0;
		uint64_t count = 0;
	};

	struct Section
	{
		std::string name;
		std::string target;
		std::string primvarName;
		std::string usdType;
		std::string interpolation;
		uint32_t tupleSize = 1;
		std::string storage = storageFloat();
		std::string path;
		POP_AttributeClass attrClass = POP_AttributeClass::Point;
		std::string attrName;
		std::vector<uint32_t> components;
		std::vector<Sample> samples;
	};

	struct Attr
	{
		POP_AttributeClass klass = POP_AttributeClass::Point;
		std::string name;
		uint32_t numComponents = 0;
		POP_AttributeType type = POP_AttributeType::Float;
		uint64_t elementCount = 0;
		OP_SmartRef<POP_Buffer> buffer;
	};

	struct Snapshot
	{
		uint32_t points = 0;
		uint32_t vertices = 0;
		uint32_t prims = 0;
		GeometryKind kind = GeometryKind::Points;
		bool isMesh = false;
		std::vector<int32_t> faceCounts;
		std::vector<int32_t> faceIndices;
		std::vector<uint32_t> vertexSourceIndices;
		std::vector<uint32_t> primSourceIndices;
		std::vector<int32_t> curveCounts;
		std::vector<uint32_t> curvePointSourceIndices;
		std::vector<Attr> attrs;
	};

	void updatePaths(const OP_Inputs* inputs)
	{
		const char* tmp = inputs->getParFilePath("Tmpdir");
		if (tmp && *tmp)
			myTmpDir = std::filesystem::path(tmp);
		const char* status = inputs->getParFilePath("Statuspath");
		if (status && *status)
			myStatusPath = std::filesystem::path(status);
	}

	void resetState(const OP_Inputs* inputs)
	{
		mySections.clear();
		myPendingTimes.clear();
		myFramesWritten = 0;
		myInitialized = false;
		myKind = GeometryKind::Points;
		myIsMesh = false;
		myFirstTopo.clear();
		myError.clear();
		myLastSequence = inputs->getParInt("Sequence");
		myFps = inputs->getParDouble("Fps");
		myStartTime = inputs->getParInt("Starttime");
		myEndTime = inputs->getParInt("Endtime");
		myTopoVaries = inputs->getParInt("Topovaries") != 0;
		myHalfMode = halfModeFromString(inputs->getParString("Halfmode"));
		if (myTmpDir.empty())
			throw std::runtime_error("Native writer Tmpdir is empty");
		std::filesystem::create_directories(myTmpDir);
		myManifestPath = myTmpDir / "manifest.json";
	}

	void append(const OP_Inputs* inputs, const OP_POPInput* input)
	{
		if (myTmpDir.empty())
			resetState(inputs);
		myTopoVaries = inputs->getParInt("Topovaries") != 0;
		myHalfMode = halfModeFromString(inputs->getParString("Halfmode"));
		const int32_t frame = inputs->getParInt("Frame");
		Snapshot snap = snapshot(input);
		myLastPoints = snap.points;
		myLastVertices = snap.vertices;
		myLastPrims = snap.prims;

		if (isEmpty(snap) && !myInitialized)
		{
			myPendingTimes.push_back(frame);
			++myFramesWritten;
			return;
		}
		if (!myInitialized)
		{
			initSections(snap);
			for (int32_t pending : myPendingTimes)
				appendEmptyFrame(pending);
			myPendingTimes.clear();
		}
		else if (!isEmpty(snap) && schemaSignature(snap) != mySchema)
		{
			throw std::runtime_error(
				"Geometry kind or attribute set/sizes changed in native writer");
		}

		if (!myTopoVaries)
			checkTopology(snap);
		appendFrame(frame, snap, isEmpty(snap));
		++myFramesWritten;
	}

	void finish()
	{
		if (!myInitialized)
		{
			Snapshot empty;
			empty.kind = GeometryKind::Points;
			empty.isMesh = false;
			initSections(empty);
			for (int32_t pending : myPendingTimes)
				appendEmptyFrame(pending);
			myPendingTimes.clear();
		}
		writeManifest();
	}

	Snapshot snapshot(const OP_POPInput* input)
	{
		Snapshot snap;
		POP_GetBufferInfo getInfo;
		getInfo.location = POP_BufferLocation::CPU;

		POP_InfoBuffers infoBuffers;
		input->getAllInfoBuffers(&infoBuffers, getInfo, nullptr);
		if (infoBuffers.pointInfo)
		{
			const POP_PointInfo* info = static_cast<const POP_PointInfo*>(
				infoBuffers.pointInfo->getData(nullptr));
			if (info)
				snap.points = info->numPoints;
		}
		if (infoBuffers.topoInfo)
		{
			const POP_TopologyInfo* topo = static_cast<const POP_TopologyInfo*>(
				infoBuffers.topoInfo->getData(nullptr));
			const POP_IndexBuffer* indexInfo = input->getIndexBuffer(nullptr);
			OP_SmartRef<POP_Buffer> indexBuf;
			if (topo && indexInfo)
				indexBuf = indexInfo->getBuffer(getInfo, nullptr);
			const uint32_t* indices = indexBuf
				? static_cast<const uint32_t*>(indexBuf->getData(nullptr))
				: nullptr;
			if (topo)
			{
				const bool hasMesh = topo->trianglesCount || topo->quadsCount;
				const bool hasCurves = topo->lineStripsCount || topo->linesCount;
				const bool hasPointPrims = topo->pointPrimitivesCount > 0;
				if (hasMesh && (hasCurves || hasPointPrims))
					throw std::runtime_error(
						"Native POP writer does not yet support mixed mesh, curve, or point-primitive topology. Split the POP before export.");
				if (hasCurves && hasPointPrims)
					throw std::runtime_error(
						"Native POP writer does not yet support mixed curve and point-primitive topology. Split the POP before export.");
				if ((hasMesh || hasCurves) && !indices)
					throw std::runtime_error(
						"Native POP writer could not read the POP index buffer");

				if (hasMesh)
				{
					appendFaces(snap, indices, topo->trianglesStartIndex,
						topo->trianglesCount, 3, 0);
					appendFaces(snap, indices, topo->quadsStartIndex,
						topo->quadsCount, 4, topo->trianglesCount);
					snap.kind = GeometryKind::Mesh;
					snap.prims = static_cast<uint32_t>(snap.faceCounts.size());
					snap.vertices = static_cast<uint32_t>(snap.faceIndices.size());
				}
				else if (hasCurves)
				{
					if (topo->lineStripsCount)
					{
						if (!infoBuffers.lineStripsInfo)
							throw std::runtime_error(
								"Native POP writer could not read line strip metadata");
						const uint32_t* strips = static_cast<const uint32_t*>(
							infoBuffers.lineStripsInfo->getData(nullptr));
						if (!strips)
							throw std::runtime_error(
								"Native POP writer line strip metadata has no CPU data");
						appendLineStrips(snap, indices, strips,
							topo->lineStripsStartIndex, topo->lineStripsCount);
					}
					if (topo->linesCount)
						appendLines(snap, indices, topo->linesStartIndex,
							topo->linesCount);
					if (snap.curveCounts.empty())
						throw std::runtime_error(
							"Native POP writer found curve topology but no valid curves");
					snap.kind = GeometryKind::Curves;
					snap.prims = static_cast<uint32_t>(snap.curveCounts.size());
					snap.vertices = static_cast<uint32_t>(
						snap.curvePointSourceIndices.size());
				}
				else
				{
					snap.kind = GeometryKind::Points;
					snap.prims = topo->pointPrimitivesCount;
					snap.vertices = topo->pointPrimitivesCount;
				}
				snap.isMesh = snap.kind == GeometryKind::Mesh;
			}
		}

		collectAttrs(input, POP_AttributeClass::Point, getInfo, snap);
		collectAttrs(input, POP_AttributeClass::Vertex, getInfo, snap);
		collectAttrs(input, POP_AttributeClass::Primitive, getInfo, snap);
		return snap;
	}

	void appendFaces(Snapshot& snap, const uint32_t* indices,
		uint32_t start, uint32_t count, uint32_t size, uint32_t primBase)
	{
		for (uint32_t p = 0; p < count; ++p)
		{
			std::vector<int32_t> face;
			std::vector<uint32_t> sourceVertices;
			face.reserve(size);
			sourceVertices.reserve(size);
			for (uint32_t i = 0; i < size; ++i)
			{
				const uint32_t sourceIndex = start + p * size + i;
				const int32_t pointIndex = static_cast<int32_t>(indices[sourceIndex]);
				if (!face.empty() && face.back() == pointIndex)
					continue;
				face.push_back(pointIndex);
				sourceVertices.push_back(sourceIndex);
			}
			if (face.size() > 2 && face.front() == face.back())
			{
				face.pop_back();
				sourceVertices.pop_back();
			}
			if (face.size() < 3)
				continue;
			snap.faceCounts.push_back(static_cast<int32_t>(face.size()));
			snap.primSourceIndices.push_back(primBase + p);
			snap.faceIndices.insert(snap.faceIndices.end(),
				face.begin(), face.end());
			snap.vertexSourceIndices.insert(snap.vertexSourceIndices.end(),
				sourceVertices.begin(), sourceVertices.end());
		}
	}

	void appendLineStrips(Snapshot& snap, const uint32_t* indices,
		const uint32_t* strips, uint32_t stripsStart, uint32_t stripCount)
	{
		for (uint32_t strip = 0; strip < stripCount; ++strip)
		{
			const uint32_t relativeStart = strips[strip * 2];
			const uint32_t countWithRestart = strips[strip * 2 + 1];
			const size_t countBefore = snap.curvePointSourceIndices.size();
			for (uint32_t i = 0; i < countWithRestart; ++i)
			{
				const uint32_t pointIndex =
					indices[stripsStart + relativeStart + i];
				if (pointIndex == POPRestartIndex)
					continue;
				appendCurvePoint(snap, pointIndex);
			}
			const size_t count = snap.curvePointSourceIndices.size() - countBefore;
			if (count < 2)
			{
				snap.curvePointSourceIndices.resize(countBefore);
				continue;
			}
			snap.curveCounts.push_back(static_cast<int32_t>(count));
		}
	}

	void appendLines(Snapshot& snap, const uint32_t* indices,
		uint32_t start, uint32_t count)
	{
		for (uint32_t line = 0; line < count; ++line)
		{
			const uint32_t a = indices[start + line * 2];
			const uint32_t b = indices[start + line * 2 + 1];
			if (a == POPRestartIndex || b == POPRestartIndex)
				continue;
			appendCurvePoint(snap, a);
			appendCurvePoint(snap, b);
			snap.curveCounts.push_back(2);
		}
	}

	void appendCurvePoint(Snapshot& snap, uint32_t pointIndex)
	{
		if (pointIndex >= snap.points)
			throw std::runtime_error(
				"Native POP curve index is out of range");
		snap.curvePointSourceIndices.push_back(pointIndex);
	}

	void collectAttrs(const OP_POPInput* input, POP_AttributeClass klass,
		const POP_GetBufferInfo& getInfo, Snapshot& snap)
	{
		const uint32_t count = input->getNumAttributes(klass);
		for (uint32_t i = 0; i < count; ++i)
		{
			const POP_Attribute* src = input->getAttribute(klass, i, nullptr);
			if (!src)
				continue;
			OP_SmartRef<POP_Buffer> buf = src->getBuffer(getInfo, nullptr);
			if (!buf)
				continue;
			const POP_AttributeInfo& info = src->info;
			const uint32_t compBytes = componentBytes(info.type);
			const uint32_t tupleComps = info.numComponents * info.numColumns;
			if (compBytes == 0 || tupleComps == 0)
				continue;
			Attr attr;
			attr.klass = klass;
			attr.name = info.name ? info.name : "";
			attr.numComponents = tupleComps;
			attr.type = info.type;
			attr.buffer = buf;
			attr.elementCount = buf->info.size / (uint64_t(compBytes) * tupleComps);
			snap.attrs.push_back(attr);
		}
	}

	bool isEmpty(const Snapshot& snap) const
	{
		return snap.points == 0 && snap.prims == 0 && snap.vertices == 0;
	}

	void initSections(const Snapshot& snap)
	{
		mySections.clear();
		myKind = snap.kind;
		myIsMesh = myKind == GeometryKind::Mesh;
		if (myIsMesh)
		{
			addSection("faceVertexCounts", "faceVertexCounts", "",
				"int", "", 1, storageInt(), "faceVertexCounts", {});
			addSection("faceVertexIndices", "faceVertexIndices", "",
				"int", "", 1, storageInt(), "faceVertexIndices", {});
		}
		if (myKind == GeometryKind::Curves)
		{
			addSection("curveVertexCounts", "curveVertexCounts", "",
				"int", "", 1, storageInt(), "curveVertexCounts", {});
		}
		addSection("extent", "extent", "", "float3", "", 3,
			storageFloat(), "extent", {});
		addSection("points", "points", "", maybeHalf("point3f", false,
			myHalfMode), "", 3, storageFloat(), "P", {0, 1, 2});
		if (!myIsMesh)
			addSection("widths", "widths", "", maybeHalf("float", false,
				myHalfMode), "constant", 1, storageFloat(), "widths", {});

		for (const Attr& attr : snap.attrs)
		{
			if (myKind == GeometryKind::Curves &&
				attr.klass != POP_AttributeClass::Point)
			{
				throw std::runtime_error(
					"Native POP curve export currently supports point attributes only. Unsupported attribute: " +
					attr.name);
			}
			Section section;
			if (!resolveAttr(attr, section))
				continue;
			mySections.push_back(section);
		}
		mySchema = schemaSignature(snap);
		myInitialized = true;
		myFirstTopo = topologyBytes(snap);
	}

	void addSection(const std::string& name, const std::string& target,
		const std::string& primvarName, const std::string& usdType,
		const std::string& interp, uint32_t tupleSize,
		const std::string& storage, const std::string& attrName,
		const std::vector<uint32_t>& components)
	{
		Section section;
		section.name = name;
		section.target = target;
		section.primvarName = primvarName;
		section.usdType = usdType;
		section.interpolation = interp;
		section.tupleSize = tupleSize;
		section.storage = storage;
		section.attrName = attrName;
		section.components = components;
		section.path = normPath(myTmpDir / ("native_" +
			std::to_string(mySections.size()) + ".bin"));
		mySections.push_back(section);
	}

	bool resolveAttr(const Attr& attr, Section& out)
	{
		if (attr.klass == POP_AttributeClass::Point && attr.name == "P")
			return false;
		if (attr.type != POP_AttributeType::Float)
			return false;
		if (attr.numComponents < 1 || attr.numComponents > 4)
			return false;

		out.attrClass = attr.klass;
		out.attrName = attr.name;
		out.interpolation = interpolationFor(attr.klass);
		out.storage = storageFloat();
		out.path = normPath(myTmpDir / ("native_" +
			std::to_string(mySections.size()) + ".bin"));

		if (attr.name == "N" && attr.numComponents >= 3)
		{
			out.name = "normals";
			out.target = "normals";
			out.usdType = maybeHalf("normal3f", true, myHalfMode);
			out.tupleSize = 3;
			out.components = {0, 1, 2};
			return true;
		}
		if (attr.klass == POP_AttributeClass::Point &&
			(attr.name == "v" || attr.name == "PartVel") &&
			attr.numComponents >= 3)
		{
			out.name = "velocities";
			out.target = "velocities";
			out.usdType = maybeHalf("vector3f", true, myHalfMode);
			out.interpolation.clear();
			out.tupleSize = 3;
			out.components = {0, 1, 2};
			return true;
		}
		if (attr.klass == POP_AttributeClass::Vertex &&
			(attr.name == "Tex" || attr.name == "uv") &&
			attr.numComponents >= 2)
		{
			out.name = "primvars:st";
			out.target = "primvar";
			out.primvarName = "st";
			out.usdType = maybeHalf("texCoord2f", true, myHalfMode);
			out.interpolation = "faceVarying";
			out.tupleSize = 2;
			out.components = {0, 1};
			return true;
		}
		if ((attr.name == "Color" || attr.name == "Cd") &&
			attr.numComponents >= 3)
		{
			out.name = "primvars:Cd";
			out.target = "primvar";
			out.primvarName = "Cd";
			out.usdType = maybeHalf("color3f", true, myHalfMode);
			out.tupleSize = 3;
			out.components = {0, 1, 2};
			return true;
		}

		const std::string type = scalarUsdType(attr.numComponents);
		if (type.empty())
			return false;
		out.name = "primvars:" + attr.name;
		out.target = "primvar";
		out.primvarName = attr.name;
		out.usdType = maybeHalf(type, false, myHalfMode);
		out.tupleSize = attr.numComponents;
		out.components.clear();
		for (uint32_t i = 0; i < attr.numComponents; ++i)
			out.components.push_back(i);
		return true;
	}

	std::string schemaSignature(const Snapshot& snap) const
	{
		std::ostringstream s;
		s << geometryKindName(snap.kind);
		for (const Attr& attr : snap.attrs)
		{
			if (snap.kind == GeometryKind::Curves &&
				attr.klass != POP_AttributeClass::Point)
			{
				throw std::runtime_error(
					"Native POP curve export currently supports point attributes only. Unsupported attribute: " +
					attr.name);
			}
			Section section;
			if (!const_cast<TDPopUsdWriter*>(this)->resolveAttr(attr, section))
				continue;
			s << "|" << classLabel(attr.klass) << ":" << attr.name
				<< ":" << attr.numComponents << ":" << section.name
				<< ":" << section.usdType << ":" << section.interpolation;
		}
		return s.str();
	}

	std::vector<char> topologyBytes(const Snapshot& snap) const
	{
		std::vector<char> out;
		if (snap.kind == GeometryKind::Mesh)
		{
			appendBytes(out, snap.faceCounts);
			appendBytes(out, snap.faceIndices);
		}
		else if (snap.kind == GeometryKind::Curves)
		{
			appendBytes(out, snap.curveCounts);
			appendBytes(out, snap.curvePointSourceIndices);
		}
		return out;
	}

	template<typename T>
	void appendBytes(std::vector<char>& out, const std::vector<T>& data) const
	{
		const char* ptr = reinterpret_cast<const char*>(data.data());
		out.insert(out.end(), ptr, ptr + data.size() * sizeof(T));
	}

	void checkTopology(const Snapshot& snap)
	{
		if (myKind == GeometryKind::Points)
			return;
		if (topologyBytes(snap) != myFirstTopo)
			throw std::runtime_error(
				"Topology changed but Topology Changes is off");
	}

	void appendEmptyFrame(int32_t frame)
	{
		Snapshot empty;
		empty.kind = myKind;
		empty.isMesh = myIsMesh;
		appendFrame(frame, empty, true);
	}

	void appendFrame(int32_t frame, const Snapshot& snap, bool empty)
	{
		for (Section& section : mySections)
		{
			if (section.target == "faceVertexCounts")
				writeVector(section, frame, snap.faceCounts);
			else if (section.target == "faceVertexIndices")
				writeVector(section, frame, snap.faceIndices);
			else if (section.target == "curveVertexCounts")
				writeVector(section, frame, snap.curveCounts);
			else if (section.target == "extent")
				writeExtent(section, frame, snap, empty);
			else if (section.target == "points")
				writeAttribute(section, frame, snap, POP_AttributeClass::Point, "P", empty);
			else if (section.target == "widths")
				writeWidth(section, frame);
			else
				writeAttribute(section, frame, snap, section.attrClass,
					section.attrName, empty);
		}
	}

	void writeWidth(Section& section, int32_t frame)
	{
		const float value = 0.05f;
		writeRaw(section, frame, &value, sizeof(value), 1);
	}

	void writeExtent(Section& section, int32_t frame, const Snapshot& snap,
		bool empty)
	{
		float extent[6] = {0, 0, 0, 0, 0, 0};
		if (!empty)
		{
			const Attr* p = findAttr(snap, POP_AttributeClass::Point, "P");
			if (p && p->buffer && p->numComponents >= 3)
			{
				const float* data = static_cast<const float*>(
					p->buffer->getData(nullptr));
				if (data && p->elementCount > 0)
				{
					float minv[3] = {
						std::numeric_limits<float>::max(),
						std::numeric_limits<float>::max(),
						std::numeric_limits<float>::max()};
					float maxv[3] = {
						-std::numeric_limits<float>::max(),
						-std::numeric_limits<float>::max(),
						-std::numeric_limits<float>::max()};
					if (snap.kind == GeometryKind::Curves)
					{
						for (uint32_t sourceIndex : snap.curvePointSourceIndices)
						{
							if (sourceIndex >= p->elementCount)
								throw std::runtime_error(
									"POP curve point index is out of range for P");
							const float* src = data +
								uint64_t(sourceIndex) * p->numComponents;
							for (uint32_t c = 0; c < 3; ++c)
							{
								minv[c] = std::min(minv[c], src[c]);
								maxv[c] = std::max(maxv[c], src[c]);
							}
						}
					}
					else
					{
						for (uint64_t i = 0; i < p->elementCount; ++i)
						{
							const float* src = data + i * p->numComponents;
							for (uint32_t c = 0; c < 3; ++c)
							{
								minv[c] = std::min(minv[c], src[c]);
								maxv[c] = std::max(maxv[c], src[c]);
							}
						}
					}
					for (uint32_t c = 0; c < 3; ++c)
					{
						extent[c] = minv[c];
						extent[c + 3] = maxv[c];
					}
				}
			}
		}
		writeRaw(section, frame, extent, sizeof(extent), 2);
	}

	template<typename T>
	void writeVector(Section& section, int32_t frame, const std::vector<T>& data)
	{
		writeRaw(section, frame, data.data(), data.size() * sizeof(T),
			data.size());
	}

	void writeAttribute(Section& section, int32_t frame, const Snapshot& snap,
		POP_AttributeClass klass, const std::string& name, bool empty)
	{
		if (empty)
		{
			writeRaw(section, frame, nullptr, 0, 0);
			return;
		}
		const Attr* attr = findAttr(snap, klass, name);
		if (!attr || !attr->buffer)
			throw std::runtime_error("Missing POP attribute " + name);
		const float* data = static_cast<const float*>(
			attr->buffer->getData(nullptr));
		if (!data)
			throw std::runtime_error("POP attribute has no CPU data: " + name);
		if (klass == POP_AttributeClass::Vertex)
		{
			writeRemappedAttribute(section, frame, *attr, data,
				snap.vertexSourceIndices);
			return;
		}
		if (snap.kind == GeometryKind::Curves &&
			klass == POP_AttributeClass::Point)
		{
			writeRemappedAttribute(section, frame, *attr, data,
				snap.curvePointSourceIndices);
			return;
		}
		if (klass == POP_AttributeClass::Primitive &&
			snap.primSourceIndices.size() != attr->elementCount)
		{
			writeRemappedAttribute(section, frame, *attr, data,
				snap.primSourceIndices);
			return;
		}
		if (section.components.size() == attr->numComponents)
		{
			bool contiguous = true;
			for (uint32_t i = 0; i < section.components.size(); ++i)
				contiguous = contiguous && section.components[i] == i;
			if (contiguous)
			{
				writeRaw(section, frame, data,
					size_t(attr->elementCount) * attr->numComponents *
						sizeof(float),
					attr->elementCount);
				return;
			}
		}
		std::vector<float> tmp;
		tmp.reserve(size_t(attr->elementCount) * section.tupleSize);
		for (uint64_t i = 0; i < attr->elementCount; ++i)
		{
			const float* src = data + i * attr->numComponents;
			for (uint32_t comp : section.components)
				tmp.push_back(src[comp]);
		}
		writeRaw(section, frame, tmp.data(), tmp.size() * sizeof(float),
			attr->elementCount);
	}

	void writeRemappedAttribute(Section& section, int32_t frame,
		const Attr& attr, const float* data, const std::vector<uint32_t>& indices)
	{
		std::vector<float> tmp;
		tmp.reserve(size_t(indices.size()) * section.tupleSize);
		for (uint32_t sourceIndex : indices)
		{
			if (sourceIndex >= attr.elementCount)
				throw std::runtime_error(
					"POP attribute source index is out of range: " + attr.name);
			const float* src = data + uint64_t(sourceIndex) * attr.numComponents;
			for (uint32_t comp : section.components)
				tmp.push_back(src[comp]);
		}
		writeRaw(section, frame, tmp.data(), tmp.size() * sizeof(float),
			indices.size());
	}

	const Attr* findAttr(const Snapshot& snap, POP_AttributeClass klass,
		const std::string& name) const
	{
		for (const Attr& attr : snap.attrs)
			if (attr.klass == klass && attr.name == name)
				return &attr;
		return nullptr;
	}

	void writeRaw(Section& section, int32_t frame, const void* data,
		size_t bytes, uint64_t count)
	{
		std::filesystem::create_directories(std::filesystem::path(section.path).parent_path());
		std::ofstream out(section.path,
			std::ios::binary | std::ios::app);
		out.seekp(0, std::ios::end);
		const uint64_t offset = static_cast<uint64_t>(out.tellp());
		if (data && bytes > 0)
			out.write(static_cast<const char*>(data),
				static_cast<std::streamsize>(bytes));
		Sample sample;
		sample.time = frame;
		sample.offset = offset;
		sample.count = count;
		section.samples.push_back(sample);
	}

	void writeManifest()
	{
		if (myManifestPath.empty())
			myManifestPath = myTmpDir / "manifest.json";
		std::ofstream out(myManifestPath, std::ios::out | std::ios::trunc);
		out << "{\n";
		out << "  \"version\": 1,\n";
		out << "  \"stage\": {\n";
		out << "    \"geometryKind\": \"" << geometryKindName(myKind) << "\",\n";
		out << "    \"isMesh\": " << (myIsMesh ? "true" : "false") << ",\n";
		out << "    \"framesPerSecond\": " << myFps << ",\n";
		out << "    \"timeCodesPerSecond\": " << myFps << ",\n";
		out << "    \"startTimeCode\": " << myStartTime << ",\n";
		out << "    \"endTimeCode\": " << myEndTime << ",\n";
		out << "    \"metersPerUnit\": 1,\n";
		out << "    \"upAxis\": \"Y\"\n";
		out << "  },\n";
		out << "  \"sections\": [\n";
		for (size_t i = 0; i < mySections.size(); ++i)
		{
			const Section& s = mySections[i];
			out << "    {\n";
			out << "      \"kind\": \"sampler\",\n";
			out << "      \"name\": \"" << jsonEscape(s.name) << "\",\n";
			out << "      \"target\": \"" << jsonEscape(s.target) << "\",\n";
			out << "      \"primvarName\": \"" << jsonEscape(s.primvarName) << "\",\n";
			out << "      \"usdType\": \"" << jsonEscape(s.usdType) << "\",\n";
			if (s.interpolation.empty())
				out << "      \"interpolation\": null,\n";
			else
				out << "      \"interpolation\": \"" << jsonEscape(s.interpolation) << "\",\n";
			out << "      \"tupleSize\": " << s.tupleSize << ",\n";
			out << "      \"storage\": \"" << jsonEscape(s.storage) << "\",\n";
			out << "      \"path\": \"" << jsonEscape(s.path) << "\",\n";
			out << "      \"samples\": [\n";
			for (size_t j = 0; j < s.samples.size(); ++j)
			{
				const Sample& sample = s.samples[j];
				out << "        {\"offset\": " << sample.offset
					<< ", \"count\": " << sample.count
					<< ", \"time\": " << sample.time << "}";
				out << (j + 1 < s.samples.size() ? "," : "") << "\n";
			}
			out << "      ]\n";
			out << "    }" << (i + 1 < mySections.size() ? "," : "") << "\n";
		}
		out << "  ]\n";
		out << "}\n";
	}

	void writeStatus(const std::string& state)
	{
		if (myStatusPath.empty())
			return;
		std::filesystem::create_directories(myStatusPath.parent_path());
		std::ofstream out(myStatusPath, std::ios::out | std::ios::trunc);
		out << "{\n";
		out << "  \"ok\": " << (myError.empty() ? "true" : "false") << ",\n";
		out << "  \"state\": \"" << jsonEscape(state) << "\",\n";
		out << "  \"error\": \"" << jsonEscape(myError) << "\",\n";
		out << "  \"manifest\": \"" << jsonEscape(normPath(myManifestPath)) << "\",\n";
		out << "  \"frames\": " << myFramesWritten << ",\n";
		out << "  \"sections\": " << mySections.size() << ",\n";
		out << "  \"geometryKind\": \"" << geometryKindName(myKind) << "\",\n";
		out << "  \"isMesh\": " << (myIsMesh ? "true" : "false") << ",\n";
		out << "  \"lastMs\": " << myLastMs << "\n";
		out << "}\n";
	}

	std::vector<Section> mySections;
	std::vector<int32_t> myPendingTimes;
	std::vector<char> myFirstTopo;
	std::filesystem::path myTmpDir;
	std::filesystem::path myManifestPath;
	std::filesystem::path myStatusPath;
	std::string mySchema;
	std::string myError;
	double myLastMs = 0.0;
	double myFps = 60.0;
	int32_t myStartTime = 0;
	int32_t myEndTime = 0;
	int32_t myLastSequence = -1;
	int32_t myHalfMode = 0;
	uint32_t myFramesWritten = 0;
	uint32_t myLastPoints = 0;
	uint32_t myLastVertices = 0;
	uint32_t myLastPrims = 0;
	int32_t myCookCount = 0;
	bool myInitialized = false;
	GeometryKind myKind = GeometryKind::Points;
	bool myIsMesh = false;
	bool myTopoVaries = false;
};

extern "C"
{
DLLEXPORT void FillPOPPluginInfo(POP_PluginInfo* info)
{
	if (!info->setAPIVersion(POPCPlusPlusAPIVersion))
		return;
	info->customOPInfo.opType->setString("Tdpopusdwriter");
	info->customOPInfo.opLabel->setString("TD POP USD Writer");
	info->customOPInfo.opIcon->setString("TUP");
	info->customOPInfo.authorName->setString("TD-SOP-USD-Anim-Bridge");
	info->customOPInfo.authorEmail->setString("");
	info->customOPInfo.minInputs = 1;
	info->customOPInfo.maxInputs = 1;
}

DLLEXPORT POP_CPlusPlusBase* CreatePOPInstance(
	const OP_NodeInfo* info, POP_Context* context)
{
	return new TDPopUsdWriter(info, context);
}

DLLEXPORT void DestroyPOPInstance(POP_CPlusPlusBase* instance)
{
	delete static_cast<TDPopUsdWriter*>(instance);
}
}
