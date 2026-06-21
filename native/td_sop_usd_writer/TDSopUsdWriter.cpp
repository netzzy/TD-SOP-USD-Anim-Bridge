// Native SOP chunk writer for TD-SOP-USD-Anim-Bridge.
//
// This CPlusPlus SOP reads a SOP input and writes the binary chunk manifest used
// by tools/build_usdc_from_chunks.py. It is intentionally narrower than the
// Python SOP backend: the public TD C++ SOP input API exposes generic custom
// attributes as point arrays, plus dedicated standard vertex/primitive Cd/uv
// accessors. Python preflight rejects unsupported SOP attributes before this
// writer runs so data is not silently dropped.

#include "SOP_CPlusPlusBase.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

enum class NativeAttrClass
{
	Point,
	Vertex,
	Primitive,
};

enum class NativeAttrType
{
	Float,
	Int32,
	Position,
	Vector,
	Color,
	TexCoord,
};

NativeAttrClass classFromAttribSet(AttribSet set)
{
	if (set == AttribSet::Vertex)
		return NativeAttrClass::Vertex;
	if (set == AttribSet::Primitive)
		return NativeAttrClass::Primitive;
	return NativeAttrClass::Point;
}

std::string classLabel(NativeAttrClass klass)
{
	switch (klass)
	{
		case NativeAttrClass::Point: return "point";
		case NativeAttrClass::Vertex: return "vertex";
		case NativeAttrClass::Primitive: return "prim";
		default: return "unknown";
	}
}

std::string interpolationFor(NativeAttrClass klass)
{
	switch (klass)
	{
		case NativeAttrClass::Point: return "vertex";
		case NativeAttrClass::Vertex: return "faceVarying";
		case NativeAttrClass::Primitive: return "uniform";
		default: return "";
	}
}
}

class TDSopUsdWriter : public SOP_CPlusPlusBase
{
public:
	explicit TDSopUsdWriter(const OP_NodeInfo*) {}
	~TDSopUsdWriter() override = default;

