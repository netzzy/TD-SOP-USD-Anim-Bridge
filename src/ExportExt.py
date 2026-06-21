"""TD-SOP-USD-Anim-Bridge exporter module.

M0: sampler -> plain-Python intermediate representation (IR).
M1: static .usda point cloud.
M2: animated point cloud (USD time samples).
M3: polygon mesh (UsdGeomMesh); point/vertex/prim attribute classes
    map to USD vertex/faceVarying/uniform interpolations. Native TD
    Mesh-primitive tessellation (grid -> quads, poles -> triangles).
M4: changing topology (faceVertexCounts/faceVertexIndices time-sampled).

Animated .usda export is STREAMING: TouchDesigner's normal playback advances
frames sequentially with project.realTime disabled, and a frame-end callback
writes each frame's line into per-attribute temp files. The final .usda is
assembled by streaming those temp files into each attribute's timeSamples
block. Peak memory is one frame, so the exportable size is bounded by disk,
not RAM.
Animated .usdc export uses the same playback driver but writes binary numeric
chunks plus a manifest; a sidecar usd-core/numpy process authors the crate
directly, avoiding the ASCII .usda formatting path. Static .usdc still
transcodes a temporary .usda.
Whether topology changes is declared by the user (Topologychanges par),
not detected by pre-scanning the range.

Geometry kind is chosen from the data: particle/point prims -> Points;
Poly/Mesh prims -> Mesh. Empty animated frames preserve the established
kind instead of switching schemas mid-export.
"""

import os
import json
import shutil
import subprocess
import sys
import tempfile
import time
from array import array


MODE_SOP_PYTHON = 'sop_python'
MODE_SOP_NATIVE = 'sop_native'
MODE_POP_NATIVE = 'pop_native'


