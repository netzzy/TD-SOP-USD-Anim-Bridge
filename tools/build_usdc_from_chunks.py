"""Build an animated .usdc directly from TD binary chunk files.

TouchDesigner writes the chunks and manifest without importing usd-core. This
sidecar process owns all pxr/numpy work and authors the final crate layer.

Usage:
    python tools/build_usdc_from_chunks.py manifest.json output.usdc
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import numpy as np
from pxr import Sdf, Usd, UsdGeom


VALUE_TYPES = {
	"float": "FloatArray",
	"float2": "Float2Array",
	"float3": "Float3Array",
	"float4": "Float4Array",
	"half": "HalfArray",
	"half2": "Half2Array",
	"half3": "Half3Array",
	"half4": "Half4Array",
	"point3f": "Point3fArray",
	"point3h": "Point3hArray",
	"normal3f": "Normal3fArray",
	"normal3h": "Normal3hArray",
	"texCoord2f": "TexCoord2fArray",
	"texCoord2h": "TexCoord2hArray",
	"vector3f": "Vector3fArray",
	"vector3h": "Vector3hArray",
	"color3f": "Color3fArray",
	"color3h": "Color3hArray",
	"int": "IntArray",
	"int64": "Int64Array",
}


def _value_type(usd_type: str):
	try:
		return getattr(Sdf.ValueTypeNames, VALUE_TYPES[usd_type])
	except KeyError as exc:
		raise RuntimeError(f"Unsupported USD array type: {usd_type}") from exc


def _output_dtype(usd_type: str, storage: str):
	if storage in ("<i4", ">i4"):
		return np.int32
	if storage in ("<i8", ">i8"):
		return np.int64
	if usd_type.endswith("h") or usd_type == "half":
		return np.float16
	return np.float32


def _read_values(section: dict, sample: dict):
	count = int(sample["count"])
	tuple_size = int(section["tupleSize"])
	storage = section["storage"]
	total = count * tuple_size
	dtype = np.dtype(storage)
	out_dtype = _output_dtype(section["usdType"], storage)

	if total == 0:
		if tuple_size == 1:
			return np.empty((0,), dtype=out_dtype)
		return np.empty((0, tuple_size), dtype=out_dtype)

	with open(section["path"], "rb") as f:
		f.seek(int(sample["offset"]))
		arr = np.fromfile(f, dtype=dtype, count=total)
	if arr.size != total:
		raise RuntimeError(
			"%s sample at time %s is truncated: expected %d values, got %d"
			% (section["name"], sample.get("time", "default"), total, arr.size))
	arr = arr.astype(out_dtype, copy=False)
	if tuple_size == 1:
		return arr
	return arr.reshape((count, tuple_size))


def _create_attr(geom, section: dict):
	target = section["target"]
	interp = section.get("interpolation")
	prim = geom.GetPrim()
	pb = UsdGeom.PointBased(prim)
	value_type = _value_type(section["usdType"])

	if target == "faceVertexCounts":
		return prim.CreateAttribute("faceVertexCounts", value_type, False)
	if target == "faceVertexIndices":
		return prim.CreateAttribute("faceVertexIndices", value_type, False)
	if target == "extent":
		return prim.CreateAttribute("extent", value_type, False)
	if target == "points":
		return prim.CreateAttribute("points", value_type, False)
	if target == "normals":
		if interp:
			pb.SetNormalsInterpolation(interp)
		return prim.CreateAttribute("normals", value_type, False)
	if target == "velocities":
		return prim.CreateAttribute("velocities", value_type, False)
	if target == "ids":
		return prim.CreateAttribute("ids", value_type, False)
	if target == "widths":
		points = UsdGeom.Points(prim)
		if interp:
			points.SetWidthsInterpolation(interp)
		return prim.CreateAttribute("widths", value_type, False)
	if target == "primvar":
		name = section["primvarName"]
		return UsdGeom.PrimvarsAPI(prim).CreatePrimvar(
			name, value_type, interp or "").GetAttr()
	raise RuntimeError("Unsupported section target: %s" % target)


def build(manifest_path: str, output_path: str) -> bool:
	with open(manifest_path, "r", encoding="utf-8") as f:
		manifest = json.load(f)
	if manifest.get("version") != 1:
		raise RuntimeError("Unsupported chunk manifest version: %r"
			% manifest.get("version"))

	stage_info = manifest["stage"]
	output = Path(output_path).resolve()
	output.parent.mkdir(parents=True, exist_ok=True)
	if output.exists():
		output.unlink()

	stage = Usd.Stage.CreateNew(str(output))
	stage.SetFramesPerSecond(float(stage_info["framesPerSecond"]))
	stage.SetTimeCodesPerSecond(float(stage_info["timeCodesPerSecond"]))
	stage.SetStartTimeCode(float(stage_info["startTimeCode"]))
	stage.SetEndTimeCode(float(stage_info["endTimeCode"]))
	UsdGeom.SetStageMetersPerUnit(stage, float(stage_info.get("metersPerUnit", 1)))
	UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

	if stage_info["isMesh"]:
		geom = UsdGeom.Mesh.Define(stage, "/Exported")
		geom.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
	else:
		geom = UsdGeom.Points.Define(stage, "/Exported")
	stage.SetDefaultPrim(geom.GetPrim())

	for section in manifest["sections"]:
		attr = _create_attr(geom, section)
		if section["kind"] == "const":
			attr.Set(_read_values(section, section["sample"]))
		elif section["kind"] == "sampler":
			for sample in section["samples"]:
				attr.Set(_read_values(section, sample), float(sample["time"]))
		else:
			raise RuntimeError("Unsupported section kind: %s" % section["kind"])

	if not stage.GetRootLayer().Save():
		raise RuntimeError("Could not save output layer: %s" % output)
	if Usd.Stage.Open(str(output)) is None:
		raise RuntimeError("Output layer does not parse after save: %s" % output)
	print("PASS: wrote %s (%d bytes)" % (output, output.stat().st_size))
	return True


def main(argv: list[str]) -> int:
	if len(argv) != 3:
		print("Usage: python tools/build_usdc_from_chunks.py manifest.json output.usdc")
		return 2
	try:
		return 0 if build(argv[1], argv[2]) else 1
	except BaseException as exc:
		print("FAIL: %s" % exc, file=sys.stderr)
		return 1


if __name__ == "__main__":
	raise SystemExit(main(sys.argv))