	void getGeneralInfo(SOP_GeneralInfo* ginfo, const OP_Inputs*, void*) override
	{
		ginfo->cookEveryFrame = false;
		ginfo->cookEveryFrameIfAsked = false;
		ginfo->directToGPU = false;
		ginfo->winding = SOP_Winding::CCW;
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

	void execute(SOP_Output*, const OP_Inputs* inputs, void*) override
	{
		++myCookCount;
		myError.clear();
		myLastMs = 0.0;
		const auto start = std::chrono::high_resolution_clock::now();
		try
		{
			const std::string command = controlString(inputs, "Command",
				"TD_SOP_USD_WRITER_COMMAND", "idle");
			const int32_t seq = controlInt(inputs, "Sequence",
				"TD_SOP_USD_WRITER_SEQUENCE", 0);
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

			const OP_SOPInput* input = nullptr;
			if (inputs->getNumInputs() > 0)
				input = inputs->getInputSOP(0);
			if (!input)
				throw std::runtime_error("Native SOP writer has no SOP input");

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

	void executeVBO(SOP_VBOOutput*, const OP_Inputs*, void*) override {}

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
		NativeAttrClass attrClass = NativeAttrClass::Point;
		std::string attrName;
		std::vector<uint32_t> components;
		std::vector<Sample> samples;
	};

	struct Attr
	{
		NativeAttrClass klass = NativeAttrClass::Point;
		std::string name;
		uint32_t numComponents = 0;
		NativeAttrType type = NativeAttrType::Float;
		uint64_t elementCount = 0;
		const float* floatData = nullptr;
		const int32_t* intData = nullptr;
		const Position* positions = nullptr;
		const Vector* vectors = nullptr;
		const Color* colors = nullptr;
		const TexCoord* texcoords = nullptr;
	};

	struct Snapshot
	{
		uint32_t points = 0;
		uint32_t vertices = 0;
		uint32_t prims = 0;
		bool isMesh = false;
		std::vector<int32_t> faceCounts;
		std::vector<int32_t> faceIndices;
		std::vector<Attr> attrs;
	};

	void updatePaths(const OP_Inputs* inputs)
	{
		const std::string tmp = controlString(inputs, "Tmpdir",
			"TD_SOP_USD_WRITER_TMPDIR", "");
		if (!tmp.empty())
			myTmpDir = std::filesystem::path(tmp);
		const std::string status = controlString(inputs, "Statuspath",
			"TD_SOP_USD_WRITER_STATUSPATH", "");
		if (!status.empty())
			myStatusPath = std::filesystem::path(status);
	}

	void resetState(const OP_Inputs* inputs)
	{
		mySections.clear();
		myPendingTimes.clear();
		myFramesWritten = 0;
		myInitialized = false;
		myIsMesh = false;
		myFirstTopo.clear();
		myFirstPointCount = 0;
		myError.clear();
		myLastSequence = controlInt(inputs, "Sequence",
			"TD_SOP_USD_WRITER_SEQUENCE", 0);
		myFps = controlDouble(inputs, "Fps", "TD_SOP_USD_WRITER_FPS", 60.0);
		myStartTime = controlInt(inputs, "Starttime",
			"TD_SOP_USD_WRITER_STARTTIME", 0);
		myEndTime = controlInt(inputs, "Endtime",
			"TD_SOP_USD_WRITER_ENDTIME", 0);
		myTopoVaries = controlInt(inputs, "Topovaries",
			"TD_SOP_USD_WRITER_TOPOVARIES", 0) != 0;
		const std::string halfMode = controlString(inputs, "Halfmode",
			"TD_SOP_USD_WRITER_HALFMODE", "off");
		myHalfMode = halfModeFromString(halfMode.c_str());
		if (myTmpDir.empty())
			throw std::runtime_error("Native SOP writer Tmpdir is empty");
		std::filesystem::create_directories(myTmpDir);
		myManifestPath = myTmpDir / "manifest.json";
	}

	void append(const OP_Inputs* inputs, const OP_SOPInput* input)
	{
		if (myTmpDir.empty())
			resetState(inputs);
		myTopoVaries = controlInt(inputs, "Topovaries",
			"TD_SOP_USD_WRITER_TOPOVARIES", 0) != 0;
		const std::string halfMode = controlString(inputs, "Halfmode",
			"TD_SOP_USD_WRITER_HALFMODE", "off");
		myHalfMode = halfModeFromString(halfMode.c_str());
		const int32_t frame = controlInt(inputs, "Frame",
			"TD_SOP_USD_WRITER_FRAME", 0);
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
				"Geometry kind or attribute set/sizes changed in native SOP writer");
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
			empty.isMesh = false;
			initSections(empty);
			for (int32_t pending : myPendingTimes)
				appendEmptyFrame(pending);
			myPendingTimes.clear();
		}
		writeManifest();
	}

	Snapshot snapshot(const OP_SOPInput* input)
	{
		Snapshot snap;
		snap.points = static_cast<uint32_t>(std::max(0, input->getNumPoints()));
		snap.prims = static_cast<uint32_t>(std::max(0, input->getNumPrimitives()));

		Attr p;
		p.klass = NativeAttrClass::Point;
		p.name = "P";
		p.numComponents = 3;
		p.type = NativeAttrType::Position;
		p.positions = input->getPointPositions();
		p.elementCount = snap.points;
		snap.attrs.push_back(p);

		if (snap.prims > 0)
		{
			snap.isMesh = true;
			for (uint32_t i = 0; i < snap.prims; ++i)
			{
				const SOP_PrimitiveInfo& prim = input->getPrimitive(i);
				if (prim.type != PrimitiveType::Polygon || prim.numVertices < 3)
					throw std::runtime_error(
						"Native SOP writer supports polygon mesh primitives with "
						"at least 3 vertices, or point-only SOPs");
				snap.faceCounts.push_back(prim.numVertices);
				for (int32_t j = 0; j < prim.numVertices; ++j)
				{
					const int32_t idx = prim.pointIndices[j];
					if (idx < 0 || idx >= static_cast<int32_t>(snap.points))
						throw std::runtime_error("SOP primitive contains invalid point index");
					snap.faceIndices.push_back(idx);
				}
			}
		}
		snap.vertices = static_cast<uint32_t>(snap.faceIndices.size());

		collectStandardAttrs(input, snap);
		collectCustomAttrs(input, snap);
		return snap;
	}

