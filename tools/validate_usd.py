"""Validate an exported USD file with usd-core (the pxr Python bindings).

TouchDesigner ships USD as C++ only (no `pxr` module), so this runs in a
separate environment. It opens the stage and, for every authored time code,
checks that element counts are coherent across the geometry: positions,
normals, velocities, extent, ids, and every primvar must match the count its
interpolation implies (vertex/varying -> points, faceVarying -> face-vertices,
uniform -> faces, constant -> 1).

Usage:
    tools/.venv-usd/Scripts/python.exe tools/validate_usd.py [path.usd[a|c]]
Default path: export/sop_usd_export.usda
"""

import sys

from pxr import Usd, UsdGeom


def _len(value):
	return 0 if value is None else len(value)


def _expected(interp, n_points, n_face_verts, n_faces):
	return {
		'constant': 1,
		'uniform': n_faces,
		'varying': n_points,
		'vertex': n_points,
		'faceVarying': n_face_verts,
	}.get(interp)


def _time_codes(prim):
	pb = UsdGeom.PointBased(prim)
	attrs = [pb.GetPointsAttr(), pb.GetNormalsAttr(), pb.GetVelocitiesAttr()]
	if prim.IsA(UsdGeom.Boundable):
		attrs.append(UsdGeom.Boundable(prim).GetExtentAttr())
	if prim.IsA(UsdGeom.Mesh):
		mesh = UsdGeom.Mesh(prim)
		attrs.extend([mesh.GetFaceVertexCountsAttr(),
			mesh.GetFaceVertexIndicesAttr()])
	if prim.IsA(UsdGeom.BasisCurves):
		curves = UsdGeom.BasisCurves(prim)
		attrs.extend([curves.GetCurveVertexCountsAttr(),
			curves.GetWidthsAttr()])
	if prim.IsA(UsdGeom.Points):
		attrs.append(UsdGeom.Points(prim).GetWidthsAttr())
	attrs.extend(UsdGeom.PrimvarsAPI(prim).GetPrimvars())

	samples = set()
	for attr in attrs:
		if attr and attr.IsDefined():
			samples.update(attr.GetTimeSamples())
	return sorted(samples) if samples else [Usd.TimeCode.Default()]


def validate(path):
	stage = Usd.Stage.Open(path)
	if stage is None:
		print('FAIL: could not open %s' % path)
		return False

	prim = None
	for p in stage.Traverse():
		if (p.IsA(UsdGeom.Mesh) or p.IsA(UsdGeom.Points)
				or p.IsA(UsdGeom.BasisCurves)):
			prim = p
			break
	if prim is None:
		print('FAIL: no Mesh, Points, or BasisCurves prim found')
		return False

	is_mesh = prim.IsA(UsdGeom.Mesh)
	is_curves = prim.IsA(UsdGeom.BasisCurves)
	mesh = UsdGeom.Mesh(prim) if is_mesh else None
	curves = UsdGeom.BasisCurves(prim) if is_curves else None
	pb = UsdGeom.PointBased(prim)
	api = UsdGeom.PrimvarsAPI(prim)
	failures = []
	kind = 'Mesh' if is_mesh else 'BasisCurves' if is_curves else 'Points'

	print('Validating %s  (%s "%s")'
		% (path, kind, prim.GetName()))

	for t in _time_codes(prim):
		n_points = _len(pb.GetPointsAttr().Get(t))
		n_faces = n_face_verts = 0
		n_curves = n_curve_verts = 0
		if is_mesh:
			counts = mesh.GetFaceVertexCountsAttr().Get(t)
			indices = mesh.GetFaceVertexIndicesAttr().Get(t)
			n_faces = _len(counts)
			n_face_verts = _len(indices)
			if counts is not None and sum(counts) != n_face_verts:
				failures.append('t=%s sum(faceVertexCounts)=%d != '
					'faceVertexIndices=%d' % (t, sum(counts), n_face_verts))
			if indices and max(indices) >= n_points:
				failures.append('t=%s index %d out of range (points=%d)'
					% (t, max(indices), n_points))
		if is_curves:
			counts = curves.GetCurveVertexCountsAttr().Get(t)
			n_curves = _len(counts)
			n_curve_verts = sum(counts) if counts is not None else 0
			n_faces = n_curves
			n_face_verts = n_curve_verts
			if counts is not None and n_curve_verts != n_points:
				failures.append('t=%s sum(curveVertexCounts)=%d != '
					'points=%d' % (t, n_curve_verts, n_points))

		checks = []
		normals = pb.GetNormalsAttr().Get(t)
		if normals is not None:
			checks.append(('normals', pb.GetNormalsInterpolation(),
				_len(normals)))
		velocities = pb.GetVelocitiesAttr().Get(t)
		if velocities is not None and _len(velocities) != n_points:
			failures.append('t=%s velocities len=%d != points=%d'
				% (t, _len(velocities), n_points))
		extent = UsdGeom.Boundable(prim).GetExtentAttr().Get(t)
		if extent is not None and _len(extent) != 2:
			failures.append('t=%s extent len=%d != 2'
				% (t, _len(extent)))
		if is_curves:
			widths = curves.GetWidthsAttr().Get(t)
			if widths is not None:
				checks.append(('widths', curves.GetWidthsInterpolation(),
					_len(widths)))
		elif prim.IsA(UsdGeom.Points):
			points = UsdGeom.Points(prim)
			widths = points.GetWidthsAttr().Get(t)
			if widths is not None:
				checks.append(('widths', points.GetWidthsInterpolation(),
					_len(widths)))
		for pv in api.GetPrimvars():
			val = pv.Get(t)
			if val is not None:
				checks.append((pv.GetName(), pv.GetInterpolation(),
					_len(val)))

		for name, interp, got in checks:
			exp = _expected(interp, n_points, n_face_verts, n_faces)
			if exp is not None and got != exp:
				failures.append('t=%s %s (%s) len=%d != expected %d'
					% (t, name, interp, got, exp))

		tag = 't=%s' % t
		if is_curves:
			print('  %-10s points=%d curves=%d curveVerts=%d  attrs=%d'
				% (tag, n_points, n_curves, n_curve_verts, len(checks)))
		else:
			print('  %-10s points=%d faces=%d faceVerts=%d  attrs=%d'
				% (tag, n_points, n_faces, n_face_verts, len(checks)))

	if failures:
		print('FAIL: %d problem(s):' % len(failures))
		for f in failures:
			print('  - %s' % f)
		return False
	print('PASS: all time codes coherent')
	return True


if __name__ == '__main__':
	target = sys.argv[1] if len(sys.argv) > 1 else 'export/sop_usd_export.usda'
	sys.exit(0 if validate(target) else 1)