class ExportExt:
	def __init__(self, ownerComp):
		self.ownerComp = ownerComp
		self._playbackExport = None
		self.RefreshUiState()

	# ---- public API -------------------------------------------------

	def Sample(self):
		"""Sample IN_FOR_EXPORT for the current frame into an IR dict."""
		return self._sampleSop(self._inputSop())

	def Export(self):
		"""Write a USD file, or start an animated playback export."""
		fmt = self._format()
		finalPath = self._resolvePath(fmt)
		animated = self._parBool('Animate')
		mode = self._exportMode()
		if mode != MODE_SOP_PYTHON:
			return self._exportNative(mode, fmt, finalPath, animated)
		tmpUsda = None
		path = finalPath
		try:
			usdaPath = finalPath
			if fmt == 'usdc' and not animated:
				tmpUsda = self._tempUsdaPath(finalPath)
				usdaPath = tmpUsda

			if animated:
				startedPath = self._startPlaybackExport(
					fmt, finalPath, usdaPath, tmpUsda)
				# Ownership moved to the playback state machine. Do not remove
				# the temp .usda from this synchronous finally block.
				tmpUsda = None
				return startedPath
			else:
				self._ensureFolder(usdaPath)
				with open(usdaPath, 'w', encoding='utf-8') as f:
					f.write(self._build(self.Sample()))
				count = 'static'

			if fmt == 'usdc':
				self._transcode(usdaPath, finalPath)
				path = finalPath

			print('[TD-SOP-USD-Anim-Bridge] wrote %s %s -> %s' % (count, fmt, path))
			return path
		finally:
			if tmpUsda and os.path.exists(tmpUsda):
				os.remove(tmpUsda)

	def PlaybackFrame(self, frame):
		"""Frame-end callback used by the playback-driven animated exporter."""
		state = getattr(self, '_playbackExport', None)
		if not state or not state.get('active'):
			return
		try:
			f = int(round(frame))
			if f < state['frameStart']:
				self._setProgress(0)
				self._setExportStatus('Pre-roll %d -> %d'
					% (f, state['frameStart']))
				return
			if state['written'] >= state['frameCount']:
				self._finishPlaybackExport()
				return
			expected = state['sourceFrames'][state['written']]
			if f < expected:
				return
			if f > expected:
				raise RuntimeError(
					'Playback skipped frame %d and reached %d. Export stopped '
					'before assembling an incomplete USD file.' % (expected, f))
			if f > state['frameEnd']:
				raise RuntimeError(
					'Playback passed Frame End before export completed '
					'(%d/%d frames written).' % (
						state['written'], state['frameCount']))

			if state.get('nativeMode') in (MODE_SOP_NATIVE, MODE_POP_NATIVE):
				outputFrame = state['frames'][state['written']]
				self._appendNativeFrame(state, outputFrame)
				state['written'] += 1
				state['writtenFrames'].add(f)
				self._publishPlaybackState(state)
				self._setProgress(float(state['written']) / state['frameCount'])
				self._setExportStatus('Exporting native %s %d/%d'
					% (self._nativeModeLabel(state['nativeMode']),
						state['written'], state['frameCount']))
				if state['written'] >= state['frameCount']:
					self._finishPlaybackExport()
				return

			ir = self._sampleSop(self._inputSop())
			outputFrame = state['frames'][state['written']]
			if state['sections'] is None:
				if self._isEmptyIr(ir):
					state['pendingEmptyFrames'].append((f, outputFrame, ir))
					state['written'] += 1
					state['writtenFrames'].add(f)
					if state['written'] >= state['frameCount']:
						self._initPlaybackSections(state, ir)
						self._flushPendingEmptyFrames(state)
						self._finishPlaybackExport()
						return
					self._publishPlaybackState(state)
					self._setProgress(float(state['written']) / state['frameCount'])
					self._setExportStatus('Exporting %d/%d'
						% (state['written'], state['frameCount']))
					return
				self._initPlaybackSections(state, ir)
				self._flushPendingEmptyFrames(state)
			elif self._schema(ir) != state['firstSchema']:
				if self._isEmptyIr(ir):
					ir = self._emptyIrForSchema(ir, state['firstSchema'])
				else:
					raise RuntimeError(
						'Geometry kind or attribute set/sizes change at frame '
						'%d. Counts/topology may vary, the schema may not.' % f)
			if not state['topoVaries'] and self._topoKey(ir) != state['firstTopo']:
				raise RuntimeError(
					'Topology changes at frame %d but "Topology Changes" '
					'is off. Enable it and re-export.' % f)
			self._streamPlaybackFrameStep(state, ir, f,
				state['streamed'] == 0, outputFrame)
			state['streamed'] += 1
			state['written'] += 1
			state['writtenFrames'].add(f)
			self._publishPlaybackState(state)
			self._setProgress(float(state['written']) / state['frameCount'])
			self._setExportStatus('Exporting %d/%d'
				% (state['written'], state['frameCount']))
			if state['written'] >= state['frameCount']:
				self._finishPlaybackExport()
		except Exception as e:
			self._finishPlaybackExport(error=e)

	def CancelExport(self):
		"""Cancel an active playback export and clean temporary files."""
		state = getattr(self, '_playbackExport', None)
		if not state or not state.get('active'):
			stored = self.ownerComp.fetch('_tdsopusd_playback_export', None)
			if stored and stored.get('active'):
				self._clearOrphanPlaybackExport(stored)
			else:
				self._setExportStatus('No active export')
				self.RefreshUiState()
			return
		self._finishPlaybackExport(cancelled=True)

	def RefreshUiState(self):
		"""Enable only the controls that are meaningful in the current state."""
		active = self._playbackActive()
		animated = self._parBool('Animate')
		self._setParsEnabled(
			('Playbackstart', 'Framestart', 'Frameend', 'Framestep',
				'Outputfps', 'Topologychanges'),
			animated and not active)
		self._setParsEnabled(('Cancel',), active)
		self._setParsEnabled(('Export',), not active)
		self._setParsEnabled(('Animate',), not active)
		self._setParsEnabled(('Exportmode', 'Tempfolder'), not active)
		self._setParsReadOnly(('Progress', 'Exportstatus', 'Nativestatus'), True)
		self._refreshNativeStatus(log=False)

	def SetupBinarySupport(self):
		"""Install/update the usd-core sidecar venv without blocking TD."""
		running = self.ownerComp.fetch('_tdsopusd_setup_binary', None)
		if running:
			self._setBinaryStatus('Setup already running (pid %d)'
				% int(running['pid']))
			return int(running['pid'])
		bundled = self._bundledUsdPython()
		if bundled and self._pythonHasUsd(bundled):
			self._setBinaryStatus('Binary support ready')
			return bundled

		script = os.path.join(project.folder, 'tools', 'setup.py')
		if not os.path.isfile(script):
			raise RuntimeError('Binary setup script not found: %s' % script)

		py = self._tdPython()
		tmpdir = self._makeTempDir('setup_')
		logPath = os.path.join(tmpdir, 'setup.log')
		statusPath = os.path.join(tmpdir, 'status.json')
		cmd = [py, script, '--status-json', statusPath]
		with open(logPath, 'w', encoding='utf-8') as log:
			proc = subprocess.Popen(cmd, stdout=log,
				stderr=subprocess.STDOUT, text=True)
		self.ownerComp.store('_tdsopusd_setup_binary', {
			'pid': proc.pid,
			'log': logPath,
			'status': statusPath,
		})
		self._setBinaryStatus('Setup running (pid %d)' % proc.pid)
		run("op(%r).SetupBinaryPoll()" % self.ownerComp.path, delayFrames=30)
		return proc.pid

	def SetupBinaryPoll(self):
		"""Poll the setup subprocess started by SetupBinarySupport()."""
		state = self.ownerComp.fetch('_tdsopusd_setup_binary', None)
		if not state:
			return
		statusPath = state['status']
		logPath = state['log']
		if os.path.isfile(statusPath):
			with open(statusPath, 'r', encoding='utf-8') as f:
				status = json.load(f)
			if os.path.isfile(logPath):
				with open(logPath, 'r', encoding='utf-8', errors='replace') as f:
					log = f.read().strip()
				if log:
					print('[TD-SOP-USD-Anim-Bridge] setup output:\n%s' % log)
			if status.get('ok'):
				self._setBinaryStatus('Binary support ready')
			else:
				self._setBinaryStatus('Setup failed: %s'
					% status.get('error', 'unknown error'))
			self.ownerComp.unstore('_tdsopusd_setup_binary')
			return
		try:
			os.kill(int(state['pid']), 0)
		except OSError:
			log = self._readSetupLog(logPath)
			if log:
				print('[TD-SOP-USD-Anim-Bridge] setup output:\n%s' % log)
			bundled = self._bundledUsdPython()
			if bundled and self._pythonHasUsd(bundled):
				self._setBinaryStatus(
					'Binary support ready (setup exited without status)')
			else:
				self._setBinaryStatus(
					'Setup failed: process exited without status; see %s'
					% logPath)
			self.ownerComp.unstore('_tdsopusd_setup_binary')
			return
		run("op(%r).SetupBinaryPoll()" % self.ownerComp.path, delayFrames=30)

	def SetupNativeSupport(self):
		"""Build optional native plugins without making them mandatory."""
		running = self.ownerComp.fetch('_tdsopusd_setup_native', None)
		if running:
			self._setNativeStatus('Native setup already running (pid %d)'
				% int(running['pid']))
			return int(running['pid'])
		script = os.path.join(project.folder, 'native', 'build.ps1')
		if not os.path.isfile(script):
			self._setNativeStatus('Native build script not found: %s' % script)
			return ''
		self._setNativePluginsUnload(True)
		tmpdir = self._makeTempDir('native_setup_')
		logPath = os.path.join(tmpdir, 'native_setup.log')
		statusPath = os.path.join(tmpdir, 'native_status.json')
		cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
			'-File', script, '-StatusJson', statusPath]
		with open(logPath, 'w', encoding='utf-8') as log:
			proc = subprocess.Popen(cmd, stdout=log,
				stderr=subprocess.STDOUT, text=True)
		self.ownerComp.store('_tdsopusd_setup_native', {
			'pid': proc.pid,
			'log': logPath,
			'status': statusPath,
		})
		self._setNativeStatus('Native setup running (pid %d)' % proc.pid)
		run("op(%r).SetupNativePoll()" % self.ownerComp.path, delayFrames=30)
		return proc.pid

	def SetupNativePoll(self):
		"""Poll the native plugin build started by SetupNativeSupport()."""
		state = self.ownerComp.fetch('_tdsopusd_setup_native', None)
		if not state:
			return
		statusPath = state['status']
		logPath = state['log']
		if os.path.isfile(statusPath):
			with open(statusPath, 'r', encoding='utf-8') as f:
				status = json.load(f)
			log = self._readSetupLog(logPath)
			if log:
				print('[TD-SOP-USD-Anim-Bridge] native setup output:\n%s' % log)
			if status.get('ok'):
				self._setNativeStatus('Native support built')
			else:
				self._setNativeStatus('Native setup failed: %s'
					% status.get('error', 'unknown error'))
			self.ownerComp.unstore('_tdsopusd_setup_native')
			return
		try:
			os.kill(int(state['pid']), 0)
		except OSError:
			log = self._readSetupLog(logPath)
			self._setNativeStatus(
				'Native setup exited without status; see %s' % logPath)
			if log:
				print('[TD-SOP-USD-Anim-Bridge] native setup output:\n%s' % log)
			self.ownerComp.unstore('_tdsopusd_setup_native')
			return
		run("op(%r).SetupNativePoll()" % self.ownerComp.path, delayFrames=30)

	# ---- helpers ----------------------------------------------------

	def _inputSop(self):
		sop = self.ownerComp.op('IN_FOR_EXPORT')
		if sop is None:
			raise RuntimeError('IN_FOR_EXPORT not found inside the module')
		return sop

	def _inputPop(self):
		pop = self.ownerComp.op('IN_POP_FOR_EXPORT')
		if pop is None:
			raise RuntimeError('IN_POP_FOR_EXPORT not found inside the module')
		return pop

	def _exportMode(self):
		par = getattr(self.ownerComp.par, 'Exportmode', None)
		mode = str(par.eval() if par is not None else MODE_SOP_PYTHON).lower()
		aliases = {
			'compatible': MODE_SOP_PYTHON,
			'compatible_sop_python': MODE_SOP_PYTHON,
			'sop python': MODE_SOP_PYTHON,
			'experimental_native_sop': MODE_SOP_NATIVE,
			'native_sop': MODE_SOP_NATIVE,
			'sop native': MODE_SOP_NATIVE,
			'experimental_native_pop': MODE_POP_NATIVE,
			'native_pop': MODE_POP_NATIVE,
			'pop native': MODE_POP_NATIVE,
		}
		mode = aliases.get(mode, mode)
		if mode not in (MODE_SOP_PYTHON, MODE_SOP_NATIVE, MODE_POP_NATIVE):
			raise RuntimeError('Unsupported Export Mode: %s' % mode)
		return mode

	def _exportNative(self, mode, fmt, finalPath, animated):
		if mode == MODE_SOP_NATIVE:
			self._preflightSopNative()
			self._requireNativeReady(mode)
			self._usdPython()
			if animated:
				return self._startPlaybackExport(
					fmt, finalPath, finalPath, None, nativeMode=MODE_SOP_NATIVE)
			return self._exportNativeStatic(mode, fmt, finalPath)
		if mode != MODE_POP_NATIVE:
			raise RuntimeError('Unsupported native mode: %s' % mode)
		self._requireNativeReady(mode)
		# Native chunk assembly uses the usd-core/numpy sidecar for both .usda
		# and .usdc because TD must not import pxr in-process.
		self._usdPython()
		if animated:
			return self._startPlaybackExport(
				fmt, finalPath, finalPath, None, nativeMode=MODE_POP_NATIVE)
		return self._exportNativeStatic(mode, fmt, finalPath)

	def _format(self):
		par = getattr(self.ownerComp.par, 'Format', None)
		fmt = str(par.eval() if par is not None else 'usda').lower()
		if fmt not in ('usda', 'usdc'):
			raise RuntimeError('Unsupported USD format: %s' % fmt)
		return fmt

	def _parBool(self, name, default=False):
		par = getattr(self.ownerComp.par, name, None)
		if par is None:
			return default
		value = par.eval()
		if isinstance(value, str):
			return value.strip().lower() in ('1', 'true', 'on', 'yes')
		return bool(value)

	def _halfMode(self):
		par = getattr(self.ownerComp.par, 'Halfprecision', None)
		mode = str(par.eval() if par is not None else 'off').lower()
		if mode not in ('off', 'safe', 'all'):
			raise RuntimeError('Unsupported Half Precision mode: %s' % mode)
		return mode

	def _maybeHalf(self, usdType, eligible=False):
		mode = self._halfMode()
		if mode == 'off' or (mode == 'safe' and not eligible):
			return usdType
		return {
			'point3f': 'point3h',
			'normal3f': 'normal3h',
			'texCoord2f': 'texCoord2h',
			'vector3f': 'vector3h',
			'color3f': 'color3h',
			'float': 'half',
			'float2': 'half2',
			'float3': 'half3',
			'float4': 'half4',
		}.get(usdType, usdType)

	def _resolvePath(self, fmt=None):
		path = tdu.expandPath(self.ownerComp.par.File.eval())
		if not os.path.isabs(path):
			path = os.path.join(project.folder, path)
		if fmt:
			root, _ = os.path.splitext(path)
			path = root + '.' + fmt
		return path

	def _ensureFolder(self, path):
		folder = os.path.dirname(path)
		if folder:
			os.makedirs(folder, exist_ok=True)

	def _tempRoot(self):
		par = getattr(self.ownerComp.par, 'Tempfolder', None)
		value = str(par.eval()).strip() if par is not None else ''
		if not value:
			value = '_tdsopusd_temp'
		value = tdu.expandPath(value)
		value = os.path.expanduser(os.path.expandvars(value))
		if not os.path.isabs(value):
			value = os.path.join(project.folder, value)
		path = os.path.abspath(value)
		os.makedirs(path, exist_ok=True)
		return path

	def _cleanupStaleTempDirs(self, maxAgeSeconds=24 * 60 * 60):
		root = self._tempRoot()
		now = time.time()
		for name in os.listdir(root):
			path = os.path.join(root, name)
			if os.path.isfile(path) and name.endswith('.tmp.usda'):
				try:
					age = now - os.path.getmtime(path)
				except OSError:
					continue
				if age > maxAgeSeconds:
					try:
						os.remove(path)
					except OSError:
						pass
				continue
			if not (name.startswith('export_') or name.startswith('setup_')
					or name.startswith('native_export_')
					or name.startswith('native_setup_')):
				continue
			if not os.path.isdir(path):
				continue
			markers = ('.tdsopusd_tmp',)
			if not any(os.path.isfile(os.path.join(path, m)) for m in markers):
				continue
			try:
				age = now - os.path.getmtime(path)
			except OSError:
				continue
			if age > maxAgeSeconds:
				shutil.rmtree(path, ignore_errors=True)

	def _makeTempDir(self, prefix):
		self._cleanupStaleTempDirs()
		path = tempfile.mkdtemp(prefix=prefix, dir=self._tempRoot())
		with open(os.path.join(path, '.tdsopusd_tmp'), 'w', encoding='utf-8') as f:
			f.write('TD-SOP-USD-Anim-Bridge temporary directory\n')
		return path

	def _tempUsdaPath(self, finalPath):
		base = os.path.splitext(os.path.basename(finalPath))[0] or 'export'
		fd, path = tempfile.mkstemp(
			prefix=base + '.', suffix='.tmp.usda', dir=self._tempRoot())
		os.close(fd)
		return path

	def _tdPython(self):
		if sys.executable and os.path.isfile(sys.executable):
			return sys.executable
		name = 'python.exe' if os.name == 'nt' else 'python'
		return os.path.join(app.binFolder, name)

	def _setupHint(self):
		return ('Press Setup Binary Support, run "python tools/setup.py", '
			'or set USD Python Executable/TD_SOP_USD_ANIM_BRIDGE_PYTHON. '
			'The Python environment must include usd-core and numpy. '
			'See tools/README.md.')

	def _parPath(self, name):
		par = getattr(self.ownerComp.par, name, None)
		if par is None:
			return ''
		value = str(par.eval()).strip()
		if not value:
			return ''
		value = tdu.expandPath(value)
		return os.path.abspath(os.path.expanduser(os.path.expandvars(value)))

	def _bundledUsdPython(self):
		base = os.path.join(project.folder, 'tools', '.venv-usd')
		candidates = []
		if os.name == 'nt':
			candidates.append(os.path.join(base, 'Scripts', 'python.exe'))
		candidates.extend([
			os.path.join(base, 'bin', 'python3'),
			os.path.join(base, 'bin', 'python'),
			os.path.join(base, 'Scripts', 'python.exe'),
		])
		for path in candidates:
			if os.path.isfile(path):
				return path
		return ''

	def _pythonHasUsd(self, path):
		if not path or not os.path.isfile(path):
			return False
		try:
			result = subprocess.run([path, '-c', 'from pxr import Usd; import numpy'],
				capture_output=True, text=True, timeout=15)
		except Exception:
			return False
		return result.returncode == 0

	def _readSetupLog(self, path, maxChars=6000):
		if not path or not os.path.isfile(path):
			return ''
		with open(path, 'r', encoding='utf-8', errors='replace') as f:
			text = f.read()
		if len(text) > maxChars:
			return '...\n' + text[-maxChars:]
		return text

	def _requirePython(self, path, label):
		if os.path.isfile(path):
			return path
		raise RuntimeError('%s does not point to a Python executable: %s. '
			'Fix or clear it, then %s' % (label, path, self._setupHint()))

	def _usdPython(self):
		parPath = self._parPath('Usdpython')
		if parPath:
			return self._requirePython(parPath, 'USD Python Executable')
		envPath = os.environ.get('TD_SOP_USD_ANIM_BRIDGE_PYTHON', '').strip()
		if envPath:
			envPath = os.path.abspath(os.path.expanduser(
				os.path.expandvars(envPath)))
			return self._requirePython(envPath, 'TD_SOP_USD_ANIM_BRIDGE_PYTHON')
		bundled = self._bundledUsdPython()
		if bundled:
			return bundled
		raise RuntimeError('USD binary export requires usd-core sidecar. %s'
			% self._setupHint())

	def _setBinaryStatus(self, msg):
		par = getattr(self.ownerComp.par, 'Binarystatus', None)
		if par is not None:
			par.val = msg
		print('[TD-SOP-USD-Anim-Bridge] %s' % msg)

	def _setNativeStatus(self, msg, log=True):
		par = getattr(self.ownerComp.par, 'Nativestatus', None)
		if par is not None:
			par.val = msg
		if log:
			print('[TD-SOP-USD-Anim-Bridge] %s' % msg)

	def _setNativePluginsUnload(self, unload):
		for name in ('NATIVE_SOP_WRITER', 'NATIVE_POP_WRITER'):
			native = self.ownerComp.op(name)
			if native is None:
				continue
			par = getattr(native.par, 'unloadplugin', None)
			if par is not None:
				par.val = bool(unload)

	def _refreshNativeStatus(self, log=False):
		par = getattr(self.ownerComp.par, 'Nativestatus', None)
		if par is None:
			return
		mode = MODE_SOP_PYTHON
		try:
			mode = self._exportMode()
		except Exception:
			pass
		if mode == MODE_SOP_PYTHON:
			self._setNativeStatus(
				'Compatible SOP Python active; native plugins optional',
				log=log)
			return
		label = self._nativeModeLabel(mode)
		path = self._nativePluginPath(mode)
		op = self._nativeOp(mode, required=False)
		if not os.path.isfile(path):
			self._setNativeStatus(
				'Experimental Native %s unavailable; build native plugins'
					% label,
				log=log)
		elif op is None:
			self._setNativeStatus(
				'Experimental Native %s unavailable; native %s node missing'
					% (label, label),
				log=log)
		else:
			self._setNativeStatus('Experimental Native %s ready' % label,
				log=log)

	def _nativeModeLabel(self, mode):
		return 'SOP' if mode == MODE_SOP_NATIVE else 'POP'

	def _nativeOpName(self, mode):
		if mode == MODE_SOP_NATIVE:
			return 'NATIVE_SOP_WRITER'
		if mode == MODE_POP_NATIVE:
			return 'NATIVE_POP_WRITER'
		raise RuntimeError('Unsupported native mode: %s' % mode)

	def _nativePluginRelPath(self, mode):
		if mode == MODE_SOP_NATIVE:
			name = 'TDSopUsdWriter.dll' if os.name == 'nt' else 'TDSopUsdWriter.so'
			return os.path.join('native', 'td_sop_usd_writer', 'build', name)
		if mode == MODE_POP_NATIVE:
			name = 'TDPopUsdWriter.dll' if os.name == 'nt' else 'TDPopUsdWriter.so'
			return os.path.join('native', 'td_pop_usd_writer', 'build', name)
		raise RuntimeError('Unsupported native mode: %s' % mode)

	def _nativePluginPath(self, mode):
		return os.path.join(project.folder, self._nativePluginRelPath(mode))

	def _nativePluginExpr(self, mode):
		rel = self._nativePluginRelPath(mode).replace('\\', '/')
		return "project.folder + '/%s'" % rel

	def _resolvePluginPath(self, value):
		value = tdu.expandPath(str(value).strip())
		if not value:
			return ''
		if not os.path.isabs(value):
			value = os.path.join(project.folder, value)
		return os.path.abspath(os.path.expanduser(os.path.expandvars(value)))

	def _nativeOp(self, mode, required=True):
		name = self._nativeOpName(mode)
		native = self.ownerComp.op(name)
		if native is None and required:
			raise RuntimeError('%s CPlusPlus %s not found'
				% (name, self._nativeModeLabel(mode)))
		return native

	def _requireNativeReady(self, mode):
		path = self._nativePluginPath(mode)
		label = self._nativeModeLabel(mode)
		if not os.path.isfile(path):
			raise RuntimeError(
				'Experimental Native %s plugin is not built: %s. Press '
				'Setup Native Support or run native/build.ps1.' % (label, path))
		op = self._nativeOp(mode, required=True)
		unload = getattr(op.par, 'unloadplugin', None)
		if unload is not None:
			unload.val = False
		plugin = getattr(op.par, 'Plugin', None)
		if plugin is None:
			plugin = getattr(op.par, 'plugin', None)
		if plugin is not None:
			current = str(plugin.eval()).strip()
			if self._resolvePluginPath(current) != os.path.abspath(path):
				plugin.expr = self._nativePluginExpr(mode)
				pulse = getattr(op.par, 'reinitpulse', None)
				if pulse is not None:
					pulse.pulse()
		if mode == MODE_SOP_NATIVE:
			return op
		if getattr(op.par, 'Command', None) is None:
			raise RuntimeError(
				'Native %s plugin did not expose its command parameters. '
				'Re-init %s after loading the native plugin.'
				% (label, self._nativeOpName(mode)))
		return op

	def _preflightSopNative(self):
		ir = self.Sample()
		unsupported = []
		for name in sorted(ir.get('vertexAttribs', {})):
			if name not in ('uv', 'Cd'):
				unsupported.append('vertex:%s' % name)
		for name in sorted(ir.get('primAttribs', {})):
			if name != 'Cd':
				unsupported.append('prim:%s' % name)
		if unsupported:
			raise RuntimeError(
				'Experimental Native SOP cannot preserve generic vertex/'
				'primitive custom attributes: %s. Use Compatible SOP Python '
				'or Experimental Native POP.' % ', '.join(unsupported))

	def _setNativePar(self, native, mode, name, value):
		par = getattr(native.par, name, None)
		if par is None:
			par = getattr(native.par, name.lower(), None)
		if par is None:
			raise RuntimeError(
				'%s is missing parameter %s. Re-init the CPlusPlus %s after '
				'loading the native plugin.'
				% (self._nativeOpName(mode), name, self._nativeModeLabel(mode)))
		par.val = value

	def _readNativeStatus(self, statusPath, mode):
		if not os.path.isfile(statusPath):
			raise RuntimeError(
				'Native %s writer did not write a status file: %s'
				% (self._nativeModeLabel(mode), statusPath))
		with open(statusPath, 'r', encoding='utf-8') as f:
			status = json.load(f)
		if not status.get('ok'):
			raise RuntimeError('Native %s writer failed: %s'
				% (self._nativeModeLabel(mode),
					status.get('error', 'unknown error')))
		return status

	def _runNativeCommand(self, state, command, frame=None):
		mode = state['nativeMode']
		if mode == MODE_SOP_NATIVE:
			return self._runNativeSopCommand(state, command, frame=frame)
		native = self._nativeOp(mode, required=True)
		state['nativeSeq'] = int(state.get('nativeSeq', 0)) + 1
		self._setNativePar(native, mode, 'Tmpdir', state['tmpdir'])
		self._setNativePar(native, mode, 'Statuspath', state['nativeStatusPath'])
		self._setNativePar(native, mode, 'Sequence', state['nativeSeq'])
		self._setNativePar(native, mode, 'Command', command)
		self._setNativePar(native, mode, 'Frame',
			int(state['frames'][0] if frame is None else frame))
		self._setNativePar(native, mode, 'Topovaries',
			1 if state['topoVaries'] else 0)
		self._setNativePar(native, mode, 'Halfmode', self._halfMode())
		self._setNativePar(native, mode, 'Fps', state['fps'])
		self._setNativePar(native, mode, 'Starttime', state['frames'][0])
		self._setNativePar(native, mode, 'Endtime', state['frames'][-1])
		try:
			native.cook(force=True)
		except TypeError:
			native.cook()
		return self._readNativeStatus(state['nativeStatusPath'], mode)

	def _nativeSopEnv(self, state, command, frame=None):
		return {
			'TD_SOP_USD_WRITER_COMMAND': command,
			'TD_SOP_USD_WRITER_TMPDIR': state['tmpdir'],
			'TD_SOP_USD_WRITER_STATUSPATH': state['nativeStatusPath'],
			'TD_SOP_USD_WRITER_SEQUENCE': int(state['nativeSeq']),
			'TD_SOP_USD_WRITER_FRAME':
				int(state['frames'][0] if frame is None else frame),
			'TD_SOP_USD_WRITER_TOPOVARIES':
				1 if state['topoVaries'] else 0,
			'TD_SOP_USD_WRITER_HALFMODE': self._halfMode(),
			'TD_SOP_USD_WRITER_FPS': state['fps'],
			'TD_SOP_USD_WRITER_STARTTIME': state['frames'][0],
			'TD_SOP_USD_WRITER_ENDTIME': state['frames'][-1],
		}

	def _clearNativeSopEnv(self):
		for key in (
				'TD_SOP_USD_WRITER_COMMAND',
				'TD_SOP_USD_WRITER_TMPDIR',
				'TD_SOP_USD_WRITER_STATUSPATH',
				'TD_SOP_USD_WRITER_SEQUENCE',
				'TD_SOP_USD_WRITER_FRAME',
				'TD_SOP_USD_WRITER_TOPOVARIES',
				'TD_SOP_USD_WRITER_HALFMODE',
				'TD_SOP_USD_WRITER_FPS',
				'TD_SOP_USD_WRITER_STARTTIME',
				'TD_SOP_USD_WRITER_ENDTIME'):
			os.environ.pop(key, None)

	def _clearNativeCommandPars(self, mode):
		native = self._nativeOp(mode, required=False)
		if native is None:
			return
		defaults = {
			'Command': 'idle',
			'Tmpdir': '',
			'Statuspath': '',
			'Sequence': 0,
			'Frame': 0,
			'Topovaries': False,
			'Halfmode': 'off',
			'Fps': self._fps(),
			'Starttime': 0,
			'Endtime': 0,
		}
		for name, value in defaults.items():
			par = getattr(native.par, name, None)
			if par is None:
				par = getattr(native.par, name.lower(), None)
			if par is not None:
				par.val = value

	def _runNativeSopCommand(self, state, command, frame=None):
		native = self._nativeOp(MODE_SOP_NATIVE, required=True)
		state['nativeSeq'] = int(state.get('nativeSeq', 0)) + 1
		for key, value in self._nativeSopEnv(state, command, frame=frame).items():
			os.environ[key] = str(value)
		try:
			try:
				native.cook(force=True)
			except TypeError:
				native.cook()
		finally:
			os.environ['TD_SOP_USD_WRITER_COMMAND'] = 'idle'
		return self._readNativeStatus(state['nativeStatusPath'], MODE_SOP_NATIVE)

	def _resetNative(self, state):
		self._requireNativeReady(state['nativeMode'])
		return self._runNativeCommand(state, 'reset',
			frame=state['frames'][0])

	def _appendNativeFrame(self, state, outputFrame):
		return self._runNativeCommand(state, 'append',
			frame=outputFrame)

	def _finishNative(self, state):
		status = self._runNativeCommand(state, 'finish',
			frame=state['frames'][-1])
		manifest = status.get('manifest') or state.get('manifestPath')
		if not manifest or not os.path.isfile(manifest):
			raise RuntimeError('Native %s writer did not produce manifest.json'
				% self._nativeModeLabel(state['nativeMode']))
		return manifest

	def _exportNativeStatic(self, mode, fmt, finalPath):
		tmpdir = self._makeTempDir('native_export_')
		frame = int(round(self.ownerComp.time.frame))
		state = {
			'nativeMode': mode,
			'tmpdir': tmpdir,
			'nativeStatusPath': os.path.join(tmpdir, 'native_status.json'),
			'manifestPath': os.path.join(tmpdir, 'manifest.json'),
			'nativeSeq': 0,
			'frames': [frame],
			'fps': self._fps(),
			'topoVaries': self._parBool('Topologychanges'),
		}
		try:
			self._setProgress(0)
			self._setExportStatus('Exporting native %s'
				% self._nativeModeLabel(mode), log=True)
			self._resetNative(state)
			self._appendNativeFrame(state, frame)
			manifest = self._finishNative(state)
			self._ensureFolder(finalPath)
			self._setExportStatus('Assembling %s' % fmt)
			self._buildUsdcFromChunks(manifest, finalPath)
			self._setProgress(1)
			self._setExportStatus('Done')
			print('[TD-SOP-USD-Anim-Bridge] wrote native %s %s -> %s'
				% (self._nativeModeLabel(mode), fmt, finalPath))
			return finalPath
		except Exception as e:
			self._setExportStatus('Failed: %s' % e, log=True)
			raise
		finally:
			if mode == MODE_SOP_NATIVE:
				self._clearNativeSopEnv()
			self._clearNativeCommandPars(mode)
			shutil.rmtree(tmpdir, ignore_errors=True)

	def _transcode(self, src, dst):
		py = self._usdPython()
		script = os.path.join(project.folder, 'tools', 'transcode_usd.py')
		if not os.path.isfile(script):
			raise RuntimeError('USD transcode script not found: %s' % script)
		result = subprocess.run([py, script, src, dst],
			capture_output=True, text=True)
		if result.returncode:
			detail = (result.stderr or result.stdout or '').strip()
			if not detail:
				detail = 'exit code %d' % result.returncode
			raise RuntimeError('USD transcode failed: %s' % detail)
		msg = result.stdout.strip()
		if msg:
			print('[TD-SOP-USD-Anim-Bridge] transcode: %s' % msg)

	def _buildUsdcFromChunks(self, manifestPath, dst):
		py = self._usdPython()
		script = os.path.join(project.folder, 'tools',
			'build_usdc_from_chunks.py')
		if not os.path.isfile(script):
			raise RuntimeError('USD chunk builder script not found: %s' % script)
		result = subprocess.run([py, script, manifestPath, dst],
			capture_output=True, text=True)
		if result.returncode:
			detail = (result.stderr or result.stdout or '').strip()
			if not detail:
				detail = 'exit code %d' % result.returncode
			raise RuntimeError('USD binary chunk build failed: %s' % detail)
		msg = result.stdout.strip()
		if msg:
			print('[TD-SOP-USD-Anim-Bridge] chunk build: %s' % msg)

	def _fps(self):
		par = getattr(self.ownerComp.par, 'Outputfps', None)
		fps = float(par.eval()) if par is not None else float(self.ownerComp.time.rate)
		if fps <= 0:
			raise RuntimeError('Output FPS must be greater than zero')
		return fps

	def _frameStep(self):
		par = getattr(self.ownerComp.par, 'Framestep', None)
		step = int(round(par.eval())) if par is not None else 1
		if step < 1:
			raise RuntimeError('Frame Step must be 1 or greater')
		return step

	def _frameRange(self):
		start = int(self.ownerComp.par.Framestart)
		end = int(self.ownerComp.par.Frameend)
		if end < start:
			start, end = end, start
		return list(range(start, end + 1))

	def _frameBounds(self):
		start = int(self.ownerComp.par.Framestart)
		end = int(self.ownerComp.par.Frameend)
		if end < start:
			start, end = end, start
		return start, end

	def _writeFrames(self):
		start, end = self._frameBounds()
		return list(range(start, end + 1, self._frameStep()))

	def _outputFrames(self, count):
		start, _ = self._frameBounds()
		return list(range(start, start + count))

	def _playbackStartFrame(self, frameStart):
		par = getattr(self.ownerComp.par, 'Playbackstart', None)
		start = int(par.eval()) if par is not None else frameStart
		if start > frameStart:
			raise RuntimeError(
				'Playback Start must be less than or equal to Frame Start')
		return start

	def _execPlayback(self, required=True):
		dat = self.ownerComp.op('exec_playback')
		if dat is None and required:
			raise RuntimeError('exec_playback DAT not found inside the module')
		return dat

	def _setProgress(self, value):
		par = getattr(self.ownerComp.par, 'Progress', None)
		if par is not None:
			par.val = max(0.0, min(1.0, float(value)))

	def _playbackActive(self):
		state = getattr(self, '_playbackExport', None)
		if state and state.get('active'):
			return True
		stored = self.ownerComp.fetch('_tdsopusd_playback_export', None)
		return bool(stored and stored.get('active'))

	def _clearOrphanPlaybackExport(self, stored):
		"""Clear a stored active flag left behind after extension reinitialization."""
		execDat = self._execPlayback(required=False)
		if execDat is not None:
			try:
				execDat.par.active = False
			except Exception:
				pass
		self._setTimeMember(self.ownerComp.time, 'play', False)
		self._playbackExport = None
		self.ownerComp.unstore('_tdsopusd_playback_export')
		written = stored.get('written', 0) if isinstance(stored, dict) else 0
		frameCount = stored.get('frameCount', 0) if isinstance(stored, dict) else 0
		self._setExportStatus(
			'Cancelled orphaned export state (%d/%d frames written)'
			% (written, frameCount),
			log=True)
		self.RefreshUiState()

	def _setParsEnabled(self, names, enabled):
		for name in names:
			par = getattr(self.ownerComp.par, name, None)
			if par is not None:
				par.enable = bool(enabled)

	def _setParsReadOnly(self, names, readOnly):
		for name in names:
			par = getattr(self.ownerComp.par, name, None)
			if par is not None:
				par.readOnly = bool(readOnly)

	def _setExportStatus(self, msg, log=False):
		par = getattr(self.ownerComp.par, 'Exportstatus', None)
		if par is None:
			par = getattr(self.ownerComp.par, 'Binarystatus', None)
		if par is not None:
			par.val = msg
		if log:
			print('[TD-SOP-USD-Anim-Bridge] %s' % msg)

	def _timeSnapshot(self):
		tc = self.ownerComp.time
		prev = {'realTime': project.realTime}
		for name in ('play', 'frame', 'loop', 'start', 'end',
			'rangeStart', 'rangeEnd'):
			try:
				prev[name] = getattr(tc, name)
			except Exception:
				pass
		return prev

	def _setTimeMember(self, tc, name, value):
		try:
			setattr(tc, name, value)
		except Exception:
			pass

	def _setPlaybackWindow(self, tc, playbackStart, frameStart, frameEnd):
		"""Set TD playback and visible working ranges for animated export."""
		self._setTimeMember(tc, 'loop', False)
		self._setTimeMember(tc, 'start', playbackStart)
		self._setTimeMember(tc, 'end', frameEnd)
		self._setTimeMember(tc, 'rangeStart', frameStart)
		self._setTimeMember(tc, 'rangeEnd', frameEnd)

	def _publishPlaybackState(self, state):
		self.ownerComp.store('_tdsopusd_playback_export', {
			'active': bool(state.get('active')),
			'path': state.get('finalPath', ''),
			'frameStart': state.get('frameStart'),
			'frameEnd': state.get('frameEnd'),
			'frameCount': state.get('frameCount'),
			'frameStep': state.get('frameStep'),
			'outputFps': state.get('fps'),
			'mode': state.get('nativeMode') or MODE_SOP_PYTHON,
			'written': state.get('written', 0),
		})

	def _initPlaybackSections(self, state, ir):
		state['isMesh'] = ir['isMesh']
		state['firstSchema'] = self._schema(ir)
		state['firstTopo'] = self._topoKey(ir)
		if state.get('binary'):
			state['sections'] = self._buildBinarySections(
				ir, state['isMesh'], state['topoVaries'], state['tmpdir'])
		else:
			state['sections'] = self._buildSections(
				ir, state['isMesh'], state['topoVaries'], state['tmpdir'])

	def _streamPlaybackFrameStep(self, state, ir, sourceFrame, first,
			outputFrame=None):
		if state.get('binary'):
			self._streamBinaryFrameStep(state['sections'], ir,
				sourceFrame, first, outputFrame)
		else:
			self._streamFrameStep(state['sections'], ir,
				sourceFrame, first, outputFrame)

	def _flushPendingEmptyFrames(self, state):
		pending = state.get('pendingEmptyFrames') or []
		if not pending:
			return
		for sourceFrame, outputFrame, pendingIr in pending:
			ir = self._emptyIrForSchema(pendingIr, state['firstSchema'])
			if not state['topoVaries'] and self._topoKey(ir) != state['firstTopo']:
				raise RuntimeError(
					'Topology changes at frame %d but "Topology Changes" '
					'is off. Enable it and re-export.' % sourceFrame)
			self._streamPlaybackFrameStep(state, ir, sourceFrame,
				state['streamed'] == 0, outputFrame)
			state['streamed'] += 1
		state['pendingEmptyFrames'] = []

	def _startPlaybackExport(self, fmt, finalPath, usdaPath, tmpUsda,
			nativeMode=None):
		if self._playbackExport and self._playbackExport.get('active'):
			raise RuntimeError('An animated export is already active')

		sourceFrames = self._writeFrames()
		frameStart, frameEnd = self._frameBounds()
		frames = self._outputFrames(len(sourceFrames))
		playbackStart = self._playbackStartFrame(frameStart)
		execDat = self._execPlayback()
		tmpdir = self._makeTempDir('export_')
		tc = self.ownerComp.time
		binary = fmt == 'usdc'
		native = nativeMode is not None
		state = {
			'active': True,
			'fmt': fmt,
			'binary': binary,
			'nativeMode': nativeMode,
			'finalPath': finalPath,
			'usdaPath': usdaPath,
			'tmpUsda': tmpUsda,
			'tmpdir': tmpdir,
			'manifestPath': os.path.join(tmpdir, 'manifest.json')
				if (binary or native) else '',
			'nativeStatusPath': os.path.join(tmpdir, 'native_status.json')
				if native else '',
			'nativeSeq': 0,
			'frames': frames,
			'sourceFrames': sourceFrames,
			'frameCount': len(sourceFrames),
			'frameStart': frameStart,
			'frameEnd': frameEnd,
			'playbackStart': playbackStart,
			'fps': self._fps(),
			'frameStep': self._frameStep(),
			'topoVaries': self._parBool('Topologychanges'),
			'sections': None,
			'isMesh': None,
			'firstSchema': None,
			'firstTopo': None,
			'written': 0,
			'streamed': 0,
			'writtenFrames': set(),
			'pendingEmptyFrames': [],
			'prev': self._timeSnapshot(),
		}

		try:
			self._playbackExport = state
			self._publishPlaybackState(state)
			self.RefreshUiState()
			self._setProgress(0)
			self._setExportStatus('Exporting', log=True)
			if nativeMode in (MODE_SOP_NATIVE, MODE_POP_NATIVE):
				self._setExportStatus('Exporting native %s'
					% self._nativeModeLabel(nativeMode), log=True)
				self._resetNative(state)

			execDat.par.active = False
			tc.play = False
			project.realTime = False
			self._setPlaybackWindow(tc, playbackStart, frameStart, frameEnd)
			tc.frame = playbackStart
			execDat.par.active = True
			# Capture the starting frame deterministically. Later callbacks guard
			# against duplicates if TD also reports this frame on playback start.
			self.PlaybackFrame(playbackStart)
			state = self._playbackExport
			if state and state.get('active'):
				tc.play = True
			return finalPath
		except Exception:
			state['active'] = False
			try:
				execDat.par.active = False
			except Exception:
				pass
			if nativeMode == MODE_SOP_NATIVE:
				self._clearNativeSopEnv()
			self._restorePlaybackState(state)
			self._cleanupPlaybackTemp(state, removeTmpUsda=False)
			self._playbackExport = None
			self.ownerComp.unstore('_tdsopusd_playback_export')
			self.RefreshUiState()
			raise

	def _restorePlaybackState(self, state):
		tc = self.ownerComp.time
		prev = state.get('prev', {})
		execDat = self._execPlayback(required=False)
		if execDat is not None:
			try:
				execDat.par.active = False
			except Exception:
				pass
		self._setTimeMember(tc, 'play', False)
		for name in ('loop', 'start', 'end', 'rangeStart', 'rangeEnd', 'frame'):
			if name in prev:
				self._setTimeMember(tc, name, prev[name])
		if 'realTime' in prev:
			project.realTime = prev['realTime']
		if 'play' in prev:
			self._setTimeMember(tc, 'play', prev['play'])

	def _closePlaybackSections(self, state):
		sections = state.get('sections')
		if not sections:
			return
		for s in sections:
			if s.get('file') is not None and not s['file'].closed:
				s['file'].close()

	def _cleanupPlaybackTemp(self, state, removeTmpUsda=True):
		if removeTmpUsda and state.get('tmpUsda'):
			try:
				if os.path.exists(state['tmpUsda']):
					os.remove(state['tmpUsda'])
			except OSError:
				pass
		if state.get('tmpdir'):
			shutil.rmtree(state['tmpdir'], ignore_errors=True)

	def _finishPlaybackExport(self, cancelled=False, error=None):
		state = self._playbackExport
		if not state:
			return
		state['active'] = False
		self._publishPlaybackState(state)
		self._setTimeMember(self.ownerComp.time, 'play', False)
		if self._execPlayback(required=False) is not None:
			self._execPlayback(required=False).par.active = False
		try:
			self._closePlaybackSections(state)
			if error is not None:
				self._setExportStatus('Failed: %s' % error, log=True)
				return
			if cancelled:
				self._setExportStatus('Cancelled', log=True)
				return
			if not state.get('sections') or state.get('written') != state['frameCount']:
				if state.get('nativeMode') is None:
					raise RuntimeError(
						'Playback export ended before all frames were written')
			if state.get('nativeMode') in (MODE_SOP_NATIVE, MODE_POP_NATIVE):
				if state.get('written') != state['frameCount']:
					raise RuntimeError(
						'Native playback export ended before all frames were written')
				self._setExportStatus('Assembling %s' % state['fmt'])
				manifestPath = self._finishNative(state)
				self._buildUsdcFromChunks(manifestPath, state['finalPath'])
			elif state.get('binary'):
				self._setExportStatus('Assembling usdc')
				manifestPath = self._writeBinaryManifest(state)
				self._buildUsdcFromChunks(manifestPath, state['finalPath'])
			else:
				self._ensureFolder(state['usdaPath'])
				self._assemble(state['usdaPath'], state['sections'],
					state['isMesh'], state['frames'], state['fps'])
				if state['fmt'] == 'usdc':
					self._transcode(state['usdaPath'], state['finalPath'])
			self._setProgress(1)
			self._setExportStatus('Done')
			print('[TD-SOP-USD-Anim-Bridge] wrote %d-frame %s -> %s'
				% (state['frameCount'], state['fmt'], state['finalPath']))
		except Exception as e:
			self._setExportStatus('Failed: %s' % e, log=True)
		finally:
			self._restorePlaybackState(state)
			if state.get('nativeMode') == MODE_SOP_NATIVE:
				self._clearNativeSopEnv()
			if state.get('nativeMode') in (MODE_SOP_NATIVE, MODE_POP_NATIVE):
				self._clearNativeCommandPars(state['nativeMode'])
			self._cleanupPlaybackTemp(state)
			self._playbackExport = None
			self.ownerComp.unstore('_tdsopusd_playback_export')
			self.RefreshUiState()

	# ---- sampling ---------------------------------------------------

	def _readAttribs(self, elements, attribDefs):
		"""Read every attribute generically (no per-name hardcoding).

		Each value is an AttributeData object: indexable by component
		but not iterable, so we pull components by index.
		"""
		out = {}
		for a in attribDefs:
			name, size = a.name, a.size
			values = []
			for e in elements:
				data = getattr(e, name)
				values.append(tuple(data[k] for k in range(size)))
			out[name] = {'size': size, 'values': values}
		return out

	def _isMeshGeo(self, sop):
		"""True for Poly/Mesh prims; False for point/particle geometry.

		A particle system (or any point-only primitive) reports prims of
		class 'Prim' wrapping the points - that is a point cloud, not a
		mesh, so it routes to the UsdGeomPoints path.
		"""
		if sop.numPrims == 0:
			return False
		classes = set(type(pr).__name__ for pr in sop.prims)
		if classes <= {'Poly', 'Mesh'}:
			return True
		if classes <= {'Prim', 'Part', 'Particle'}:
			return False
		raise RuntimeError(
			'Unsupported prim type(s) %s. Use Polygon/Mesh for a mesh or a '
			'particle/point system for points; convert others with a '
			'Convert SOP first.' % sorted(classes))

	def _sampleSop(self, sop):
		points = sop.points
		isMesh = self._isMeshGeo(sop)
		ir = {
			'numPoints': sop.numPoints,
			'numPrims': sop.numPrims,
			'numVertices': sop.numVertices,
			'isMesh': isMesh,
			'P': [(p.x, p.y, p.z) for p in points],
			'pointAttribs': self._readAttribs(points, sop.pointAttribs),
			'vertexAttribs': {},
			'primAttribs': {},
		}
		if isMesh:
			# faceData: list of (sourcePrim, [Vertex, ...]) in output order.
			# Poly prims pass through; Mesh prims are tessellated into quads.
			faceData = self._faces(sop)
			verts = [v for _, face in faceData for v in face]
			ir['faceVertexCounts'] = [len(face) for _, face in faceData]
			ir['faceVertexIndices'] = [v.point.index for v in verts]
			ir['vertexAttribs'] = self._readAttribs(verts, sop.vertexAttribs)
			# Prim attributes are uniform (per output face): replicate the
			# source prim's value for each face it produced.
			primElems = [pr for pr, _ in faceData]
			ir['primAttribs'] = self._readAttribs(primElems, sop.primAttribs)
		return ir

	def _isEmptyIr(self, ir):
		return (ir['numPoints'] == 0 and ir['numPrims'] == 0
			and ir['numVertices'] == 0)

	def _emptyIrForSchema(self, ir, schema):
		"""Return an empty-frame IR coerced to an already chosen USD schema.

		An empty SOP has no primitive classes, so it cannot prove whether the
		source is a mesh or point cloud. Animated export must keep the USD prim
		type stable and treat that as varying topology/counts, not a schema
		change.
		"""
		out = dict(ir)
		out['numPoints'] = 0
		out['numPrims'] = 0
		out['numVertices'] = 0
		out['P'] = []
		out['isMesh'] = schema[0]
		byClass = dict((klass, attrs) for klass, attrs in schema[1:])
		for klass, key in (('point', 'pointAttribs'),
				('vertex', 'vertexAttribs'), ('prim', 'primAttribs')):
			out[key] = dict((name, {'size': size, 'values': []})
				for name, size in byClass.get(klass, ()))
		if out['isMesh']:
			out['faceVertexCounts'] = []
			out['faceVertexIndices'] = []
		else:
			out.pop('faceVertexCounts', None)
			out.pop('faceVertexIndices', None)
		return out

	def _faces(self, sop):
		"""Resolve every prim to one or more polygon faces.

		Returns a list of (sourcePrim, [Vertex, ...]). Poly prims are a
		single face; Mesh prims are tessellated from their row/column grid
		into quads (triangles at poles where the grid collapses).
		"""
		out = []
		for pr in sop.prims:
			cls = type(pr).__name__
			if cls == 'Poly':
				out.append((pr, [v for v in pr]))
			elif cls == 'Mesh':
				for face in self._meshFaces(pr):
					out.append((pr, face))
			else:
				raise RuntimeError(
					'Unsupported prim type %s. Convert to Polygon, or feed a '
					'Mesh; NURBS/Bezier/etc. need a Convert SOP first.' % cls)
		return out

	def _meshFaces(self, pr):
		"""Tessellate a Mesh grid prim into a list of face vertex rings."""
		R, C = pr.numRows, pr.numCols
		rmax = R if pr.closedV else R - 1
		cmax = C if pr.closedU else C - 1
		faces = []
		for r in range(rmax):
			r1 = (r + 1) % R
			for c in range(cmax):
				c1 = (c + 1) % C
				ring = self._dedupeRing(
					[pr[r, c], pr[r, c1], pr[r1, c1], pr[r1, c]])
				if len(ring) >= 3:
					faces.append(ring)
		return faces

	def _dedupeRing(self, verts):
		"""Drop consecutive (and wrap-around) duplicate points.

		Collapses a degenerate pole quad (two corners share a point) into
		a triangle; fully degenerate rings shrink below 3 and are dropped.
		"""
		out = []
		for v in verts:
			if not out or out[-1].point.index != v.point.index:
				out.append(v)
		if len(out) > 2 and out[0].point.index == out[-1].point.index:
			out.pop()
		return out

	def _schema(self, ir):
		"""A hashable signature of geometry kind + attribute set."""
		return (ir['isMesh'],) + tuple(
			(klass, tuple(sorted((n, a['size'])
				for n, a in ir.get(key, {}).items())))
			for klass, key in (('point', 'pointAttribs'),
				('vertex', 'vertexAttribs'), ('prim', 'primAttribs')))

	def _topoKey(self, ir):
		"""Topology signature used to police the constant-topology toggle."""
		if ir['isMesh']:
			return (ir['faceVertexCounts'], ir['faceVertexIndices'])
		return ir['numPoints']

	# ---- USDA serialization ----------------------------------------

	def _usdScalarType(self, size):
		return {1: 'float', 2: 'float2', 3: 'float3', 4: 'float4'}.get(
			size, 'float%d' % size)

	def _resolveAttr(self, name, size, klass):
		"""Map a TD attribute to USD.

		Returns (type, decl, interpolation, comps, halfEligible).
		comps selects components (e.g. take u, v from a 3D uv); None
		keeps all. Interpolation follows the TD attribute class:
		point -> vertex, vertex -> faceVarying, prim -> uniform.
		"""
		interp = {'point': 'vertex', 'vertex': 'faceVarying',
			'prim': 'uniform'}[klass]
		if klass == 'point' and name == 'N' and size == 3:
			return 'normal3f', 'normals', 'vertex', None, True
		if klass == 'point' and name in ('v', 'PartVel') and size == 3:
			return 'vector3f', 'velocities', None, None, True
		if klass == 'vertex' and name == 'uv' and size >= 2:
			return 'texCoord2f', 'primvars:st', 'faceVarying', [0, 1], True
		if name == 'Cd' and size == 3:
			return 'color3f', 'primvars:Cd', interp, None, True
		return self._usdScalarType(size), 'primvars:%s' % name, interp, None, False

	def _sliceVals(self, values, comps):
		if comps is None:
			return values
		return [tuple(v[i] for i in comps) for v in values]

	def _bbox(self, points):
		if not points:
			return [(0, 0, 0), (0, 0, 0)]
		xs = [p[0] for p in points]
		ys = [p[1] for p in points]
		zs = [p[2] for p in points]
		return [(min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))]

	def _defaultLines(self, prefix, interp, value):
		if interp:
			return ['%s = %s (' % (prefix, value),
				'        interpolation = "%s"' % interp, '    )']
		return ['%s = %s' % (prefix, value)]

	def _fmtVec(self, vec):
		if len(vec) == 1:
			return '%.6g' % vec[0]
		return '(' + ', '.join('%.6g' % c for c in vec) + ')'

	def _fmtArray(self, values):
		return '[' + ', '.join(self._fmtVec(v) for v in values) + ']'

	def _fmtIntArray(self, values):
		return '[' + ', '.join(str(int(v)) for v in values) + ']'

	def _header(self, extra=None):
		lines = ['#usda 1.0', '(', '    defaultPrim = "Exported"',
			'    metersPerUnit = 1', '    upAxis = "Y"']
		if extra:
			lines.extend(extra)
		lines.append(')')
		lines.append('')
		return lines

	# ---- static (single-frame) writer ------------------------------

	def _emitAttr(self, lines, usdType, decl, interp, static, fmt=None):
		prefix = '    %s[] %s' % (usdType, decl)
		value = (fmt or self._fmtArray)(static)
		lines.extend(self._defaultLines(prefix, interp, value))

	def _build(self, ir):
		"""Build a static (single-frame) .usda in memory (one frame fits)."""
		isMesh = ir['isMesh']
		lines = self._header()
		lines.append('def %s "Exported"' % ('Mesh' if isMesh else 'Points'))
		lines.append('{')
		if isMesh:
			lines.append('    int[] faceVertexCounts = %s'
				% self._fmtIntArray(ir['faceVertexCounts']))
			lines.append('    int[] faceVertexIndices = %s'
				% self._fmtIntArray(ir['faceVertexIndices']))
			lines.append('    uniform token subdivisionScheme = "none"')
		self._emitAttr(lines, 'float3', 'extent', None, self._bbox(ir['P']))
		self._emitAttr(lines, self._maybeHalf('point3f', False),
			'points', None, ir['P'])
		if not isMesh:
			self._emitAttr(lines, self._maybeHalf('float', False),
				'widths', 'constant', [(0.05,)])
		for klass, key in (('point', 'pointAttribs'),
				('vertex', 'vertexAttribs'), ('prim', 'primAttribs')):
			for name, attr in ir.get(key, {}).items():
				if not isMesh and klass == 'point' and name in ('id', 'ids'):
					vals = [int(round(v[0])) for v in attr['values']]
					lines.append('    int64[] ids = %s' % self._fmtIntArray(vals))
					continue
				usdType, decl, interp, comps, eligible = self._resolveAttr(
					name, attr['size'], klass)
				usdType = self._maybeHalf(usdType, eligible)
				self._emitAttr(lines, usdType, decl, interp,
					self._sliceVals(attr['values'], comps))
		lines.append('}')
		lines.append('')
		return '\n'.join(lines)

	# ---- streaming (animated) writer -------------------------------

	def _attrGetter(self, key, name, comps):
		def get(ir):
			return self._sliceVals(ir[key][name]['values'], comps)
		return get

	def _idsGetter(self, key, name):
		def get(ir):
			return [int(round(v[0])) for v in ir[key][name]['values']]
		return get

	def _binaryUsdTypes(self):
		return set(('float', 'float2', 'float3', 'float4',
			'half', 'half2', 'half3', 'half4',
			'point3f', 'point3h', 'normal3f', 'normal3h',
			'texCoord2f', 'texCoord2h', 'vector3f', 'vector3h',
			'color3f', 'color3h', 'int', 'int64'))

	def _binaryStorage(self, usdType):
		prefix = '<' if sys.byteorder == 'little' else '>'
		if usdType == 'int64':
			return prefix + 'i8'
		if usdType == 'int':
			return prefix + 'i4'
		return prefix + 'f4'

	def _checkBinaryUsdType(self, usdType):
		if usdType not in self._binaryUsdTypes():
			raise RuntimeError(
				'Animated .usdc binary fast path does not support USD array '
				'type %s. Export .usda or remove/convert that attribute.'
				% usdType)

	def _binaryPrimvarName(self, decl):
		return decl[len('primvars:'):] if decl.startswith('primvars:') else ''

	def _binaryArray(self, values, storage, tupleSize):
		base = storage[1:]
		typecode = {'f4': 'f', 'i4': 'i', 'i8': 'q'}[base]
		out = array(typecode)
		if out.itemsize != int(base[1:]):
			raise RuntimeError('Unexpected Python array itemsize for %s' % storage)
		if tupleSize == 1:
			if base == 'f4':
				for v in values:
					if isinstance(v, (tuple, list)):
						v = v[0]
					out.append(float(v))
			else:
				for v in values:
					if isinstance(v, (tuple, list)):
						v = v[0]
					out.append(int(v))
		else:
			for v in values:
				for i in range(tupleSize):
					out.append(float(v[i]))
		return out

	def _writeBinaryValues(self, f, values, storage, tupleSize):
		offset = f.tell()
		data = self._binaryArray(values, storage, tupleSize)
		if data:
			data.tofile(f)
		return {'offset': offset, 'count': len(data) // int(tupleSize)}

	def _binarySampler(self, idx, tmpdir, name, target, usdType, interp,
			tupleSize, get, primvarName=''):
		self._checkBinaryUsdType(usdType)
		path = os.path.join(tmpdir, 'b%d.bin' % idx[0])
		idx[0] += 1
		return {
			'type': 'binary_sampler',
			'name': name,
			'target': target,
			'primvarName': primvarName,
			'usdType': usdType,
			'interpolation': interp,
			'tupleSize': int(tupleSize),
			'storage': self._binaryStorage(usdType),
			'path': path,
			'file': open(path, 'wb'),
			'get': get,
			'samples': [],
		}

	def _binaryConst(self, idx, tmpdir, name, target, usdType, interp,
			tupleSize, values, primvarName=''):
		self._checkBinaryUsdType(usdType)
		path = os.path.join(tmpdir, 'b%d.bin' % idx[0])
		idx[0] += 1
		storage = self._binaryStorage(usdType)
		with open(path, 'wb') as f:
			sample = self._writeBinaryValues(f, values, storage, tupleSize)
		return {
			'type': 'binary_const',
			'name': name,
			'target': target,
			'primvarName': primvarName,
			'usdType': usdType,
			'interpolation': interp,
			'tupleSize': int(tupleSize),
			'storage': storage,
			'path': path,
			'file': None,
			'sample': sample,
		}

	def _buildBinarySections(self, ir, isMesh, topoVaries, tmpdir):
		"""Plan animated .usdc output as binary chunk sections."""
		sections = []
		idx = [0]

		if isMesh:
			if topoVaries:
				sections.append(self._binarySampler(idx, tmpdir,
					'faceVertexCounts', 'faceVertexCounts', 'int', None, 1,
					lambda ir: ir['faceVertexCounts']))
				sections.append(self._binarySampler(idx, tmpdir,
					'faceVertexIndices', 'faceVertexIndices', 'int', None, 1,
					lambda ir: ir['faceVertexIndices']))
			else:
				sections.append(self._binaryConst(idx, tmpdir,
					'faceVertexCounts', 'faceVertexCounts', 'int', None, 1,
					ir['faceVertexCounts']))
				sections.append(self._binaryConst(idx, tmpdir,
					'faceVertexIndices', 'faceVertexIndices', 'int', None, 1,
					ir['faceVertexIndices']))
		sections.append(self._binarySampler(idx, tmpdir,
			'extent', 'extent', 'float3', None, 3,
			lambda ir: self._bbox(ir['P'])))
		sections.append(self._binarySampler(idx, tmpdir,
			'points', 'points', self._maybeHalf('point3f', False), None, 3,
			lambda ir: ir['P']))
		if not isMesh:
			sections.append(self._binaryConst(idx, tmpdir,
				'widths', 'widths', self._maybeHalf('float', False),
				'constant', 1, [(0.05,)]))
		for klass, key in (('point', 'pointAttribs'),
				('vertex', 'vertexAttribs'), ('prim', 'primAttribs')):
			for name, attr in ir.get(key, {}).items():
				if not isMesh and klass == 'point' and name in ('id', 'ids'):
					sections.append(self._binarySampler(idx, tmpdir,
						'ids', 'ids', 'int64', None, 1,
						self._idsGetter(key, name)))
					continue
				usdType, decl, interp, comps, eligible = self._resolveAttr(
					name, attr['size'], klass)
				usdType = self._maybeHalf(usdType, eligible)
				target = 'primvar' if decl.startswith('primvars:') else decl
				tupleSize = len(comps) if comps is not None else attr['size']
				sections.append(self._binarySampler(idx, tmpdir,
					decl, target, usdType, interp, tupleSize,
					self._attrGetter(key, name, comps),
					self._binaryPrimvarName(decl)))
		return sections

	def _streamBinaryFrameStep(self, sections, ir, sourceFrame, first,
			outputFrame=None):
		"""Append one sampled frame to every binary animated section."""
		frame = sourceFrame if outputFrame is None else outputFrame
		for s in sections:
			if s['type'] != 'binary_sampler':
				continue
			sample = self._writeBinaryValues(
				s['file'], s['get'](ir), s['storage'], s['tupleSize'])
			sample['time'] = frame
			s['samples'].append(sample)

	def _writeBinaryManifest(self, state):
		sections = []
		for s in state['sections']:
			if s['type'] not in ('binary_sampler', 'binary_const'):
				continue
			item = {
				'kind': 'sampler' if s['type'] == 'binary_sampler' else 'const',
				'name': s['name'],
				'target': s['target'],
				'primvarName': s.get('primvarName', ''),
				'usdType': s['usdType'],
				'interpolation': s['interpolation'],
				'tupleSize': s['tupleSize'],
				'storage': s['storage'],
				'path': s['path'],
			}
			if s['type'] == 'binary_sampler':
				item['samples'] = s['samples']
			else:
				item['sample'] = s['sample']
			sections.append(item)
		manifest = {
			'version': 1,
			'stage': {
				'isMesh': bool(state['isMesh']),
				'framesPerSecond': state['fps'],
				'timeCodesPerSecond': state['fps'],
				'startTimeCode': state['frames'][0],
				'endTimeCode': state['frames'][-1],
				'metersPerUnit': 1,
				'upAxis': 'Y',
			},
			'sections': sections,
		}
		path = state['manifestPath']
		with open(path, 'w', encoding='utf-8') as f:
			json.dump(manifest, f, indent=2)
		return path

	def _buildSections(self, ir, isMesh, topoVaries, tmpdir):
		"""Plan the output as ordered sections.

		Each section is either a 'const' block of literal lines, or a
		'sampler' that owns a temp file receiving one '<frame>: <array>,'
		line per frame, later streamed into its timeSamples block.
		"""
		sections = []
		idx = [0]

		def sampler(decl, interp, get, fmt):
			path = os.path.join(tmpdir, 'c%d.txt' % idx[0])
			idx[0] += 1
			return {'type': 'sampler', 'decl': decl, 'interp': interp,
				'get': get, 'fmt': fmt, 'tmppath': path,
				'file': open(path, 'w', encoding='utf-8'),
				'const': True, 'first': None}

		if isMesh:
			if topoVaries:
				sections.append(sampler('    int[] faceVertexCounts', None,
					lambda ir: ir['faceVertexCounts'], self._fmtIntArray))
				sections.append(sampler('    int[] faceVertexIndices', None,
					lambda ir: ir['faceVertexIndices'], self._fmtIntArray))
			else:
				sections.append({'type': 'const', 'lines': [
					'    int[] faceVertexCounts = %s'
						% self._fmtIntArray(ir['faceVertexCounts']),
					'    int[] faceVertexIndices = %s'
						% self._fmtIntArray(ir['faceVertexIndices'])]})
			sections.append({'type': 'const', 'lines': [
				'    uniform token subdivisionScheme = "none"']})
		sections.append(sampler('    float3[] extent', None,
			lambda ir: self._bbox(ir['P']), self._fmtArray))
		sections.append(sampler('    %s[] points'
				% self._maybeHalf('point3f', False), None,
			lambda ir: ir['P'], self._fmtArray))
		if not isMesh:
			sections.append({'type': 'const', 'lines': self._defaultLines(
				'    %s[] widths' % self._maybeHalf('float', False),
				'constant', self._fmtArray([(0.05,)]))})
		for klass, key in (('point', 'pointAttribs'),
				('vertex', 'vertexAttribs'), ('prim', 'primAttribs')):
			for name, attr in ir.get(key, {}).items():
				if not isMesh and klass == 'point' and name in ('id', 'ids'):
					sections.append(sampler('    int64[] ids', None,
						self._idsGetter(key, name), self._fmtIntArray))
					continue
				usdType, decl, interp, comps, eligible = self._resolveAttr(
					name, attr['size'], klass)
				usdType = self._maybeHalf(usdType, eligible)
				sections.append(sampler('    %s[] %s' % (usdType, decl),
					interp, self._attrGetter(key, name, comps),
					self._fmtArray))
		return sections

	def _streamFrameStep(self, sections, ir, sourceFrame, first, outputFrame=None):
		"""Append one sampled frame to every animated section."""
		frame = sourceFrame if outputFrame is None else outputFrame
		for s in sections:
			if s['type'] != 'sampler':
				continue
			value = s['fmt'](s['get'](ir))
			if first:
				s['first'] = value
			elif s['const'] and value != s['first']:
				s['const'] = False
			s['file'].write('        %d: %s,\n' % (frame, value))

	def _assemble(self, path, sections, isMesh, frames, fps):
		"""Stream the final .usda; temp files are copied in chunks."""
		extra = ['    framesPerSecond = %g' % fps,
			'    timeCodesPerSecond = %g' % fps,
			'    startTimeCode = %d' % frames[0],
			'    endTimeCode = %d' % frames[-1]]
		with open(path, 'w', encoding='utf-8') as out:
			out.write('\n'.join(self._header(extra)) + '\n')
			out.write('def %s "Exported"\n' % ('Mesh' if isMesh else 'Points'))
			out.write('{\n')
			for s in sections:
				if s['type'] == 'const':
					for ln in s['lines']:
						out.write(ln + '\n')
					continue
				prefix = s['decl']
				if s.get('const'):
					for ln in self._defaultLines(prefix, s['interp'], s['first']):
						out.write(ln + '\n')
					continue
				if s['interp']:
					out.write('%s (\n' % prefix)
					out.write('        interpolation = "%s"\n' % s['interp'])
					out.write('    )\n')
				else:
					out.write('%s\n' % prefix)
				out.write('%s.timeSamples = {\n' % prefix)
				with open(s['tmppath'], 'r', encoding='utf-8') as tf:
					shutil.copyfileobj(tf, out)
				out.write('    }\n')
			out.write('}\n')