	std::string envString(const char* name) const
	{
		const char* value = std::getenv(name);
		return value ? std::string(value) : std::string();
	}

	std::string controlString(const OP_Inputs* inputs, const char* parName,
		const char* envName, const char* fallback) const
	{
		const std::string envValue = envString(envName);
		if (!envValue.empty())
			return envValue;
		const char* parValue = inputs->getParString(parName);
		if (parValue && *parValue)
			return parValue;
		const char* fileValue = inputs->getParFilePath(parName);
		if (fileValue && *fileValue)
			return fileValue;
		return fallback ? std::string(fallback) : std::string();
	}

	int32_t controlInt(const OP_Inputs* inputs, const char* parName,
		const char* envName, int32_t fallback) const
	{
		const std::string envValue = envString(envName);
		if (!envValue.empty())
			return static_cast<int32_t>(std::strtol(envValue.c_str(), nullptr, 10));
		return inputs ? inputs->getParInt(parName) : fallback;
	}

	double controlDouble(const OP_Inputs* inputs, const char* parName,
		const char* envName, double fallback) const
	{
		const std::string envValue = envString(envName);
		if (!envValue.empty())
			return std::strtod(envValue.c_str(), nullptr);
		return inputs ? inputs->getParDouble(parName) : fallback;
	}

	void collectStandardAttrs(const OP_SOPInput* input, Snapshot& snap)
	{
		const SOP_NormalInfo* normals = input->getNormals();
		if (normals && normals->normals && normals->numNormals > 0)
			addVectorAttr(snap, classFromAttribSet(normals->attribSet), "N",
				normals->normals, normals->numNormals);

		const SOP_ColorInfo* colors = input->getColors();
		if (colors && colors->colors && colors->numColors > 0)
			addColorAttr(snap, classFromAttribSet(colors->attribSet), "Cd",
				colors->colors, colors->numColors);

		const SOP_TextureInfo* textures = input->getTextures();
		if (textures && textures->textures && textures->numTextures > 0)
			addTexAttr(snap, classFromAttribSet(textures->attribSet), "uv",
				textures->textures, textures->numTextures);

		const SOP_ColorInfo* vtxColors = input->getVtxColors();
		if (vtxColors && vtxColors->colors && vtxColors->numColors > 0)
			addColorAttr(snap, NativeAttrClass::Vertex, "Cd",
				vtxColors->colors, vtxColors->numColors);

		const SOP_TextureInfo* vtxTextures = input->getVtxTextures();
		if (vtxTextures && vtxTextures->textures && vtxTextures->numTextures > 0)
			addTexAttr(snap, NativeAttrClass::Vertex, "uv",
				vtxTextures->textures, vtxTextures->numTextures);

		const SOP_ColorInfo* primColors = input->getPrimColors();
		if (primColors && primColors->colors && primColors->numColors > 0)
			addColorAttr(snap, NativeAttrClass::Primitive, "Cd",
				primColors->colors, primColors->numColors);
	}

	void collectCustomAttrs(const OP_SOPInput* input, Snapshot& snap)
	{
		const int32_t count = input->getNumCustomAttributes();
		for (int32_t i = 0; i < count; ++i)
		{
			const SOP_CustomAttribData* src = input->getCustomAttribute(i);
			if (!src || !src->name || src->numComponents <= 0)
				continue;
			const std::string name(src->name);
			if (name == "P" || name == "N" || name == "Cd" || name == "uv")
				continue;
			Attr attr;
			attr.klass = NativeAttrClass::Point;
			attr.name = name;
			attr.numComponents = static_cast<uint32_t>(src->numComponents);
			attr.elementCount = snap.points;
			if (src->attribType == AttribType::Float)
			{
				if (!src->floatData)
					throw std::runtime_error("SOP float custom attribute has no data: " + name);
				attr.type = NativeAttrType::Float;
				attr.floatData = src->floatData;
			}
			else
			{
				if (!src->intData)
					throw std::runtime_error("SOP int custom attribute has no data: " + name);
				attr.type = NativeAttrType::Int32;
				attr.intData = src->intData;
			}
			snap.attrs.push_back(attr);
		}
	}

	void addVectorAttr(Snapshot& snap, NativeAttrClass klass,
		const std::string& name, const Vector* data, int32_t count)
	{
		Attr attr;
		attr.klass = klass;
		attr.name = name;
		attr.numComponents = 3;
		attr.type = NativeAttrType::Vector;
		attr.vectors = data;
		attr.elementCount = static_cast<uint64_t>(std::max(0, count));
		snap.attrs.push_back(attr);
	}

	void addColorAttr(Snapshot& snap, NativeAttrClass klass,
		const std::string& name, const Color* data, int32_t count)
	{
		Attr attr;
		attr.klass = klass;
		attr.name = name;
		attr.numComponents = 4;
		attr.type = NativeAttrType::Color;
		attr.colors = data;
		attr.elementCount = static_cast<uint64_t>(std::max(0, count));
		snap.attrs.push_back(attr);
	}

	void addTexAttr(Snapshot& snap, NativeAttrClass klass,
		const std::string& name, const TexCoord* data, int32_t count)
	{
		Attr attr;
		attr.klass = klass;
		attr.name = name;
		attr.numComponents = 3;
		attr.type = NativeAttrType::TexCoord;
		attr.texcoords = data;
		attr.elementCount = static_cast<uint64_t>(std::max(0, count));
		snap.attrs.push_back(attr);
	}

	bool isEmpty(const Snapshot& snap) const
	{
		return snap.points == 0 && snap.prims == 0 && snap.vertices == 0;
	}

	void initSections(const Snapshot& snap)
	{
		mySections.clear();
		myIsMesh = snap.isMesh;
		myFirstPointCount = snap.points;
		if (myIsMesh)
		{
			addSection("faceVertexCounts", "faceVertexCounts", "",
				"int", "", 1, storageInt(), "faceVertexCounts", {});
			addSection("faceVertexIndices", "faceVertexIndices", "",
				"int", "", 1, storageInt(), "faceVertexIndices", {});
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
		section.path = normPath(myTmpDir / ("native_sop_" +
			std::to_string(mySections.size()) + ".bin"));
		mySections.push_back(section);
	}

	bool resolveAttr(const Attr& attr, Section& out)
	{
		if (attr.klass == NativeAttrClass::Point && attr.name == "P")
			return false;
		if (attr.numComponents < 1 || attr.numComponents > 4)
			throw std::runtime_error("Unsupported SOP attribute tuple size for " + attr.name);
		if (attr.type == NativeAttrType::Int32 && attr.numComponents != 1)
			throw std::runtime_error(
				"Native SOP writer supports only scalar int custom attributes: " +
				attr.name);

		out.attrClass = attr.klass;
		out.attrName = attr.name;
		out.interpolation = interpolationFor(attr.klass);
		out.storage = attr.type == NativeAttrType::Int32 ? storageInt() : storageFloat();
		out.path = normPath(myTmpDir / ("native_sop_" +
			std::to_string(mySections.size()) + ".bin"));

		if (attr.name == "N" && attr.type != NativeAttrType::Int32 &&
			attr.numComponents >= 3)
		{
			out.name = "normals";
			out.target = "normals";
			out.usdType = maybeHalf("normal3f", true, myHalfMode);
			out.tupleSize = 3;
			out.components = {0, 1, 2};
			return true;
		}
		if (attr.klass == NativeAttrClass::Point &&
			(attr.name == "v" || attr.name == "PartVel") &&
			attr.type != NativeAttrType::Int32 && attr.numComponents >= 3)
		{
			out.name = "velocities";
			out.target = "velocities";
			out.usdType = maybeHalf("vector3f", true, myHalfMode);
			out.interpolation.clear();
			out.tupleSize = 3;
			out.components = {0, 1, 2};
			return true;
		}
		if (attr.klass == NativeAttrClass::Vertex &&
			(attr.name == "Tex" || attr.name == "uv") &&
			attr.type != NativeAttrType::Int32 && attr.numComponents >= 2)
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
			attr.type != NativeAttrType::Int32 && attr.numComponents >= 3)
		{
			out.name = "primvars:Cd";
			out.target = "primvar";
			out.primvarName = "Cd";
			out.usdType = maybeHalf("color3f", true, myHalfMode);
			out.tupleSize = 3;
			out.components = {0, 1, 2};
			return true;
		}

		if (attr.type == NativeAttrType::Int32)
		{
			out.name = "primvars:" + attr.name;
			out.target = "primvar";
			out.primvarName = attr.name;
			out.usdType = "int";
			out.tupleSize = 1;
			out.components = {0};
			return true;
		}

		const std::string type = scalarUsdType(attr.numComponents);
		if (type.empty())
			throw std::runtime_error("Unsupported SOP float attribute size: " + attr.name);
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
		s << (snap.isMesh ? "mesh" : "points");
		for (const Attr& attr : snap.attrs)
		{
			Section section;
			if (!const_cast<TDSopUsdWriter*>(this)->resolveAttr(attr, section))
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
		appendBytes(out, snap.faceCounts);
		appendBytes(out, snap.faceIndices);
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
		if (myIsMesh)
		{
			if (topologyBytes(snap) != myFirstTopo)
				throw std::runtime_error(
					"Topology changed but Topology Changes is off");
			return;
		}
		if (snap.points != myFirstPointCount)
			throw std::runtime_error(
				"Point count changed but Topology Changes is off");
	}

	void appendEmptyFrame(int32_t frame)
	{
		Snapshot empty;
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
			else if (section.target == "extent")
				writeExtent(section, frame, snap, empty);
			else if (section.target == "points")
				writeAttribute(section, frame, snap, "P", empty);
			else if (section.target == "widths")
				writeWidth(section, frame);
			else
				writeAttribute(section, frame, snap, section.attrName, empty);
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
			const Attr* p = findAttr(snap, NativeAttrClass::Point, "P");
			if (p && p->positions && p->elementCount > 0)
			{
				float minv[3] = {
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max()};
				float maxv[3] = {
					-std::numeric_limits<float>::max(),
					-std::numeric_limits<float>::max(),
					-std::numeric_limits<float>::max()};
				for (uint64_t i = 0; i < p->elementCount; ++i)
				{
					const Position& pos = p->positions[i];
					const float values[3] = {pos.x, pos.y, pos.z};
					for (uint32_t c = 0; c < 3; ++c)
					{
						minv[c] = std::min(minv[c], values[c]);
						maxv[c] = std::max(maxv[c], values[c]);
					}
				}
				for (uint32_t c = 0; c < 3; ++c)
				{
					extent[c] = minv[c];
					extent[c + 3] = maxv[c];
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
		const std::string& name, bool empty)
	{
		if (empty)
		{
			writeRaw(section, frame, nullptr, 0, 0);
			return;
		}
		const Attr* attr = findAttr(snap, section.attrClass, name);
		if (!attr)
			throw std::runtime_error("Missing SOP attribute " + name);

		if (attr->type == NativeAttrType::Int32)
		{
			writeIntAttribute(section, frame, *attr);
			return;
		}
		writeFloatAttribute(section, frame, *attr);
	}

	void writeIntAttribute(Section& section, int32_t frame, const Attr& attr)
	{
		if (!attr.intData)
			throw std::runtime_error("SOP int attribute has no data: " + attr.name);
		writeRaw(section, frame, attr.intData,
			size_t(attr.elementCount) * sizeof(int32_t), attr.elementCount);
	}

	void writeFloatAttribute(Section& section, int32_t frame, const Attr& attr)
	{
		std::vector<float> tmp;
		tmp.reserve(size_t(attr.elementCount) * section.tupleSize);
		for (uint64_t i = 0; i < attr.elementCount; ++i)
		{
			for (uint32_t comp : section.components)
				tmp.push_back(readFloatComponent(attr, i, comp));
		}
		writeRaw(section, frame, tmp.data(), tmp.size() * sizeof(float),
			attr.elementCount);
	}

	float readFloatComponent(const Attr& attr, uint64_t index, uint32_t comp) const
	{
		switch (attr.type)
		{
			case NativeAttrType::Position:
			{
				if (!attr.positions)
					throw std::runtime_error("SOP position data is missing");
				const Position& p = attr.positions[index];
				const float values[3] = {p.x, p.y, p.z};
				return values[comp];
			}
			case NativeAttrType::Vector:
			{
				if (!attr.vectors)
					throw std::runtime_error("SOP vector data is missing: " + attr.name);
				const Vector& v = attr.vectors[index];
				const float values[3] = {v.x, v.y, v.z};
				return values[comp];
			}
			case NativeAttrType::Color:
			{
				if (!attr.colors)
					throw std::runtime_error("SOP color data is missing: " + attr.name);
				const Color& c = attr.colors[index];
				const float values[4] = {c.r, c.g, c.b, c.a};
				return values[comp];
			}
			case NativeAttrType::TexCoord:
			{
				if (!attr.texcoords)
					throw std::runtime_error("SOP texture data is missing: " + attr.name);
				const TexCoord& t = attr.texcoords[index];
				const float values[3] = {t.u, t.v, t.w};
				return values[comp];
			}
			case NativeAttrType::Float:
			{
				if (!attr.floatData)
					throw std::runtime_error("SOP float data is missing: " + attr.name);
				return attr.floatData[index * attr.numComponents + comp];
			}
			default:
				throw std::runtime_error("SOP attribute is not float-compatible: " + attr.name);
		}
	}

	const Attr* findAttr(const Snapshot& snap, NativeAttrClass klass,
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
		std::filesystem::create_directories(
			std::filesystem::path(section.path).parent_path());
		std::ofstream out(section.path, std::ios::binary | std::ios::app);
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
	uint32_t myFirstPointCount = 0;
	int32_t myCookCount = 0;
	bool myInitialized = false;
	bool myIsMesh = false;
	bool myTopoVaries = false;
};

extern "C"
{
DLLEXPORT void FillSOPPluginInfo(SOP_PluginInfo* info)
{
	if (!info->setAPIVersion(SOPCPlusPlusAPIVersion))
		return;
	info->customOPInfo.opType->setString("Tdsopusdwriter");
	info->customOPInfo.opLabel->setString("TD SOP USD Writer");
	info->customOPInfo.opIcon->setString("TUS");
	info->customOPInfo.authorName->setString("TD-SOP-USD-Anim-Bridge");
	info->customOPInfo.authorEmail->setString("");
	info->customOPInfo.minInputs = 1;
	info->customOPInfo.maxInputs = 1;
}

DLLEXPORT SOP_CPlusPlusBase* CreateSOPInstance(const OP_NodeInfo* info)
{
	return new TDSopUsdWriter(info);
}

DLLEXPORT void DestroySOPInstance(SOP_CPlusPlusBase* instance)
{
	delete static_cast<TDSopUsdWriter*>(instance);
}
}
