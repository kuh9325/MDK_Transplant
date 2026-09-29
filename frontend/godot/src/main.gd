extends Node3D

# Phase 9 (G3) acceptance scene: load LEVEL3, render the traversal
# DISPLAY SET (current + active partner arenas) through the
# MdkBridge GDExtension, present spawned DynamicObjects from real
# RuntimeModel geometry, and show the player as a debug proxy —
# all driven purely by traversal-runtime snapshots. mdk_core owns
# data/semantics; this script only wires returned snapshots into
# Node3D objects — nothing here integrates motion or drives state.
#
# Per frame:
#   raw device input -> bridge.step_frame_input -> core step
#   -> arena display digest -> ArenaRoot sync (display set)
#   -> object snapshots -> DynamicObjectRoot nodes (opaque ids)
#   -> player snapshot -> PlayerRoot transform (presentation only)
#
# CLI (after --):
#   --data-path DIR   data root (default: MDK_DATA_ROOT env, else
#                     <repo>/original/installed relative to res://)
#   --level RELDTI    default TRAVERSE/LEVEL3/LEVEL3.DTI
#   --arena NAME      default HMO_1 ("" -> spawn arena)
#   --start X Y Z     diagnostic re-anchor into --arena (native
#                     diagnostic — test/QA path, not original flow)
#   --start-yaw DEG   yaw for --start (default 0)
#   --freefall N      mode-2 freefall course 0..4 instead of --level
#                     (Phase 16C; hands off to traversal on landing,
#                     or the frontend route on death)
#   --skill N         difficulty 0..2 for --freefall (default 0)
#   --seed N          RNG seed for --freefall (default 0xC0FFEE —
#                     the mdk-inspect freefall digest seed)
#   --smoke           headless deterministic check, then quit
#   --save-restore    with --smoke: run the save->restore combat
#                     golden (LEVEL3/HMO_9) instead of the smoke
#   --combat-demo     scripted scoped-fire input for real-renderer
#                     runs (pairs with --frames / interactive)
#   --screenshot P    after 8 frames, save a PNG capture then quit
#                     (requires a real renderer — not --headless)
#   --frames N        run N process frames then quit (startup proof)
#
# Keys: WASD move, Q/E strafe, A/D turn, R/F look up/down, Space
# jump, Shift turbo, F1 collision wire toggle, F2 dynamic-object
# debug toggle (AABB wires + name/id/arena tags), F3 debug text.
# Interactive runs capture the mouse; Esc releases the capture once,
# then quits. Mouse deltas/buttons are forwarded to the core raw-
# input path — the configured W-set mapping (axes "ABG", scales
# 16/16/50, button masks 1/4/2/0) lives entirely in mdk_core.

# Variant on purpose: keeping the MdkBridge reference untyped lets
# the script still parse when the GDExtension is missing, so the
# failure surfaces as an actionable error instead of a parse abort.
var bridge = null
var last_display_digest := -1
var smoke := false
var interactive := false
var failures := 0
var shot_path := ""
var shot_frames_left := 0
var frames_left := 0
var obj_debug := false
var proxy_debug := false      # F4 — legacy capsule/wire proxy
var last_kurt := {}           # last applied kurt snapshot (diag)
var combat_demo := false      # --combat-demo: scripted scoped-fire
                              # input under the real renderer
var demo_frame := 0

# Phase 16C — mode-2 freefall presentation state. All gameplay lives
# in the core; these are view-side caches only.
var freefall := false        # --freefall launcher flag
var ff_materials := {}       # "m:<name>" / "pen:<n>" -> StandardMaterial3D
var ff_palette := PackedByteArray()  # FALLP_<c+1> bytes (768)
var ff_handoff_seen := false # printed the mode transition once

# Raw mouse accumulators — device deltas for the next frame only.
var mouse_dx := 0
var mouse_dy := 0
var mouse_dz := 0

# Phase 17A — traversal combat presentation state. mdk_core owns
# every gameplay fact; these are view-side node/resource caches only.
var combat_diag := false          # --combat-diag flag
var shot_nodes: Array = []        # [Node3D] per slot, under ShotRoot
var shot_geom := {}               # class_idx -> {key:int, meshes:Array}
var named_geom := {}              # model name -> {key:int, meshes:Array}
var shot_vps: Array = []          # [SubViewport] per slot
var shot_cams: Array = []         # [Camera3D] per slot
var shot_wins: Array = []         # [TextureRect] per slot (HUD windows)
var shot_fills: Array = []        # [ColorRect] per slot (HUD fills)
var fx_palette := PackedByteArray()  # get_active_palette() (768)
var shard_mats := {}              # palette pen -> StandardMaterial3D
var shard_tetra: Array = []       # jittered-tetra mesh bank
var fx_enable_live := false       # 0x54150c gate (per-frame)
var shards: Array = []            # live shard dicts {mi, vel, ttl}
var remnant_seq := 0              # remnant node counter
var remnants: Array = []          # live EXPLODE remnant Node3Ds
var fx_seq := 0                   # deterministic jitter counter (see
                                  #   _fx_rand — never touches core RNG)
var fx_recent: Array = []         # last drained events (diag, cap 16)
var fx_stats := {"events": 0, "shards": 0, "remnants": 0,
	"kinds": {}}                # --combat-diag counters

# Phase 17B.2 — traversal HUD / view presentation state. The core
# composes the 600x360 indexed framebuffer and owns every semantic;
# these are view-side node/resource caches only.
var last_hud := {}                # last applied HUD snapshot (diag)
var scope_vp: SubViewport = null  # mode-1 aperture viewport (384x280)
var scope_cam: Camera3D = null    # scope view camera (mirrors pose)

# The original 600x360 HUD window rects (0x49b900/0x49b8e8, OBSERVED).
const SHOT_HUD_RECT := [
	Rect2(72, 10, 140, 70),
	Rect2(228, 0, 140, 70),
	Rect2(384, 10, 140, 70),
]
# FUN_00437444's paletteMode table — select -> {pen, shadeByte,
# scale} (0x43745f..0x43751d, OBSERVED):
#   0  -> {pen 3 when 0x54150c set else 0xd, shade 3,    scale 1.0}
#   1  -> {pen 0x25,                           shade 0xf0, scale 0.5}
#   >=2-> {pen 10,                             shade 3,    scale 1.0}
# The select-0 gate needs the live 0x54150c — resolved per frame
# from the bridge's fx_enable, not folded into this const.
const SHARD_SEL1 := {"pen": 0x25, "shade": 0xf0, "scale": 0.5}
const SHARD_SEL2 := {"pen": 10, "shade": 3, "scale": 1.0}
const SHARD_SEL0_ON := {"pen": 3, "shade": 3, "scale": 1.0}
const SHARD_SEL0_OFF := {"pen": 0xd, "shade": 3, "scale": 1.0}
# The death-teardown burst (FUN_00457cf4 -> FUN_00404108, OBSERVED):
# 16 shards at pen 0x30, shade 0x10, scale 1.0, spawn pos = victim.
const TEAR_SHARDS := 16
const TEAR_PEN := 0x30
const TEAR_SHADE := 0x10
const SHARD_CAP := 512            # the original pool is bounded too
# FUN_00404108's tetra vertex directions (0x403dd0) in Godot axes —
# MDK (x,y,z) -> (-y, z, -x) already folded in.
const SHARD_DIR := [
	Vector3(0.0, 0.5, 0.0),
	Vector3(0.0, -0.5, -0.5),
	Vector3(-0.5, -0.5, 0.5),
	Vector3(0.5, -0.5, 0.5),
]
# Face indices verbatim (u16 triples at +0x96/+0xba/+0xde/+0x102).
const SHARD_FACES := [
	[0, 2, 1], [0, 3, 2], [0, 1, 3], [1, 2, 3],
]
# Bullet-cam projector: window divisors {69.95,34.95} over the 140x70
# rect -> tan(hfov/2)=70/69.95, tan(vfov/2)=35/34.95 — a ~90x90 deg
# frustum squashed into 2:1. We render a square viewport (equal
# h/v fov) and let the TextureRect squash it to the rect.
const SHOT_CAM_FOV := 90.08       # 2*atan(35/34.95) in degrees
const SHARD_TICKS_PER_SEC := 30.0 # the +0x196 countdown is frameStep
                                  # units — the 30Hz tick domain

# Presentation resources (built in _build_player_proxy).
var body_mat: StandardMaterial3D
var marker_mat: StandardMaterial3D
var wire_mat: StandardMaterial3D
var col_mat: StandardMaterial3D
var obj_wire_mat: StandardMaterial3D

# Dynamic-object presentation caches — geometry keyed by the core
# geom_key digest (immutable local geometry may be shared when the
# digest proves identity), materials keyed by element name.
var geom_cache := {}        # geom_key -> [{elem:int, mesh:ArrayMesh}]
var elem_mats := {}         # elem name -> StandardMaterial3D
var obj_wires := {}         # object id -> MeshInstance3D (AABB wire)
var obj_tags := {}          # object id -> Label3D

const ACT_TURN_LEFT := 1
const ACT_TURN_RIGHT := 2
const ACT_FORWARD := 4
const ACT_BACK := 8
const ACT_STRAFE_LEFT := 16
const ACT_STRAFE_RIGHT := 32
const ACT_JUMP := 64
const ACT_TURBO := 128
const ACT_LOOK_UP := 256
const ACT_LOOK_DOWN := 512

const COLOR_GROUNDED := Color(0.25, 0.9, 0.3)
const COLOR_AIRBORNE := Color(1.0, 0.55, 0.15)
# 12 edges of an AABB, as pairs of 0/1 corner selectors.
const BOX_EDGES := [
	[Vector3(0, 0, 0), Vector3(1, 0, 0)],
	[Vector3(1, 0, 0), Vector3(1, 0, 1)],
	[Vector3(1, 0, 1), Vector3(0, 0, 1)],
	[Vector3(0, 0, 1), Vector3(0, 0, 0)],
	[Vector3(0, 1, 0), Vector3(1, 1, 0)],
	[Vector3(1, 1, 0), Vector3(1, 1, 1)],
	[Vector3(1, 1, 1), Vector3(0, 1, 1)],
	[Vector3(0, 1, 1), Vector3(0, 1, 0)],
	[Vector3(0, 0, 0), Vector3(0, 1, 0)],
	[Vector3(1, 0, 0), Vector3(1, 1, 0)],
	[Vector3(1, 0, 1), Vector3(1, 1, 1)],
	[Vector3(0, 0, 1), Vector3(0, 1, 1)],
]


func _arg_value(args: PackedStringArray, name: String, fallback: String) -> String:
	var i := args.find(name)
	if i >= 0 and i + 1 < args.size():
		return args[i + 1]
	return fallback


func _check(cond: bool, label: String) -> void:
	if cond:
		print("  ok   ", label)
	else:
		failures += 1
		printerr("  FAIL ", label)


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	smoke = "--smoke" in args
	# Phase 17A diagnostics — combat-FX counters in the F3 label.
	# Observational only: never touches core state or RNG.
	combat_diag = "--combat-diag" in args
	# --combat-demo: drive a scoped shot through the live input path
	# (MMB edge, then held LMB) so a real-renderer run exercises the
	# combat presentation without a harness.
	combat_demo = "--combat-demo" in args
	var data_root := _arg_value(args, "--data-path",
		OS.get_environment("MDK_DATA_ROOT"))
	if data_root.is_empty():
		data_root = ProjectSettings.globalize_path(
			"res://../../original/installed")
	elif data_root.is_relative_path():
		# Godot chdir's into the project dir; resolve user paths
		# against the launch directory ($PWD survives the chdir).
		var launch_dir := OS.get_environment("PWD")
		if not launch_dir.is_empty():
			data_root = launch_dir.path_join(data_root).simplify_path()
	var level := _arg_value(args, "--level", "TRAVERSE/LEVEL3/LEVEL3.DTI")
	var arena := _arg_value(args, "--arena", "HMO_1")
	var start := _arg_value(args, "--start", "")
	var start_yaw := float(_arg_value(args, "--start-yaw", "0"))
	var ff_course := _arg_value(args, "--freefall", "")
	var ff_skill := int(_arg_value(args, "--skill", "0"))
	# 0xC0FFEE — the same default mdk-inspect's --freefall-runtime
	# digest runs use, so driven courses are cross-checkable.
	var ff_seed := int(_arg_value(args, "--seed", "12648430"))
	freefall = not ff_course.is_empty()
	shot_path = _arg_value(args, "--screenshot", "")
	if shot_path.is_relative_path() and not shot_path.is_empty():
		var launch_dir := OS.get_environment("PWD")
		if not launch_dir.is_empty():
			shot_path = launch_dir.path_join(shot_path).simplify_path()
	frames_left = int(_arg_value(args, "--frames", "0"))
	interactive = not smoke and shot_path.is_empty() and frames_left <= 0

	# Gate on the extension BEFORE touching it — game mode only
	# discovers GDExtensions listed in .godot/extension_list.cfg
	# (written by an editor scan or by build.sh/run.sh).
	if not ClassDB.class_exists("MdkBridge"):
		printerr("MdkBridge class missing — the GDExtension is not ",
			"loaded. Run frontend/godot/build.sh (it also writes ",
			".godot/extension_list.cfg), then relaunch.")
		get_tree().quit(1)
		return
	bridge = ClassDB.instantiate("MdkBridge")
	if not bridge.initialize(data_root):
		printerr("MdkBridge.initialize failed: ", bridge.get_last_error())
		get_tree().quit(1)
		return
	if freefall:
		if not bridge.load_freefall(int(ff_course), ff_skill, ff_seed):
			printerr("MdkBridge.load_freefall failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return
	else:
		if not bridge.load_level(level):
			printerr("MdkBridge.load_level failed: ", bridge.get_last_error())
			get_tree().quit(1)
			return
		if not bridge.load_arena(arena):
			printerr("MdkBridge.load_arena failed: ", bridge.get_last_error())
			get_tree().quit(1)
			return
	if not freefall and not start.is_empty():
		# NATIVE DIAGNOSTIC — re-anchor into --arena at the given MDK
		# position (mirrors mdk-inspect's --arena/--start selftests).
		var parts := start.split(" ", false)
		if parts.size() != 3:
			printerr("--start needs 'X Y Z'")
			get_tree().quit(1)
			return
		var arena_idx := -1
		var names: Array = bridge.get_arena_names()
		for i in names.size():
			if String(names[i]) == arena:
				arena_idx = i
				break
		if arena_idx < 0:
			printerr("--arena '", arena, "' not in level")
			get_tree().quit(1)
			return
		var dr: Dictionary = bridge.diagnostic_start(arena_idx,
			Vector3(float(parts[0]), float(parts[1]), float(parts[2])),
			start_yaw)
		if not dr.get("ok", false):
			printerr("diagnostic_start failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return

	_build_player_proxy()
	_build_kurt_presenter()
	_build_combat_presenter()
	_build_hud_presenter()

	# One idle frame settles the deterministic spawn camera.
	bridge.step_frame_input(0.0, {})
	if freefall:
		_apply_freefall()
	else:
		_apply_arena_snapshots()
		_apply_object_snapshots()
		_apply_player_snapshot()
		_apply_camera_snapshot()
		_apply_kurt_snapshot()
		_update_debug_label()

	if smoke:
		if "--save-restore" in args:
			# Phase 17A closeout — the save->restore golden is bound
			# to the canonical LEVEL3 combat arena (HMO_9).
			if freefall or level != "TRAVERSE/LEVEL3/LEVEL3.DTI":
				printerr("--save-restore smoke requires the " +
					"default LEVEL3 launch")
				get_tree().quit(2)
				return
			_run_smoke_restore()
		elif freefall:
			_run_smoke_freefall(int(ff_course), ff_skill, ff_seed)
		elif level == "TRAVERSE/LEVEL3/LEVEL3.DTI" and \
				arena == "HMO_1" and start.is_empty():
			_run_smoke(data_root)
		else:
			_run_smoke_generic(level, arena)
		get_tree().quit(0 if failures == 0 else 1)
		return
	if not shot_path.is_empty():
		# --headless forces the dummy rendering server: no viewport
		# texture exists to read back. Fail intentionally instead of
		# dereferencing null later.
		if DisplayServer.get_name() == "headless":
			printerr("--screenshot needs a real renderer; ",
				"--headless always selects the dummy one. Run ",
				"without --headless (game mode opens a window ",
				"briefly).")
			get_tree().quit(2)
			return
		shot_frames_left = 8  # let the pipeline settle first
	if interactive:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

	if freefall:
		print(("mdk-godot: FREEFALL course=%d skill=%d seed=%08x  " +
			"(arrows/WASD steer, F3 debug, Esc release/quit)") %
			[int(ff_course), ff_skill, ff_seed])
	else:
		print(("mdk-godot: arena=%s arenas=%d  (WASD/QE move+strafe, " +
			"AD turn, RF look, Space jump, mouse=captured, F1 " +
			"collision, F2 object debug, Esc release/quit)") %
			[arena, bridge.get_arena_names().size()])


func _build_player_proxy() -> void:
	# DebugBody — capsule over the proven player extents: the core
	# playerBox is pos+-1.25 x/y and pos.z..pos.z+4.25 (MDK) ->
	# 2.5x2.5 footprint, 4.25 tall, snapshot pos at the feet (+Y up).
	var cap := CapsuleMesh.new()
	cap.radius = 1.0
	cap.height = 4.25
	$PlayerRoot/DebugBody.mesh = cap
	$PlayerRoot/DebugBody.position = Vector3(0, 2.125, 0)
	body_mat = StandardMaterial3D.new()
	body_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	body_mat.albedo_color = COLOR_GROUNDED
	$PlayerRoot/DebugBody.material_override = body_mat
	# ForwardMarker — a nose box on local -Z (the facing direction),
	# distinct from the body so strafe-vs-turn is visible.
	var nose := BoxMesh.new()
	nose.size = Vector3(0.35, 0.35, 1.7)
	$PlayerRoot/ForwardMarker.mesh = nose
	$PlayerRoot/ForwardMarker.position = Vector3(0, 3.2, -1.6)
	marker_mat = StandardMaterial3D.new()
	marker_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	marker_mat.albedo_color = Color(0.2, 0.7, 1.0)
	$PlayerRoot/ForwardMarker.material_override = marker_mat
	# PlayerBoxWire — the exact collision extents (0x540c30..44) as
	# a 12-edge line box, rebuilt per frame in world space.
	$PlayerBoxWire.mesh = ImmediateMesh.new()
	wire_mat = StandardMaterial3D.new()
	wire_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	wire_mat.albedo_color = Color(1.0, 0.85, 0.15)
	$PlayerBoxWire.material_override = wire_mat
	# CollisionDebug — the display set's collision poly edges
	# combined (F1 toggles).
	col_mat = StandardMaterial3D.new()
	col_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	col_mat.albedo_color = Color(0.9, 0.2, 0.9)
	$CollisionDebug.material_override = col_mat
	$CollisionDebug.visible = false
	# Object AABB wires (F2 toggles ObjectDebugRoot).
	obj_wire_mat = StandardMaterial3D.new()
	obj_wire_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	obj_wire_mat.albedo_color = Color(1.0, 0.45, 0.9)


func _apply_arena_snapshots() -> void:
	# The traversal display set: one MeshInstance3D per presented
	# arena (current + active partner — a corridor current arena
	# contributes nothing, its partner's geometry stays up).
	var snaps: Array = bridge.get_arena_render_snapshots()
	var live := {}
	var lines := PackedVector3Array()
	for s in snaps:
		var idx := int(s["arena_index"])
		live[idx] = true
		var node: MeshInstance3D = $ArenaRoot.get_node_or_null(
			"Arena_%d" % idx)
		if node == null:
			node = MeshInstance3D.new()
			node.name = "Arena_%d" % idx
			$ArenaRoot.add_child(node)
		node.mesh = s["mesh"]
		node.set_surface_override_material(0, s["material"])
		lines.append_array(s["collision_lines"])
	for child in $ArenaRoot.get_children():
		var idx := int(child.name.trim_prefix("Arena_"))
		if not live.has(idx):
			child.queue_free()
	# Combined collision soup for the F1 debug view.
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = lines
	var am := ArrayMesh.new()
	if not lines.is_empty():
		am.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	$CollisionDebug.mesh = am
	last_display_digest = bridge.get_display_digest()


func _split_elem_meshes(src: ArrayMesh,
		surface_elems: PackedInt32Array) -> Array:
	# One single-surface mesh per element — lets the element-disable
	# mask toggle visibility per element.
	var out := []
	for s in src.get_surface_count():
		var arrays: Array = src.surface_get_arrays(s)
		var m := ArrayMesh.new()
		m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		out.append({"elem": int(surface_elems[s]), "mesh": m})
	return out


func _elem_material(elem_name: String) -> StandardMaterial3D:
	# Deterministic per-element debug material — the model triangle
	# record's material/UV fields are NOT evidenced, so objects
	# render unshaded in stable element-name colors. Documented
	# fidelity seam (docs/GODOT_FRONTEND.md §materials).
	var key := elem_name if not elem_name.is_empty() else "<anon>"
	if elem_mats.has(key):
		return elem_mats[key]
	var h := 5381
	for i in key.length():
		h = ((h * 33) + key.unicode_at(i)) & 0x7fffffff
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	m.albedo_color = Color.from_hsv(float(h % 360) / 360.0, 0.55, 0.95)
	elem_mats[key] = m
	return m


func _object_geom_meshes(g: Dictionary) -> Array:
	var key := int(g["geom_key"])
	if geom_cache.has(key):
		return geom_cache[key]
	var split := _split_elem_meshes(g["mesh"], g["surface_elems"])
	geom_cache[key] = split
	return split


func _apply_object_snapshots() -> void:
	var snaps: Array = bridge.get_object_snapshots()
	var live := {}
	for o in snaps:
		var oid := int(o["id"])
		live[oid] = true
		var node: Node3D = $DynamicObjectRoot.get_node_or_null(
			"Object_%d" % oid)
		if node == null:
			node = Node3D.new()
			node.name = "Object_%d" % oid
			$DynamicObjectRoot.add_child(node)
			node.set_meta("geom_key", -1)
		# Geometry (re)build only when the core digest changes —
		# deep-copied models may diverge later; the digest proves it.
		if int(node.get_meta("geom_key")) != int(o["geom_key"]):
			for c in node.get_children():
				node.remove_child(c)
				c.free()
			var g: Dictionary = bridge.get_object_geometry(oid)
			if not g.is_empty():
				var names: PackedStringArray = g["elem_names"]
				for sm in _object_geom_meshes(g):
					var mi := MeshInstance3D.new()
					mi.name = "E%d" % int(sm["elem"])
					mi.mesh = sm["mesh"]
					mi.material_override = _elem_material(
						names[int(sm["elem"])])
					mi.set_meta("elem", int(sm["elem"]))
					node.add_child(mi)
				node.set_meta("geom_key", int(o["geom_key"]))
		# Core-authoritative transform — verbatim from the snapshot.
		node.transform = o["transform"]
		# Element-disable mask (connMaskLock/HC, SW_DUMMY, +0x2c8).
		var mask := int(o["elem_mask"])
		for c in node.get_children():
			c.visible = (mask & (1 << int(c.get_meta("elem")))) == 0
		# F2 debug — world-space AABB wire + a Label3D tag.
		if obj_debug:
			_update_object_debug(o)
	for child in $DynamicObjectRoot.get_children():
		var oid := int(child.name.trim_prefix("Object_"))
		if not live.has(oid):
			child.queue_free()
			_clear_object_debug(oid)


func _update_object_debug(o: Dictionary) -> void:
	var oid := int(o["id"])
	var wire: MeshInstance3D = obj_wires.get(oid)
	if wire == null:
		wire = MeshInstance3D.new()
		wire.mesh = ImmediateMesh.new()
		wire.material_override = obj_wire_mat
		$ObjectDebugRoot.add_child(wire)
		obj_wires[oid] = wire
	var box: AABB = o["aabb"]
	var im: ImmediateMesh = wire.mesh
	im.clear_surfaces()
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	for e in BOX_EDGES:
		im.surface_add_vertex(box.position + e[0] * box.size)
		im.surface_add_vertex(box.position + e[1] * box.size)
	im.surface_end()
	var tag: Label3D = obj_tags.get(oid)
	if tag == null:
		tag = Label3D.new()
		tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		tag.font_size = 48
		tag.outline_size = 8
		$ObjectDebugRoot.add_child(tag)
		obj_tags[oid] = tag
	tag.position = box.position + Vector3(box.size.x * 0.5,
		box.size.y + 0.5, box.size.z * 0.5)
	tag.text = "%s #%d a:%d e:%d s:%d" % [String(o["model"]), oid,
		int(o["arena"]), int(o["enemy_index"]), int(o["spawn_id"])]


func _clear_object_debug(oid: int) -> void:
	if obj_wires.has(oid):
		obj_wires[oid].queue_free()
		obj_wires.erase(oid)
	if obj_tags.has(oid):
		obj_tags[oid].queue_free()
		obj_tags.erase(oid)


func _apply_camera_snapshot() -> void:
	var cam: Dictionary = bridge.get_camera_snapshot()
	if cam.is_empty():
		return
	$Camera3D.global_transform = cam["transform"]
	$Camera3D.fov = cam["fov_deg"]


func _apply_player_snapshot() -> void:
	var p: Dictionary = bridge.get_player_snapshot()
	if p.is_empty():
		return
	# Presentation only — the transform is a copy of core output;
	# nothing on the Godot side ever writes back into the runtime.
	$PlayerRoot.global_transform = p["transform"]
	body_mat.albedo_color = (COLOR_GROUNDED if p["grounded"]
		else COLOR_AIRBORNE)
	_update_box_wire(p["box"])


# ---------------------------------------------------------------------------
# Phase 16B — traversal Kurt sprite presentation.
#
# The original draws Kurt into the 600x360 indexed work buffer at
# integer anchors produced by FUN_00431300's registration (the M1
# transform + mode-0 projector -> rint -> 0x540c4c/50), with the
# scope/HUD y-offset 0x540d34 added to the blit y. FUN_00409724 then
# draws the optional overlay FIRST at (x+ofsX, y+ofsY), then the main
# frame at (x, y) — each through FUN_00409760's hotspot rule
# (dest = anchor - hotspot) at native 1:1 pixels.
#
# Here the 600x360 software space maps onto the real viewport by
# independent x/y window ratios — the same normalized position the
# original computes. There is no second projection and no sprite
# scaling beyond the space->window map: the 0x540dbc scale field is
# scope/HUD state, never sprite size.
# ---------------------------------------------------------------------------

const KURT_SCREEN := Vector2(600.0, 360.0)


func _build_kurt_presenter() -> void:
	# The screen-space quads live in the scene (KurtLayer canvas):
	# KurtOverlay precedes KurtMain in sibling order, so the main
	# sprite composites on top — the original draws the overlay
	# first for exactly that stacking. Nearest filtering keeps the
	# indexed-art edges (no filtering existed in the blit).
	for n in ["KurtOverlay", "KurtMain"]:
		var r: TextureRect = $KurtLayer/KurtViewport.get_node(n)
		r.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST


func _kurt_reject(dx: float, dy: float, w: float, h: float) -> bool:
	# FUN_00415ff0's entry rejects in 600x360 space: fully outside
	# draws nothing; the right edge is all-or-nothing (x>=0 && x+w
	# spills past 600 -> nothing at all). Left/top partial clips
	# draw — offscreen pixels simply land outside the viewport.
	if dx >= KURT_SCREEN.x or dy >= KURT_SCREEN.y:
		return true
	if dx + w <= 0.0 or dy + h <= 0.0:
		return true
	if dx >= 0.0 and dx + w > KURT_SCREEN.x:
		return true
	return false


func _apply_kurt_snapshot() -> void:
	var k: Dictionary = bridge.get_kurt_snapshot()
	if k.is_empty():
		return
	last_kurt = k
	var vp := get_viewport().get_visible_rect().size
	var sx := vp.x / KURT_SCREEN.x
	var sy := vp.y / KURT_SCREEN.y
	var km: TextureRect = $KurtLayer/KurtViewport/KurtMain
	var ko: TextureRect = $KurtLayer/KurtViewport/KurtOverlay
	var drawn := bool(k["drawn"])
	# Blit anchor: x = 0x540c4c, y = 0x540d34 + 0x540c50 — the
	# scope/HUD offset folds into the blit y for both sprites.
	var ax := float(k["anchor_x"])
	var ay := float(k["anchor_y"]) + float(k["scope_ofs"])
	var ov: Dictionary = k["overlay"]
	var otex = ov.get("tex")
	if drawn and not ov.is_empty() and otex != null:
		# Overlay at (x + 0x54cb0c, y + 0x54cb10), dest -= hotspot.
		var dx := ax + float(k["ofs_x"]) - float(ov["hot_x"])
		var dy := ay + float(k["ofs_y"]) - float(ov["hot_y"])
		ko.visible = not _kurt_reject(dx, dy, float(ov["w"]),
			float(ov["h"]))
		ko.position = Vector2(dx * sx, dy * sy)
		ko.size = Vector2(float(ov["w"]) * sx, float(ov["h"]) * sy)
		ko.texture = otex
	else:
		ko.visible = false
	var mn: Dictionary = k["main"]
	var mtex = mn.get("tex")
	if drawn and not mn.is_empty() and mtex != null:
		var dx := ax - float(mn["hot_x"])
		var dy := ay - float(mn["hot_y"])
		km.visible = not _kurt_reject(dx, dy, float(mn["w"]),
			float(mn["h"]))
		km.position = Vector2(dx * sx, dy * sy)
		km.size = Vector2(float(mn["w"]) * sx, float(mn["h"]) * sy)
		km.texture = mtex
	else:
		km.visible = false


func _update_box_wire(box: AABB) -> void:
	var im: ImmediateMesh = $PlayerBoxWire.mesh
	im.clear_surfaces()
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	for e in BOX_EDGES:
		im.surface_add_vertex(box.position + e[0] * box.size)
		im.surface_add_vertex(box.position + e[1] * box.size)
	im.surface_end()


# ---------------------------------------------------------------------------
# Phase 17A — traversal combat presentation.
#
# Everything below is FUN_0045f030's domain: the scoped shot render
# pass (world class-mesh per slot + the three bullet-cam HUD windows
# + the indicator fills) and the combat FX that FUN_00437444 /
# FUN_004575fc / FUN_00457cf4 spawn. Core owns every gameplay fact —
# snapshots/events arrive already converted; this file only turns
# them into nodes.
#
# Deferred seams (documented, not emulated):
#   - the per-face lambert shade depth (pen - +0x95 * max(0, n.L) in
#     FUN_00405c58) — shards draw unshaded in their base pen, the
#     same convention as object element materials.
#   - the 0x404e40 emitter variant (25% of effScale>=1.0 shards that
#     trail 0x196==0xb stationary children every +0x1a2 ticks).
#   - the second 0x412e94 sweep-damp on contact and the +0x186&8
#     expire->FUN_004575fc remnant path (not set on this pool's
#     records, so expiry just frees).
#   - RICO1-3 impact sounds and +0x150 per-object sound overrides
#     (aux carries the marker; no audio here).
#   - FUN_0046ec60 heading-indicator sprite and FUN_00409760's type-4
#     +0xf4 HUD counter — HUD garnish beyond the combat pass.
# ---------------------------------------------------------------------------


func _build_combat_presenter() -> void:
	# Per-slot world mesh — children rebuild when the bound class
	# record's geom_key changes.
	for i in 3:
		var n := Node3D.new()
		n.name = "Shot_%d" % i
		n.visible = false
		$ShotRoot.add_child(n)
		shot_nodes.append(n)
		# Bullet-cam: a square SubViewport (the window's 140x70 squash
		# is handled by stretching — the projector's 69.95/34.95
		# divisors give ~90 deg on both axes). Shares the main world
		# so the display list the window re-renders matches.
		var vp := SubViewport.new()
		vp.name = "ShotVP_%d" % i
		vp.size = Vector2i(140, 140)
		vp.world_3d = get_viewport().world_3d
		vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
		var cam := Camera3D.new()
		cam.fov = SHOT_CAM_FOV
		cam.near = 0.05
		cam.far = 20000.0
		vp.add_child(cam)
		$ShotCamRoot.add_child(vp)
		shot_vps.append(vp)
		shot_cams.append(cam)
		var vt := ViewportTexture.new()
		vt.viewport_path = vp.get_path()
		var win := TextureRect.new()
		win.name = "ShotWin_%d" % i
		win.texture = vt
		win.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		win.stretch_mode = TextureRect.STRETCH_SCALE
		win.mouse_filter = Control.MOUSE_FILTER_IGNORE
		win.visible = false
		$ShotCamLayer.add_child(win)
		shot_wins.append(win)
		# The expired/free-slot indicator fill (FUN_00416aa8 rectfill
		# of the window rect in the hudFrame pen).
		var fill := ColorRect.new()
		fill.name = "ShotFill_%d" % i
		fill.mouse_filter = Control.MOUSE_FILTER_IGNORE
		fill.visible = false
		$ShotCamLayer.add_child(fill)
		shot_fills.append(fill)
	fx_palette = bridge.get_active_palette()


func _pen_color(pen: int) -> Color:
	var p := clampi(pen, 0, 255) * 3
	if fx_palette.size() < 768:
		return Color(0, 0, 0)
	return Color(fx_palette[p] / 255.0, fx_palette[p + 1] / 255.0,
		fx_palette[p + 2] / 255.0)


func _shard_material(pen: int) -> StandardMaterial3D:
	var key := clampi(pen, 0, 255)
	if shard_mats.has(key):
		return shard_mats[key]
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	m.albedo_color = _pen_color(key)
	shard_mats[key] = m
	return m


func _fx_rand() -> float:
	# Deterministic presentation jitter (SplitMix-ish over a local
	# counter) — NEVER touches core RNG, so diagnostics and draw
	# order can't perturb gameplay randomness.
	fx_seq = (fx_seq + 1) & 0x7fffffff
	var z := (fx_seq * 0x6c8e9cf5) & 0x7fffffff
	z = (z ^ (z >> 15)) & 0x7fffffff
	z = (z * 0x2c1b3c6d) & 0x7fffffff
	z = (z ^ (z >> 12)) & 0x7fffffff
	return float(z & 0xffff) / 65536.0


func _shard_mesh(jit_seed: int) -> ArrayMesh:
	# FUN_00404108's tetra (OBSERVED): verts = (dirTable[i] +
	# (rand-0x4000)*2e-5 per comp) * effScale — the dir entry is
	# +-0.5 and the jitter spans +-0.33, so each shard is a visibly
	# irregular tetra. A bank of jittered variants keeps the draw
	# cheap while preserving the observed deformation range.
	var seed := jit_seed
	var verts := PackedVector3Array()
	for i in 4:
		var v: Vector3 = SHARD_DIR[i]
		seed = (seed * 1103515245 + 12345) & 0x7fffffff
		v.x += ((seed & 0x7fff) / 32768.0 - 0.5) * 0.65536
		seed = (seed * 1103515245 + 12345) & 0x7fffffff
		v.y += ((seed & 0x7fff) / 32768.0 - 0.5) * 0.65536
		seed = (seed * 1103515245 + 12345) & 0x7fffffff
		v.z += ((seed & 0x7fff) / 32768.0 - 0.5) * 0.65536
		verts.append(v)
	var idx := PackedInt32Array()
	for f in SHARD_FACES:
		for vi in f:
			idx.append(vi)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_INDEX] = idx
	var m := ArrayMesh.new()
	m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return m


func _spawn_shards(pos: Vector3, count: int, select: int,
		arena_idx: int) -> void:
	# FUN_00437444's spawn loop: count records through the +0x5c
	# pool, each FUN_00404108-inited (pos jitter, random velocity,
	# 60+(rand>>9) tick life, effScale = select.scale * (1+-0.5)).
	var mode: Dictionary
	if select == 1:
		mode = SHARD_SEL1
	elif select == 0:
		mode = SHARD_SEL0_ON if fx_enable_live else SHARD_SEL0_OFF
	else:
		mode = SHARD_SEL2
	var pen := int(mode["pen"])
	var scale := float(mode["scale"])
	for i in count:
		if shards.size() >= SHARD_CAP:
			break
		var mi := MeshInstance3D.new()
		# Jittered-tetra bank — 8 variants is plenty for the
		# +-0.33 vert-perturbation range to read as irregular.
		while shard_tetra.size() < 8:
			shard_tetra.append(_shard_mesh(shard_tetra.size() * 7919))
		mi.mesh = shard_tetra[fx_seq & 7]
		mi.material_override = _shard_material(pen)
		# effScale = select.scale * (1 + (rand-0x4000)*3.0517e-5)
		mi.scale = Vector3.ONE * (scale * (0.5 + _fx_rand()))
		# +0x20/+0x30/+0x40: pos + jitter — +-1.0 horiz (2^-14),
		# +-0.5 vertical (2^-15), MDK axes folded: -y,x->godot -x,-z.
		mi.position = pos + Vector3(
			(_fx_rand() - 0.5) * 2.0,
			(_fx_rand() - 0.5) * 1.0,
			(_fx_rand() - 0.5) * 2.0)
		$FxRoot.add_child(mi)
		# +0x18a/+0x18e/+0x192 velocity (units/tick): +-1.0 horiz,
		# (rand-0x800)*2^-14 vertical -> [-0.125, +1.875) up-biased.
		var vel := Vector3(
			(_fx_rand() - 0.5) * 2.0,
			_fx_rand() * 2.0 - 0.125,
			(_fx_rand() - 0.5) * 2.0)
		# +0x196 countdown: 60 + (rand>>9) ticks (0x404417, OBSERVED).
		var ttl := 60.0 + floorf(_fx_rand() * 64.0)
		# 0x46b180 tumble — random per-axis rates (~+-14 deg/tick).
		var spin := Vector3((_fx_rand() - 0.5) * 28.0,
			(_fx_rand() - 0.5) * 28.0, (_fx_rand() - 0.5) * 28.0)
		shards.append({"mi": mi, "vel": vel, "ttl": ttl,
			"arena": arena_idx, "spin": spin})
		fx_stats["shards"] = int(fx_stats["shards"]) + 1


func _spawn_tear_shards(pos: Vector3, arena_idx: int) -> void:
	# FUN_00457cf4's 16-shard child burst — the same 0x404108 init
	# (random vel/ttl/jitter) at pen 0x30, shade 0x10, scale 1.0.
	if shards.size() >= SHARD_CAP:
		return
	for i in TEAR_SHARDS:
		if shards.size() >= SHARD_CAP:
			break
		var mi := MeshInstance3D.new()
		while shard_tetra.size() < 8:
			shard_tetra.append(_shard_mesh(shard_tetra.size() * 7919))
		mi.mesh = shard_tetra[(fx_seq + i) & 7]
		mi.material_override = _shard_material(TEAR_PEN)
		mi.scale = Vector3.ONE * (0.5 + _fx_rand())
		mi.position = pos + Vector3(
			(_fx_rand() - 0.5) * 2.0,
			(_fx_rand() - 0.5) * 1.0,
			(_fx_rand() - 0.5) * 2.0)
		$FxRoot.add_child(mi)
		shards.append({"mi": mi,
			"vel": Vector3((_fx_rand() - 0.5) * 2.0,
				_fx_rand() * 2.0 - 0.125, (_fx_rand() - 0.5) * 2.0),
			"ttl": 60.0 + floorf(_fx_rand() * 64.0),
			"arena": arena_idx,
			"spin": Vector3((_fx_rand() - 0.5) * 28.0,
				(_fx_rand() - 0.5) * 28.0, (_fx_rand() - 0.5) * 28.0)})
		fx_stats["shards"] = int(fx_stats["shards"]) + 1


func _spawn_remnant(ev: Dictionary) -> void:
	# FUN_004575fc / the FUN_00457cf4 corpse — an EXPLODE-class dead
	# object at the event transform (the bridge folds facingDeg +
	# bankDeg + scale through buildObjectMatrix). Persist until the
	# arena tears down — the original record is +0x06-flagged and
	# survives the FUN_0045cf18 sweep.
	if not named_geom.has("EXPLODE"):
		named_geom["EXPLODE"] = _load_geom_entry(
			bridge.get_named_geometry("EXPLODE"))
	var ent: Dictionary = named_geom["EXPLODE"]
	if ent["meshes"].is_empty():
		return
	var n := Node3D.new()
	n.name = "Remnant_%d" % remnant_seq
	remnant_seq += 1
	n.transform = ev["transform"]
	for sm in ent["meshes"]:
		var mi := MeshInstance3D.new()
		mi.mesh = sm["mesh"]
		mi.material_override = _elem_material(String(sm["name"]))
		n.add_child(mi)
	$FxRoot.add_child(n)
	remnants.append(n)
	fx_stats["remnants"] = int(fx_stats["remnants"]) + 1


func _load_geom_entry(g: Dictionary) -> Dictionary:
	# Normalize a get_*_geometry dict into {key, meshes:[{mesh,name}]}
	# for per-element material naming (same convention as objects).
	if g.is_empty():
		return {"key": -1, "meshes": []}
	var meshes := []
	var names: PackedStringArray = g["elem_names"]
	for sm in _object_geom_meshes(g):
		var nm := ""
		var ei := int(sm["elem"])
		if ei >= 0 and ei < names.size():
			nm = names[ei]
		meshes.append({"mesh": sm["mesh"], "name": nm})
	return {"key": int(g["geom_key"]), "meshes": meshes}


func _apply_shot_snapshots() -> void:
	var ss: Dictionary = bridge.get_shot_snapshots()
	if ss.is_empty():
		return
	var scoped := bool(ss["scoped"])
	var hud := bool(ss["hud_active"])
	fx_enable_live = bool(ss["fx_enable"])
	# The active palette follows the displayed arena — refresh per
	# frame and drop stale pen materials when it changes.
	var pal: PackedByteArray = bridge.get_active_palette()
	if pal != fx_palette:
		fx_palette = pal
		shard_mats.clear()
	var vp := get_viewport().get_visible_rect().size
	var sx := vp.x / KURT_SCREEN.x
	var sy := vp.y / KURT_SCREEN.y
	var arr: Array = ss["shots"]
	for i in mini(shot_nodes.size(), arr.size()):
		var s: Dictionary = arr[i]
		var node: Node3D = shot_nodes[i]
		# World mesh — the 0x431503 submit gate is state == 1 only,
		# independent of the scope state (a shot stays live after
		# unscoping until it dies).
		if bool(s["mesh_renderable"]):
			var ci := int(s["class_idx"])
			if not shot_geom.has(ci):
				shot_geom[ci] = _load_geom_entry(
					bridge.get_shot_geometry(ci))
			var ent: Dictionary = shot_geom[ci]
			var gkey := int(ent["key"])
			if int(node.get_meta("geom_key", -1)) != gkey:
				for c in node.get_children():
					node.remove_child(c)
					c.free()
				for sm in ent["meshes"]:
					var mi := MeshInstance3D.new()
					mi.mesh = sm["mesh"]
					mi.material_override = _elem_material(
						String(sm["name"]))
					node.add_child(mi)
				node.set_meta("geom_key", gkey)
			node.transform = s["mesh_transform"]
			node.visible = gkey >= 0
		else:
			node.visible = false
		# The slot's HUD window rect in scaled 600x360 space.
		var rect: Rect2 = SHOT_HUD_RECT[i]
		var pos2 := Vector2(rect.position.x * sx,
			rect.position.y * sy)
		var size2 := Vector2(rect.size.x * sx, rect.size.y * sy)
		var win: TextureRect = shot_wins[i]
		var fill: ColorRect = shot_fills[i]
		var vps: SubViewport = shot_vps[i]
		# Mode 0 (bullet cam): state!=0 && lifetime>0 under the
		# scoped gate — the window re-renders the display list from
		# the tail camera (FUN_0045ee08).
		var wact := scoped and bool(s["window_active"])
		win.visible = wact
		vps.render_target_update_mode = \
			SubViewport.UPDATE_ALWAYS if wact else \
			SubViewport.UPDATE_DISABLED
		if wact:
			win.position = pos2
			win.size = size2
			shot_cams[i].transform = s["cam_transform"]
		# Mode 1 (FUN_00416aa8 rectfill): expired/free slots draw the
		# hudFrame pen over the window — gated on hudActive
		# (0x5414d4); an active slot's -1 frame draws nothing.
		var hf := int(s["hud_frame"])
		var fact := scoped and hud and hf >= 0
		fill.visible = fact
		if fact:
			fill.position = pos2
			fill.size = size2
			fill.color = _pen_color(hf)


func _drain_combat_fx() -> void:
	var evs: Array = bridge.drain_combat_fx()
	for ev in evs:
		fx_stats["events"] = int(fx_stats["events"]) + 1
		fx_recent.append(ev)
		while fx_recent.size() > 16:
			fx_recent.pop_front()
		var k := int(ev["kind"])
		var kinds: Dictionary = fx_stats["kinds"]
		kinds[k] = int(kinds.get(k, 0)) + 1
		match k:
			0, 1, 2, 3:
				# FUN_00437444 — the shard burst. count = mode (ECX),
				# palette/life/scale select = variant ([EBP+8]).
				_spawn_shards(ev["pos"], int(ev["mode"]),
					int(ev["variant"]), int(ev["arena_index"]))
			4:
				# FUN_004575fc — the EXPLODE detonation remnant.
				_spawn_remnant(ev)
			5:
				# +0x110 death-script handoff — the object's script
				# owns whatever it shows; nothing to spawn here.
				pass
			6:
				# FUN_00457cf4 — 16-shard burst + the EXPLODE corpse
				# (both spawned inside the teardown, OBSERVED).
				_spawn_tear_shards(ev["pos"], int(ev["arena_index"]))
				_spawn_remnant(ev)


func _tick_combat_fx(delta: float) -> void:
	if shards.is_empty():
		return
	var step := delta * SHARD_TICKS_PER_SEC
	var keep := []
	for sh in shards:
		var mi: MeshInstance3D = sh["mi"]
		var vel: Vector3 = sh["vel"]
		# FUN_00405014 (OBSERVED): newPos = pos + vel*smoothed, then
		# the FUN_00406a0c contact stab (record arena -> cur ->
		# partner). On contact: life -20, vel -= 1.4*(vel.n)*n
		# (0x4943ec lossy reflect), pos = crossing point. On a miss:
		# vel.z_mdk = 0.0711 - vel.z_mdk — the 0.2844*0.25 flutter
		# (0x4943dc*0x4943e4) — z_mdk is Godot +Y.
		var np: Vector3 = mi.position + vel * step
		var hit: Dictionary = bridge.fx_stab(mi.position, np,
			int(sh["arena"]))
		if not hit.is_empty():
			mi.position = hit["pos"]
			var n: Vector3 = hit["normal"]
			vel = vel - n * (1.4 * vel.dot(n))
			sh["ttl"] = float(sh["ttl"]) - 20.0
		else:
			mi.position = np
			vel.y = 0.0711 - vel.y
		sh["vel"] = vel
		# +0x196 -= frameStep (0x49b6e8 — ~1 per 30Hz tick); the
		# countdown is frame-domain, so ttl decrements per tick.
		sh["ttl"] = float(sh["ttl"]) - step
		# The 0x46b180 tumble — cosmetic random-axis spin.
		var sp: Vector3 = sh["spin"]
		mi.rotation += sp * (step * (PI / 180.0))
		if float(sh["ttl"]) <= 0.0:
			mi.queue_free()
		else:
			keep.append(sh)
	shards = keep


func _combat_diag_text() -> String:
	if not combat_diag:
		return ""
	return ("\nfx ev %d  shards %d live %d  remnants %d  kinds %s" %
		[int(fx_stats["events"]), int(fx_stats["shards"]),
		shards.size(), int(fx_stats["remnants"]),
		str(fx_stats["kinds"])])


func _reset_presentation_for_restore() -> void:
	# Restore boundary — bridge.restore_save() swapped in a fresh
	# authoritative TraversalRuntime. Everything below presented the
	# DISCARDED timeline: shot meshes, bullet-cam windows, transient
	# shard/remnant nodes, and every id-keyed cache. None of it is
	# serialized state — it rebuilds from the first post-restore
	# snapshot pass. Immediate free() (not queue_free) so the reset
	# is complete before the caller inspects the tree.
	for i in shot_nodes.size():
		var n: Node3D = shot_nodes[i]
		for c in n.get_children():
			n.remove_child(c)
			c.free()
		n.set_meta("geom_key", -1)
		n.visible = false
		shot_wins[i].visible = false
		shot_fills[i].visible = false
		shot_vps[i].render_target_update_mode = \
			SubViewport.UPDATE_DISABLED
	shot_geom.clear()
	named_geom.clear()
	# FxRoot carries shards + remnant nodes — sweep the whole child
	# set (ttl-reaped shards are queue-pending but still children).
	for child in $FxRoot.get_children():
		$FxRoot.remove_child(child)
		child.free()
	shards.clear()
	remnants.clear()
	remnant_seq = 0
	fx_seq = 0
	fx_recent.clear()
	fx_stats = {"events": 0, "shards": 0, "remnants": 0, "kinds": {}}
	fx_enable_live = false
	# The palette is re-read on the next apply — drop it so a
	# different level's palette can't alias.
	fx_palette = PackedByteArray()
	shard_mats.clear()
	shard_tetra.clear()
	# Every minted object id died with the old runtime (the bridge
	# re-mints from an empty map) — drop the node set wholesale.
	for child in $DynamicObjectRoot.get_children():
		$DynamicObjectRoot.remove_child(child)
		child.free()
	for oid in obj_wires.keys():
		_clear_object_debug(oid)
	# Arena nodes rebind to the restored display set.
	for child in $ArenaRoot.get_children():
		$ArenaRoot.remove_child(child)
		child.free()
	last_display_digest = -1
	geom_cache.clear()
	elem_mats.clear()
	# Phase 17B.2 — HUD/view surfaces presented the discarded
	# runtime; they rebuild from the first post-restore snapshot.
	_hide_hud()


# ---------------------------------------------------------------------------
# Phase 17B.2 — traversal HUD / view presentation.
#
# The core composes the authoritative 600x360 indexed framebuffer
# (rt.hud.fb) inside the stepped frame — health, weapons, ammo,
# inventory, timer/status, damage, and the SKULL death overlay are
# already pixels when they reach the bridge. get_hud_snapshot()
# copies the buffer verbatim and reuses one palette-expanded
# ImageTexture (pen 0 -> alpha 0). GDScript only places layers:
#
#   Kurt(1) < bezel(2) < scope view(3) < shot windows(4)
#       < HUD overlay(5) < fade(6) < debug(10)
#
# The SNIPERS1 bezel is a 640x480 screen buffer shown beneath the
# view while the core scope gate is open; the fb rides at (20,55)
# inside it (HYPOTHESIS — aperture-aligned, +/-1px). The mode-1
# scope viewport renders the shared world at 384x280 into the
# authored aperture rect; modes 2/3/4 stay on the Phase-17A
# bullet-cam windows. Deferred seams (scope warp, entry fades, the
# window pen fills in fb, message flush) are documented core gaps,
# not emulated here.
# ---------------------------------------------------------------------------


func _build_hud_presenter() -> void:
	var hr: TextureRect = $HudLayer/HudRect
	var br: TextureRect = $BezelLayer/BezelRect
	var sr: TextureRect = $ScopeLayer/ScopeRect
	for r in [hr, br, sr]:
		r.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		r.stretch_mode = TextureRect.STRETCH_SCALE
	# The mode-1 scope viewport — shares the main world_3d exactly
	# like the bullet-cam windows; the authored aperture is 384x280.
	scope_vp = SubViewport.new()
	scope_vp.name = "ScopeVP"
	scope_vp.size = Vector2i(384, 280)
	scope_vp.world_3d = get_viewport().world_3d
	scope_vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	scope_cam = Camera3D.new()
	scope_cam.near = 0.05
	scope_cam.far = 20000.0
	scope_vp.add_child(scope_cam)
	$ShotCamRoot.add_child(scope_vp)
	var vt := ViewportTexture.new()
	vt.viewport_path = scope_vp.get_path()
	sr.texture = vt


func _hide_hud() -> void:
	# The traversal HUD/view is mode-3 only — non-traversal modes and
	# runtime-swap boundaries drop every surface; none is serialized
	# or kept across a restore.
	$HudLayer/HudRect.visible = false
	$BezelLayer/BezelRect.visible = false
	$ScopeLayer/ScopeRect.visible = false
	if scope_vp != null:
		scope_vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	last_hud = {}


func _apply_hud_snapshot() -> void:
	var h: Dictionary = bridge.get_hud_snapshot()
	if h.is_empty():
		_hide_hud()
		return
	last_hud = h
	var vps := get_viewport().get_visible_rect().size
	var sx := vps.x / KURT_SCREEN.x
	var sy := vps.y / KURT_SCREEN.y
	# The composed fb IS the HUD — the TextureRect is full-window
	# (the same 600x360->screen map the Kurt sprite and the shot
	# windows use) and pen-0 pixels are transparent, so the world
	# shows through wherever the compositor wrote nothing.
	var hr: TextureRect = $HudLayer/HudRect
	hr.texture = h["tex"]
	hr.visible = true
	# sniper_view = flagC9c && transitionPhase != 0 — the scope
	# camera/aperture gate. The scope-zoom transition warp is a
	# documented deferred seam, so during transitionPhase 1 the
	# bezel+viewport simply track the (interpolating) core pose.
	var scoped := bool(h["sniper_view"])
	var br: TextureRect = $BezelLayer/BezelRect
	var sr: TextureRect = $ScopeLayer/ScopeRect
	if scoped:
		var bz: Dictionary = h.get("bezel", {})
		if not bz.is_empty():
			# Map so the bezel's fb region lands exactly on the
			# window: fb origin (20,55) inside the 640x480 buffer ->
			# bezel origin at -ofs in fb space.
			var ofs: Vector2i = bz["fb_ofs"]
			br.texture = bz["tex"]
			br.position = Vector2(-float(ofs.x) * sx,
				-float(ofs.y) * sy)
			br.size = Vector2(float(bz["w"]) * sx,
				float(bz["h"]) * sy)
			br.visible = true
		var ap: Rect2i = h["scope_rect"]
		sr.position = Vector2(float(ap.position.x) * sx,
			float(ap.position.y) * sy)
		sr.size = Vector2(float(ap.size.x) * sx,
			float(ap.size.y) * sy)
		sr.visible = true
		# The scoped camera pose is already the core's — mirror what
		# _apply_camera_snapshot put on the main camera this frame.
		scope_cam.global_transform = $Camera3D.global_transform
		scope_cam.fov = $Camera3D.fov
		scope_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	else:
		br.visible = false
		sr.visible = false
		scope_vp.render_target_update_mode = SubViewport.UPDATE_DISABLED


# ---------------------------------------------------------------------------
# Phase 16C — mode-2 freefall presentation.
#
# The original's mode-2 frame (FUN_004103d8) runs the object walk
# (FUN_00410e38) then the render pass (FUN_00410920 → FUN_004109d8):
# per active object, kind-2 entries emit the +0x0c model (and the
# +0x306 chute attachment) under the object's +0xac basis, through the
# fixed-orientation camera FUN_004123f4 writes. The scene twins in
# mdk_core already apply FUN_004555bc's vertex animation to the models
# — everything below only mirrors copy-safe snapshots into nodes.
#
# Deferred seams (never presented — documented, not emulated):
#   kind-1 radar sprite (+0x10c), kind-3 BANG frame-block overlay
#   (+0x110), kind-4 trail (+0x60), kind-5 launch glow (+0x108),
#   ZOOM intro sprites, palette cycling, and all sounds.
# ---------------------------------------------------------------------------


func _ff_input() -> Dictionary:
	# Mode-2 direction channels — the dict booleans land directly on
	# FreefallInput (the same fold FUN_00407e50 gives the bound
	# direction keys: left=-X right=+X up=+Y down=-Y).
	return {
		"left": Input.is_key_pressed(KEY_LEFT) or
			Input.is_key_pressed(KEY_A),
		"right": Input.is_key_pressed(KEY_RIGHT) or
			Input.is_key_pressed(KEY_D),
		"up": Input.is_key_pressed(KEY_UP) or
			Input.is_key_pressed(KEY_W),
		"down": Input.is_key_pressed(KEY_DOWN) or
			Input.is_key_pressed(KEY_S),
	}


func _ff_pen_color(idx: int) -> Color:
	if idx >= 0 and idx < 256 and ff_palette.size() == 768:
		return Color(ff_palette[idx * 3] / 255.0,
			ff_palette[idx * 3 + 1] / 255.0,
			ff_palette[idx * 3 + 2] / 255.0)
	return Color(0, 0, 0)


func _ff_no_draw_material() -> StandardMaterial3D:
	# NONE / index-256 — the original's no-draw flat (0x4edc28 bank's
	# 256th entry is the transparent black a no-op texel resolves to).
	var key := "<nodraw>"
	if ff_materials.has(key):
		return ff_materials[key]
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.albedo_color = Color(0, 0, 0, 0)
	ff_materials[key] = m
	return m


func _ff_fallback_material(name: String) -> StandardMaterial3D:
	# Unresolved material name (absent MTI record, no PEN_<n> digits)
	# — the bridge returns empty; present a stable neutral gray so
	# the surface still proves the geometry.
	var key := "miss:%s" % name
	if ff_materials.has(key):
		return ff_materials[key]
	var h := 5381
	for i in name.length():
		h = ((h * 33) + name.unicode_at(i)) & 0x7fffffff
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	m.albedo_color = Color.from_hsv(float(h % 360) / 360.0, 0.3, 0.7)
	ff_materials[key] = m
	return m


func _ff_material(mat_name: String, pen: int) -> StandardMaterial3D:
	# Surface material for one (element, material-index) group. Tri
	# records carry a signed s16: >=0 indexes the model's name table
	# (a FALL3D_<c+1>.MTI name — texture, index, or PEN_<n> record);
	# <0 encodes a flat palette pen directly (-mi & 0xff).
	if pen >= 0:
		var pk := "pen:%d" % pen
		if ff_materials.has(pk):
			return ff_materials[pk]
		var pm := StandardMaterial3D.new()
		pm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		pm.cull_mode = BaseMaterial3D.CULL_DISABLED
		pm.albedo_color = _ff_pen_color(pen)
		ff_materials[pk] = pm
		return pm
	if mat_name.is_empty():
		return _ff_fallback_material("")
	var mk := "m:%s" % mat_name
	if ff_materials.has(mk):
		return ff_materials[mk]
	var d: Dictionary = bridge.get_freefall_material(mat_name)
	if d.is_empty() or not bool(d.get("valid", false)):
		return _ff_fallback_material(mat_name)
	if int(d.get("palette_index", -1)) >= 0:
		# Index record or PEN_<n> — a flat palette pen. 256 is the
		# bank's no-draw slot (NONE).
		if bool(d.get("no_draw", false)):
			return _ff_no_draw_material()
		var pm2 := StandardMaterial3D.new()
		pm2.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		pm2.cull_mode = BaseMaterial3D.CULL_DISABLED
		pm2.albedo_color = d.get("palette_color",
			_ff_pen_color(int(d["palette_index"])))
		ff_materials[mk] = pm2
		return pm2
	# Texture record — pixel-space UVs are normalized by uv1_scale,
	# which belongs to the resolved texture (per-material, so the
	# name-keyed cache is exact). Nearest filter: the software
	# rasterizer texel-fetches — no filtering existed.
	var tm := StandardMaterial3D.new()
	tm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	tm.cull_mode = BaseMaterial3D.CULL_DISABLED
	tm.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	tm.albedo_texture = d["tex"]
	var tw := float(d["w"])
	var th := float(d["h"])
	if tw > 0.0 and th > 0.0:
		tm.uv1_scale = Vector3(1.0 / tw, 1.0 / th, 1.0)
	ff_materials[mk] = tm
	return tm


func _ff_bind_mesh(mi: MeshInstance3D, g: Dictionary) -> void:
	# The bridge mesh carries one surface per (element, material
	# index) group — materialize each through the shared bank.
	var mesh: ArrayMesh = g["mesh"]
	var names: PackedStringArray = g["surface_mats"]
	var pens: PackedInt32Array = g["surface_pen"]
	for s in mesh.get_surface_count():
		var nm := String(names[s]) if s < names.size() else ""
		var pn := int(pens[s]) if s < pens.size() else -1
		mesh.surface_set_material(s, _ff_material(nm, pn))
	mi.mesh = mesh


func _apply_freefall() -> void:
	var ff: Dictionary = bridge.get_freefall_snapshot()
	if ff.is_empty():
		return
	# Traversal HUD/view is mode-3 only — never over freefall.
	_hide_hud()
	if ff_palette.is_empty() and bool(ff.get("palette_ok", false)):
		ff_palette = ff["palette"]

	# FUN_004123f4's camera — fixed orientation (right=+X, down=-Y,
	# back=+Z semantic rows) at cameraPos, fov from scaleY at the
	# 600x360 projection divisors.
	var ct: Transform3D = ff["camera"]
	$Camera3D.global_transform = ct
	$Camera3D.fov = float(ff["fov_deg"])

	# 0x4edc04 — palette-bright factor applied at upload: <1 dims to
	# black (fade), >1 saturates (damage flash / missile-bump).
	var fade := float(ff["fade"])
	var fr: ColorRect = $FadeLayer/FadeRect
	if fade < 1.0:
		fr.color = Color(0, 0, 0, 1.0 - fade)
		fr.visible = true
	elif fade > 1.0:
		# Saturating multiply approximated as a white-out — the
		# original clamps every channel at 255 (fade=3 -> alpha ~.67).
		fr.color = Color(1, 1, 1, min(1.0, 1.0 - 1.0 / fade))
		fr.visible = true
	else:
		fr.visible = false

	# The FUN_004109d8 entry domain: walk the active list from
	# listHead. Painter-depth sorting is a software-renderer artifact;
	# Godot's depth buffer supersedes it, so nodes keep list order.
	var snaps: Array = bridge.get_freefall_object_snapshots()
	var live := {}
	for o in snaps:
		var slot := int(o["pool_slot"])
		live[slot] = true
		var node: Node3D = $FreefallRoot.get_node_or_null(
			"FFObj_%d" % slot)
		if node == null:
			node = Node3D.new()
			node.name = "FFObj_%d" % slot
			node.set_meta("geom_key", -1)
			var mi := MeshInstance3D.new()
			mi.name = "Body"
			node.add_child(mi)
			$FreefallRoot.add_child(node)
		var presented := bool(o.get("presented", false))
		node.visible = presented
		var body: MeshInstance3D = node.get_node("Body")
		if presented:
			# The twin's elemVerts mutate every animated frame — the
			# geom_key digest gates the mesh rebuild (same contract
			# the traversal object path uses).
			var gkey := int(o.get("geom_key", -1))
			if gkey != int(node.get_meta("geom_key")):
				var g: Dictionary = \
					bridge.get_freefall_object_geometry(slot, 0)
				if not g.is_empty():
					_ff_bind_mesh(body, g)
				node.set_meta("geom_key", gkey)
			# Core-authoritative world basis — +0xac verbatim.
			node.transform = o["transform"]
		# +0x306 — the chute attachment: a second kind-2 entry under
		# the object's own basis while the flag is set.
		var chute: MeshInstance3D = node.get_node_or_null("Chute")
		if presented and bool(o.get("chute", false)):
			if chute == null:
				chute = MeshInstance3D.new()
				chute.name = "Chute"
				chute.set_meta("geom_key", -1)
				node.add_child(chute)
			var ck := int(o.get("chute_geom_key", -1))
			if ck != int(chute.get_meta("geom_key")):
				var cg: Dictionary = \
					bridge.get_freefall_object_geometry(slot, 1)
				if not cg.is_empty():
					_ff_bind_mesh(chute, cg)
				chute.set_meta("geom_key", ck)
			chute.visible = true
		elif chute != null:
			chute.visible = false
	for child in $FreefallRoot.get_children():
		var slot := int(child.name.trim_prefix("FFObj_"))
		if not live.has(slot):
			child.queue_free()

	_update_ff_debug(ff)


func _update_ff_debug(ff: Dictionary) -> void:
	if not $DebugUI.visible:
		return
	var objs: Array = bridge.get_freefall_object_snapshots()
	var pline := ""
	var pd: Dictionary = ff.get("player", {})
	if not pd.is_empty():
		var pp: Vector3 = pd["pos_mdk"]
		pline = ("\nplayer pos_mdk %.1f %.1f %.1f  yaw %.1f roll %.1f" +
			"  anim h%d acc %.2f f%d s%d") % [pp.x, pp.y, pp.z,
			float(pd["yaw_deg"]), float(pd["roll_deg"]),
			int(pd["anim_handle"]), float(pd["anim_acc"]),
			int(pd["anim_frame"]), int(pd["anim_sentinel"])]
	var cp: Vector3 = ff["camera_pos_mdk"]
	$DebugUI/DebugLabel.text = (
		"FREEFALL c%d s%d phase %d t=%.2f hp=%d fade=%.2f->%.2f\n" %
		[int(ff["course"]), int(ff["skill"]), int(ff["phase"]),
		float(ff["timeline"]), int(ff["health"]),
		float(ff["fade"]), float(ff["fade_target"])] +
		"intro %d zoom %d/%d  cam_mdk %.1f %.1f %.1f  objs %d" %
		[int(ff["intro_countdown"]), int(ff["zoom_frame"]),
		int(ff["zoom_sub"]), cp.x, cp.y, cp.z, objs.size()] + pline)


func _run_smoke_freefall(course: int, skill: int, seed: int) -> void:
	# Headless freefall smoke — asserts the presentation contract
	# end-to-end: mode 2 load, intro cadence, the spawned KURT model
	# node with live vertex animation, camera/F0V, material decode,
	# steering, and the completion handoff into traversal.
	print("smoke(freefall): course=%d skill=%d seed=%08x" %
		[course, skill, seed])
	_check(int(bridge.get_mode()) == 2, "mode == 2 (freefall)")
	# Phase 17B.2 — the traversal HUD contract is mode-3 only: the
	# snapshot is empty and no HUD/view surface is presented.
	_check(bridge.get_hud_snapshot().is_empty(),
		"hud: no snapshot in mode 2")
	_apply_freefall()
	_check(not $HudLayer/HudRect.visible and
		not $BezelLayer/BezelRect.visible and
		not $ScopeLayer/ScopeRect.visible,
		"hud: no traversal HUD over freefall")
	var f0: Dictionary = bridge.get_freefall_snapshot()
	_check(not f0.is_empty(), "freefall snapshot non-empty")
	_check(int(f0["phase"]) == 0, "phase == intro at load")
	# The _ready settle frame consumed one frameStep (150 -> 149).
	_check(int(f0["intro_countdown"]) >= 140 and
		int(f0["intro_countdown"]) <= 150, "intro countdown ~150")
	_check(int(f0["course"]) == course and int(f0["skill"]) == skill,
		"course/skill echoed")
	_check(PackedByteArray(f0["palette"]).size() == 768 and
		bool(f0["palette_ok"]), "FALLP palette bound (768B)")
	_check(abs(float(f0["zoom"]) - 2.4) < 1e-4,
		"zoom == 2.4 (0x540b58 boot value)")
	var ct0: Transform3D = f0["camera"]
	_check(abs(ct0.basis.determinant() - 1.0) < 1e-3,
		"freefall camera basis orthonormal")
	_check(abs(float(f0["fov_deg"]) - 71.36) < 0.5,
		"freefall fov ~= 71.36 deg")

	# Intro cadence: the 0x4edcb8 countdown ticks by frameStep; the
	# player is absent until the t<90 spawn edge.
	for i in 30:
		_step_n({}, 1)
	var f1: Dictionary = bridge.get_freefall_snapshot()
	_check(int(f1["intro_countdown"]) < 150 and
		int(f1["intro_countdown"]) > 0,
		"intro countdown ticking")
	_check(int(f1["phase"]) == 0, "still intro mid-countdown")

	# Past the countdown spawn edge — the type-0 player enters the
	# active list with the KURT model bound.
	for i in 80:
		_step_n({}, 1)
	var f2: Dictionary = bridge.get_freefall_snapshot()
	_check(int(f2["list_head"]) >= 0, "player spawned (list head)")
	var pd2: Dictionary = f2.get("player", {})
	_check(not pd2.is_empty() and bool(pd2.get("alive", false)),
		"player record alive")
	var objs2: Array = bridge.get_freefall_object_snapshots()
	_check(objs2.size() >= 1, "active objects enumerated")
	var pl := {}
	for o in objs2:
		if int(o["type"]) == 0:
			pl = o
	_check(not pl.is_empty(), "type-0 player in object list")
	if not pl.is_empty():
		# model_slot 1 = the KURT roster entry (model_name is the
		# record's first material-table entry — "CB3" — not the BNI
		# record name).
		_check(int(pl["model_slot"]) == 1,
			"player model slot == KURT (1)")
		_check(int(pl["model"]) == 1,
			"player model tag == kFfModelKurt")
		_check(int(pl["elem_count"]) == 18,
			"KURT elems == 18 (BNI census)")
		_check(int(pl["vert_count"]) == 227 and
			int(pl["tri_count"]) == 363,
			"KURT geometry 227v/363t")
		_check(int(pl["anim_handle"]) == 1,
			"player bound to KURTANIM (handle 1)")
		var pxf: Transform3D = pl["transform"]
		_check(abs(pxf.basis.determinant() - 1.0) < 1e-3,
			"player basis orthonormal")
		# The node tree is populated by the apply path — smoke steps
		# bypass _process, so run one apply explicitly.
		_apply_freefall()
		var pslot := int(pl["pool_slot"])
		var pnode := $FreefallRoot.get_node_or_null(
			"FFObj_%d" % pslot)
		_check(pnode != null and pnode.visible,
			"KURT node presented under FreefallRoot")
		if pnode != null:
			var pbody: MeshInstance3D = pnode.get_node("Body")
			_check(pbody.mesh != null and
				pbody.mesh.get_surface_count() > 0,
				"KURT node carries a surfaced mesh")
		# Live vertex animation: the twin driver mutates elemVerts,
		# so geom_key must move while the clip plays.
		var gk0 := int(pl["geom_key"])
		var moved := false
		for i in 40:
			_step_n({}, 1)
			var pl_now := {}
			for o in bridge.get_freefall_object_snapshots():
				if int(o["type"]) == 0:
					pl_now = o
			if not pl_now.is_empty() and \
					int(pl_now["geom_key"]) != gk0:
				moved = true
				break
		_check(moved, "KURTANIM mutates verts (geom_key churn)")

	# Materials: texture + index/pen + miss paths through the
	# FALL3D_<c+1>.MTI bank.
	var mcb3: Dictionary = bridge.get_freefall_material("CB3")
	_check(bool(mcb3.get("valid", false)) and mcb3["tex"] != null,
		"CB3 resolves to a texture material")
	var mpen: Dictionary = bridge.get_freefall_material("PEN_16")
	_check(bool(mpen.get("valid", false)) and
		int(mpen.get("palette_index", -1)) == 16,
		"PEN_16 resolves to flat palette index 16")
	# A name with no digits cannot resolve — the PEN_<n> digit
	# convention is the only non-MTI route.
	_check(bridge.get_freefall_material("NO_SUCH_MATERIAL").is_empty(),
		"unresolved material name -> empty")

	# Steering — the digital fold (left=-X) moves the player on the
	# control-axis within the 0..30s window.
	var pxy0: Vector3 = pl.get("pos_mdk", Vector3()) \
		if not pl.is_empty() else Vector3()
	_step_n({"left": true}, 12)
	var p3m: Vector3 = Vector3()
	for o in bridge.get_freefall_object_snapshots():
		if int(o["type"]) == 0:
			p3m = o["pos_mdk"]
	_check(abs(p3m.x - pxy0.x) > 0.01 or abs(p3m.y - pxy0.y) > 0.01,
		"direction input moves the player")

	# Fade field sanity — the damage flash (>1) may legitimately
	# appear during play; the state is asserted via the machine's
	# own fields, not the overlay.
	var f3: Dictionary = bridge.get_freefall_snapshot()
	_check(float(f3["fade"]) >= 0.0, "fade in range")

	# Completion handoff — run the course out. The exit branch is
	# health-gated (OBSERVED 0x541554): >0 -> traversal (mode 3), <=0
	# -> the death fade ends into the frontend route (mode 0). Which
	# route a given course/skill/seed takes is the runtime's call —
	# assert the route's consistency, not a forced survival.
	var done := false
	var route := -1
	var seen_types := {}
	var seen_anims := {}
	var saw_chute := false
	for i in 1400:
		var r: Dictionary = _step_n({}, 1)
		for o in bridge.get_freefall_object_snapshots():
			seen_types[int(o["type"])] = true
			if int(o["type"]) == 0:
				seen_anims[int(o["anim_handle"])] = true
			if bool(o.get("chute", false)):
				saw_chute = true
		if bool(r.get("done", false)):
			done = true
			route = int(r.get("handoff_route", -1))
			break
	_check(done, "freefall course completed")
	print("  types seen: %s  player anims: %s  chute: %s" %
		[seen_types.keys(), seen_anims.keys(), saw_chute])
	_check(seen_types.has(0), "type-0 player enumerated")
	_check(seen_anims.has(1), "KURTANIM bound during play")
	var fend: Dictionary = bridge.get_freefall_snapshot()
	# bones_course (0x4edaf4) is the course>=4 flyby gate.
	if bool(fend.get("bones_course", false)):
		_check(seen_types.has(5), "bones flyby on course>=4")
	else:
		_check(not seen_types.has(5),
			"no bones flyby below course 4")
	var died := bool(fend.get("died", false))
	_check(route == 0 or route == 1, "handoff route resolved")
	_check(died == (route == 1),
		"death state == frontend route (health gate)")
	if route == 0:
		# Traversal route — FUN_004346e8's load already ran inside the
		# handoff; a couple of steps make the runtime live.
		_step_n({}, 2)
		_check(int(bridge.get_mode()) == 3,
			"handoff installed mode 3 (traversal)")
		var tp: Dictionary = bridge.get_player_snapshot()
		_check(not tp.is_empty(),
			"traversal snapshot live post-handoff")
		# Phase 17B.2 — the traversal runtime behind the handoff
		# composes its own HUD; the presentation rebuilds it.
		var hh: Dictionary = bridge.get_hud_snapshot()
		_check(not hh.is_empty(),
			"hud: snapshot live post-handoff")
		if not hh.is_empty():
			_check(int(hh["w"]) == 600 and int(hh["h"]) == 360,
				"hud: 600x360 post-handoff")
		_apply_camera_snapshot()
		_apply_hud_snapshot()
		_check($HudLayer/HudRect.visible,
			"hud: overlay rebuilt post-handoff")
	else:
		_check(int(bridge.get_mode()) == 0,
			"death route -> mode 0 (frontend)")

	print("smoke(freefall): %d failure(s)" % failures)


func _update_debug_label() -> void:
	if not $DebugUI.visible:
		return
	var p: Dictionary = bridge.get_player_snapshot()
	if p.is_empty():
		return
	var mp: Vector3 = p["pos_mdk"]
	var dsp: Dictionary = bridge.get_display_snapshot()
	var portal := ""
	if not dsp.is_empty():
		portal = ("\ncur %s(%d)  partner %s(%d)  active %s  " +
			"portal->%d  crossed %d  migrated %d  objs %d") % [
			String(dsp["cur_name"]), int(dsp["cur_arena"]),
			String(dsp["partner_name"]), int(dsp["partner_arena"]),
			dsp["partner_active"], int(dsp["portal_candidate"]),
			int(dsp["portals_crossed"]),
			int(dsp["object_migrations"]),
			bridge.get_object_snapshots().size()]
	var anim := ""
	if not last_kurt.is_empty():
		var anm := "-"
		var amd: Dictionary = last_kurt["main"]
		if not amd.is_empty():
			anm = "%s[%d]" % [String(amd["table_name"]),
				int(amd["frame"])]
		var aov := "-"
		var aod: Dictionary = last_kurt["overlay"]
		if not aod.is_empty():
			aov = "%s[%d]" % [String(aod["table_name"]),
				int(aod["frame"])]
		anim = ("\nanim %s ov %s  anchor %d,%d  scope %d  " +
			"scale %d  drawn %s  tex %d") % [
			anm, aov, int(last_kurt["anchor_x"]),
			int(last_kurt["anchor_y"]), int(last_kurt["scope_ofs"]),
			int(last_kurt["scale"]), last_kurt["drawn"],
			int(last_kurt["tex_cache"])]
	$DebugUI/DebugLabel.text = (
		"pos_mdk %.2f %.2f %.2f   yaw %.1f  pitch %.1f\n" %
		[mp.x, mp.y, mp.z, p["yaw_deg"], p["pitch_deg"]] +
		"grounded %s  loco %d  arena %d->%d\n" %
		[p["grounded"], p["loco_state"], p["arena"],
		p["arena_display"]] +
		"vel move %.2f  strafe %.2f  vert %.2f  turn %.2f" %
		[p["move_vel"], p["strafe_vel"], p["vert_vel"],
		p["turn_vel"]] + portal + anim + _combat_diag_text())


func _input_mask() -> int:
	var m := 0
	if Input.is_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT):
		m |= ACT_TURN_LEFT
	if Input.is_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT):
		m |= ACT_TURN_RIGHT
	if Input.is_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP):
		m |= ACT_FORWARD
	if Input.is_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN):
		m |= ACT_BACK
	if Input.is_key_pressed(KEY_Q):
		m |= ACT_STRAFE_LEFT
	if Input.is_key_pressed(KEY_E):
		m |= ACT_STRAFE_RIGHT
	if Input.is_key_pressed(KEY_SPACE):
		m |= ACT_JUMP
	if Input.is_key_pressed(KEY_SHIFT):
		m |= ACT_TURBO
	if Input.is_key_pressed(KEY_R):
		m |= ACT_LOOK_UP
	if Input.is_key_pressed(KEY_F):
		m |= ACT_LOOK_DOWN
	return m


func _mouse_button_bits() -> int:
	# DIMOUSESTATE nibble: bit i = physical button i held
	# (0=LMB, 1=RMB, 2=MMB, 3=XBUTTON1 — the 4th device button).
	var b := 0
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		b |= 1
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		b |= 2
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE):
		b |= 4
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_XBUTTON1):
		b |= 8
	return b


func _input(event: InputEvent) -> void:
	if event is InputEventMouseMotion:
		# Raw device deltas — sign/units are the DIMOUSESTATE domain
		# (x+ right, y+ toward the user); the core's configured
		# scales own all sensitivity semantics.
		mouse_dx += int(round(event.relative.x))
		mouse_dy += int(round(event.relative.y))
	elif event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			mouse_dz += int(round(event.factor))
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			mouse_dz -= int(round(event.factor))
		elif interactive and \
				Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_ESCAPE:
			if interactive:
				if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
					Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
				else:
					get_tree().quit(0)
			else:
				get_tree().quit(0)
		elif event.keycode == KEY_F1:
			$CollisionDebug.visible = not $CollisionDebug.visible
		elif event.keycode == KEY_F2:
			obj_debug = not obj_debug
			$ObjectDebugRoot.visible = obj_debug
			if not obj_debug:
				for oid in obj_wires.keys():
					_clear_object_debug(oid)
		elif event.keycode == KEY_F3:
			$DebugUI.visible = not $DebugUI.visible
		elif event.keycode == KEY_F4:
			# Development-only proxy: the old capsule/marker/wire
			# stand-in, hidden since the sprite presenter replaced it.
			proxy_debug = not proxy_debug
			$PlayerRoot/DebugBody.visible = proxy_debug
			$PlayerRoot/ForwardMarker.visible = proxy_debug
			$PlayerBoxWire.visible = proxy_debug


func _process(delta: float) -> void:
	if bridge == null:
		# Extension missing — _ready already quit(1); quit() still
		# pumps one more iteration before the engine exits.
		return
	if shot_frames_left > 0:
		shot_frames_left -= 1
		if shot_frames_left == 0:
			var vt := get_viewport().get_texture()
			if vt == null:
				printerr("screenshot: no viewport texture — ",
					"renderer does not expose a framebuffer ",
					"(dummy/headless).")
				get_tree().quit(2)
				return
			var img := vt.get_image()
			if img == null or img.is_empty():
				printerr("screenshot: framebuffer readback empty")
				get_tree().quit(2)
				return
			var err := img.save_png(shot_path)
			print("screenshot -> ", shot_path, " err=", err,
				" size=", img.get_width(), "x", img.get_height())
			get_tree().quit(0 if err == OK else 1)
			return
	if frames_left > 0:
		frames_left -= 1
		if frames_left == 0:
			print("frames: startup proof complete")
			get_tree().quit(0)
			return
	var mode := int(bridge.get_mode())
	if shot_path.is_empty() and (mode == 2 or mode == 3):
		# Screenshot mode keeps the exact frame-0 spawn pose.
		var input := {
			"actions": _input_mask(),
			"mouse_dx": mouse_dx,
			"mouse_dy": mouse_dy,
			"mouse_dz": mouse_dz,
			"mouse_buttons": _mouse_button_bits(),
		}
		mouse_dx = 0
		mouse_dy = 0
		mouse_dz = 0
		input.merge(_ff_input(), true)
		if combat_demo and mode == 3:
			# MMB edge at frame 4 scopes in; LMB holds from frame 10
			# — the cadence decay runs while scoped, then the shot
			# fires and the bullet-cam window/impact path render.
			demo_frame += 1
			if demo_frame == 4:
				input["mouse_buttons"] = 4
			elif demo_frame >= 10:
				input["mouse_buttons"] = 1
		bridge.step_frame_input(delta * 1000.0, input)
		# The freefall->traversal handoff can flip the mode inside the
		# step — re-read so the apply path follows the live runtime.
		# (Traversal sessions are always mode != 2; only a session
		# that STARTED in mode 2 logs the transition.)
		mode = int(bridge.get_mode())
		if freefall and mode != 2 and not ff_handoff_seen:
			ff_handoff_seen = true
			print("mdk-godot: freefall handoff -> mode %d" % mode)
	if mode == 2:
		# Mode-2 freefall — the FUN_004109d8 model walk + the
		# FUN_004123f4 fixed-orientation camera + 0x4edc04 fade.
		_apply_freefall()
		return
	if mode == 0:
		# Frontend route (post-death handoff or unload) — the mode-0
		# shell is a documented seam; freeze the last frame.
		return
	# Mode 3 (traversal — reached directly or via the handoff).
	if $FreefallRoot.visible:
		$FreefallRoot.visible = false
		$FadeLayer/FadeRect.visible = false
	_apply_player_snapshot()
	_apply_camera_snapshot()
	_apply_object_snapshots()
	_apply_kurt_snapshot()
	# Phase 17A — combat presentation. Order mirrors the original:
	# the combat FX drain runs once per rendered frame; shot windows
	# and shard lifetimes refresh on the same cadence.
	_apply_shot_snapshots()
	_drain_combat_fx()
	_tick_combat_fx(delta)
	# Phase 17B.2 — the composed HUD overlay + bezel/scope view. All
	# state comes from the post-step snapshot; nothing is derived
	# from device input here.
	_apply_hud_snapshot()
	_update_debug_label()
	# Rebuild the presented arena set only when the display digest
	# changes — a BSP-order change from camera movement, a portal
	# swap, or a partner attach/detach all fold into it.
	var d = bridge.get_display_digest()
	if d != last_display_digest:
		_apply_arena_snapshots()


func _step_n(input: Dictionary, n: int, dt_ms: float = 33.333) -> Dictionary:
	var res := {}
	for i in n:
		res = bridge.step_frame_input(dt_ms, input)
	return res


func _wait_rest(max_frames: int = 60) -> void:
	# Idle-step until the player is grounded with exact-zero vertical
	# velocity — the state the original jump gate requires.
	for i in max_frames:
		var p: Dictionary = bridge.get_player_snapshot()
		if bool(p["grounded"]) and float(p["vert_vel"]) == 0.0:
			return
		_step_n({}, 1)


func _run_smoke(data_root: String) -> void:
	print("smoke: data_root=", data_root)
	_check(bridge.is_level_loaded(), "level loaded")
	_check(bridge.is_arena_loaded(), "arena loaded")
	_check(bridge.get_arena_names().size() == 19, "arena count == 19")

	var snap: Dictionary = bridge.get_arena_render_snapshot()
	_check(not snap.is_empty(), "snapshot non-empty")
	var st: Dictionary = snap["stats"]
	_check(st["vert_count"] == 234, "verts == 234")
	_check(st["poly_count"] == 399, "polys == 399")
	_check(st["name_count"] == 20, "material names == 20")
	_check(st["resolved"] == 20 and st["missing"] == 0,
		"materials resolved 20/20")
	_check(st["textured"] == 193, "textured polys == 193")
	_check(st["pen"] == 203, "pen polys == 203")
	_check(st["unresolved"] == 3, "unresolved polys == 3")
	_check(st["submitted_count"] == 296, "submitted == 296 @ spawn cam")
	_check(st["texture_count"] == 11,
		"expanded textures == 11 (12 slots referenced, NONE is an index record)")

	var pos: PackedVector3Array = snap["positions"]
	var uvs: PackedVector2Array = snap["uvs"]
	var desc: PackedFloat32Array = snap["matdesc"]
	var order: PackedInt32Array = snap["poly_order"]
	_check(pos.size() == 296 * 3, "positions == submitted*3")
	_check(uvs.size() == pos.size() and desc.size() == pos.size() * 4,
		"uv/matdesc arrays aligned")
	_check(order.size() == 296, "poly_order == submitted")
	_check(snap["palette"].size() == 768, "palette 768 bytes")
	var img: Image = snap["atlas_image"]
	_check(img != null and img.get_width() == int(st["atlas_w"]) and
		img.get_height() == int(st["atlas_h"]), "atlas image sized")
	var mesh: ArrayMesh = snap["mesh"]
	_check(mesh != null and mesh.get_surface_count() == 1,
		"single-surface ordered mesh")

	# Coordinate conversion — proven spike goldens: MDK player
	# (-4, 0, 190) -> Godot (0, 190, 4); camera (-3.17, -7.92, 195.06)
	# -> (7.92, 195.06, 3.17).
	var cam: Dictionary = bridge.get_camera_snapshot()
	var cp: Vector3 = cam["position"]
	_check(cp.distance_to(Vector3(7.92468166, 195.058044, 3.16708231))
		< 0.01, "camera pos MDK->Godot")
	var t: Transform3D = cam["transform"]
	_check(abs(t.basis.determinant() - 1.0) < 1e-3,
		"camera basis orthonormal")
	_check(abs(float(cam["fov_deg"]) - 71.36) < 0.5,
		"fov ~= 71.36 deg from scaleY")
	var player: Dictionary = bridge.get_player_snapshot()
	_check(player["pos"].distance_to(Vector3(0.0, 190.0, 4.0)) < 0.01,
		"player pos MDK->Godot")

	# ---- G2: player presentation snapshot + transform ----
	_check(abs(float(player["yaw_deg"]) - 96.0) < 1.0,
		"spawn yaw ~96 deg")
	var pt: Transform3D = player["transform"]
	_check(pt.origin.distance_to(player["pos"]) < 1e-4,
		"player transform origin == snapshot pos")
	var yaw := deg_to_rad(float(player["yaw_deg"]))
	# MDK forward (cos yaw, sin yaw, 0) -> Godot (-sin yaw, 0, -cos yaw).
	var fwd_expect := Vector3(-sin(yaw), 0.0, -cos(yaw))
	_check((-pt.basis.z).distance_to(fwd_expect) < 1e-4,
		"player -Z == converted MDK forward")
	# MDK right (sin yaw, -cos yaw, 0) -> Godot (cos yaw, 0, -sin yaw).
	var right_expect := Vector3(cos(yaw), 0.0, -sin(yaw))
	_check(pt.basis.x.distance_to(right_expect) < 1e-4,
		"player +X == converted MDK right")
	_check(abs(pt.basis.determinant() - 1.0) < 1e-3,
		"player basis orthonormal")
	_check($PlayerRoot.global_transform.is_equal_approx(pt),
		"PlayerRoot transform == core snapshot")
	# box = the standing body extents (0x540c30..44 as the mode-3
	# tail rebuilt them): pos+-1.25 x/y, pos.z..pos.z+4.25 in MDK
	# space -> 2.5x4.25x2.5 Godot AABB rooted at the feet.
	var pbox: AABB = player["box"]
	_check(pbox.size.distance_to(Vector3(2.5, 4.25, 2.5)) < 0.01,
		"player collision box extents")
	_check(abs(pbox.position.y - player["pos"].y) < 0.01,
		"player box base at feet")
	_check(player["grounded"] == true, "spawn grounded")
	_check(int(player["arena"]) == int(player["arena_display"]),
		"display arena == core arena at spawn")

	# ---- 16B: authoritative Kurt sprite presentation ----
	var k0: Dictionary = bridge.get_kurt_snapshot()
	_check(not k0.is_empty(), "kurt snapshot non-empty")
	_check(int(k0["decoded_tables"]) == 23,
		"K_ tables decoded == 23 (LEVEL3S.SNI carries none)")
	_check(int(k0["missing_tables"]) == 6,
		"6 SNI tables absent on LEVEL3")
	_check(Array(k0["table_errors"]).is_empty(),
		"no K_ table decode errors")
	print("kurt spawn: drawn=%s reg=%s anchor=%d,%d scope=%d scale=%d "
		% [k0["drawn"], k0["registered"], int(k0["anchor_x"]),
			int(k0["anchor_y"]), int(k0["scope_ofs"]),
			int(k0["scale"])] +
		"depth=%.3f view=%.3f,%.3f screen=%.2f,%.2f clip=%d" %
		[float(k0["depth"]), float(k0["view_x"]),
			float(k0["view_y"]), float(k0["screen_x"]),
			float(k0["screen_y"]), int(k0["clip"])])
	_check(k0["drawn"] == true, "kurt drawn at spawn (idle)")
	_check(k0["registered"] == true,
		"kurt registration gate open")
	var m0: Dictionary = k0["main"]
	_check(not m0.is_empty(), "kurt main frame resolved")
	if not m0.is_empty():
		# The spawn post is the dispatcher tail's seeded roll
		# (OBSERVED 0x463f37): FUN_00401ed4(100) < 5 posts K_IDLE
		# (0x65), >= 5 posts K_STILL (0x64). LEVEL3's post-load
		# rngState deterministically lands the 95% branch.
		_check(String(m0["table_name"]) == "K_STILL",
			"spawn anim table == K_STILL (seeded 95% branch)")
		_check(int(m0["w"]) > 0 and int(m0["h"]) > 0,
			"main frame dims")
		_check(m0["tex"] != null, "main ImageTexture built")
	_check(int(k0["anchor_x"]) > 0 and int(k0["anchor_x"]) < 600 and
		int(k0["anchor_y"]) > 0 and int(k0["anchor_y"]) < 360,
		"anchor inside the 600x360 space")
	# 0x540dbc = rint(probe_y - anchor_y) — the OBSERVED signed
	# screen-y delta of pos+(0,0,1) (negative when the probe lands
	# above the anchor, which is the common pose).
	_check(int(round(float(k0["probe_y"]) - float(k0["anchor_y"])))
		== int(k0["scale"]),
		"scale == rint(probe_y - anchor_y) (FUN_00431300 probe)")
	_check(int(k0["scale"]) != 0, "scale probe non-zero")
	_check(Dictionary(k0["overlay"]).is_empty(),
		"no overlay at idle")
	var kv_main: TextureRect = $KurtLayer/KurtViewport/KurtMain
	var kv_ov: TextureRect = $KurtLayer/KurtViewport/KurtOverlay
	_check(kv_main.visible and kv_main.texture != null,
		"KurtMain quad presents a texture")
	_check(not kv_ov.visible, "KurtOverlay hidden at idle")
	_check(kv_main.get_index() > kv_ov.get_index(),
		"main composites over overlay (overlay drawn first)")
	_check(not $PlayerRoot/DebugBody.visible and
		not $PlayerRoot/ForwardMarker.visible and
		not $PlayerBoxWire.visible,
		"debug proxy hidden in normal presentation")

	# Deterministic digests — the mdk-inspect folds. Golden values:
	# geom f1cc72cbe4056174 (camera-independent); order 9ff16337ea1582ec
	# at the frame-0 spawn camera (-3.16708231, -7.92468166, 195.058044).
	_check(st["geom_digest_hex"] == "f1cc72cbe4056174",
		"geom digest == mdk-inspect")
	_check(st["order_digest_hex"] == "9ff16337ea1582ec",
		"order digest == mdk-inspect @ spawn cam")
	# Emit the digests verbatim — pytest greps these against the
	# mdk-inspect fold.
	print("geom_digest=%s" % st["geom_digest_hex"])
	print("order_digest=%s" % st["order_digest_hex"])

	# ---- G2: collision debug snapshot (before any movement) ----
	var col: Dictionary = bridge.get_collision_snapshot()
	_check(not col.is_empty(), "collision snapshot non-empty")
	_check(int(col["poly_count"]) == 399 and
		int(col["vert_count"]) == 234,
		"collision poly/vert counts == render counts")
	var lines: PackedVector3Array = col["lines"]
	_check(lines.size() == 399 * 6,
		"collision line soup == polys * 6 verts")

	# ---- G2: configured mouse path (factory W-set) ----
	var icfg: Dictionary = bridge.get_input_config()
	_check(icfg["mouse_axes_map"] == "ABG", "mouse axes map == ABG")
	_check(icfg["mouse_on"] == true, "mouse enabled")
	var masks: PackedInt32Array = icfg["mouse_button_masks"]
	_check(masks.size() == 4 and masks[0] == 1 and masks[1] == 4 and
		masks[2] == 2 and masks[3] == 0,
		"mouse button masks == {fire,jump,sniper,none}")

	# ---- Phase 17B.2: HUD / view presentation ----
	# Canonical digest — mdk-inspect --traversal-runtime's scripted
	# 60-frame stream (idle x10 -> KeyUp(103) x20 -> idle x5 ->
	# KeyJump(56) x15 -> idle x10) needs the load-time state the
	# read-only asserts above preserved; reload to get it back
	# (also the level-transition path: shutdown drops the HUD/bezel
	# cache, so this run double-checks texture rebuild).
	_check(bridge.load_level("TRAVERSE/LEVEL3/LEVEL3.DTI"),
		"hud: reload for canonical run")
	_check(bridge.load_arena("HMO_1"), "hud: arena rebind")
	# The load path composes once at load; one settle step matches
	# _ready's contract (the settle IS the diagnostic's frame 0).
	bridge.step_frame_input(0.0, {})
	for f in range(1, 60):
		var keys := PackedInt32Array()
		if f >= 10 and f < 30:
			keys = PackedInt32Array([103])
		elif f >= 35 and f < 50:
			keys = PackedInt32Array([56])
		bridge.step_frame_input(1000.0 / 30.0, {"keys": keys})
	var h0: Dictionary = bridge.get_hud_snapshot()
	_check(not h0.is_empty(), "hud: snapshot non-empty")
	_check(int(h0["w"]) == 600 and int(h0["h"]) == 360,
		"hud: framebuffer 600x360")
	var fb0: PackedByteArray = h0["fb"]
	_check(fb0.size() == 216000, "hud: 216000 indexed bytes")
	# Copy integrity — the digest folds the same buffer the bridge
	# copies out, stays stable across repeat snapshot calls, and
	# matches the mdk-inspect canonical fold for this exact runtime
	# state (the fnv1a64 fold itself is covered by mdk_tests).
	print("  hud: digest=%016x nz=%d" % [int(h0["digest"]),
		int(h0["nz"])])
	_check(int(h0["digest"]) == 0x36e1ab03a2f649ab,
		"hud: canonical composed digest @60f")
	_check(int(h0["nz"]) > 0, "hud: nontransparent pens composed")
	# The palette-expanded texture: 600x360, pen 0 -> alpha 0,
	# nonzero pens -> opaque palette colors.
	var ht0 = h0["tex"]
	_check(ht0 != null and ht0.get_width() == 600 and
		ht0.get_height() == 360, "hud: texture 600x360")
	var himg: Image = ht0.get_image()
	var pal: PackedByteArray = bridge.get_active_palette()
	var z_ok := true
	var c_ok := true
	for i in range(0, fb0.size(), 997):
		var p := himg.get_pixel(i % 600, i / 600)
		if fb0[i] == 0:
			if p.a8 != 0:
				z_ok = false
		else:
			var pen := int(fb0[i]) * 3
			if p.a8 != 255 or p.r8 != int(pal[pen]) or \
					p.g8 != int(pal[pen + 1]) or \
					p.b8 != int(pal[pen + 2]):
				c_ok = false
	_check(z_ok, "hud: pen 0 expands transparent")
	_check(c_ok, "hud: pens expand through the active palette")
	# Texture reuse — the same ImageTexture object persists across
	# calls; uploads only advance on content/palette change.
	var up0 := int(h0["tex_uploads"])
	var h0b: Dictionary = bridge.get_hud_snapshot()
	_check(h0b["tex"] == ht0, "hud: texture object reused")
	_check(int(h0b["tex_uploads"]) == up0,
		"hud: no re-upload on unchanged frame")
	_check(int(h0b["digest"]) == int(h0["digest"]),
		"hud: digest stable across calls")
	# Bezel — the 640x480 SNIPERS1 buffer, raw indexed + expanded.
	var bz0: Dictionary = h0["bezel"]
	_check(not bz0.is_empty(), "hud: bezel present")
	if not bz0.is_empty():
		_check(int(bz0["w"]) == 640 and int(bz0["h"]) == 480,
			"hud: bezel 640x480")
		_check(PackedByteArray(bz0["px"]).size() == 640 * 480,
			"hud: bezel bytes 307200")
		_check(int(bz0["w"]) - int(h0["w"]) == 2 * int(bz0["fb_ofs"].x)
			and int(bz0["h"]) - int(h0["h"]) ==
			int(bz0["fb_ofs"].y) + 65,
			"hud: bezel fb offset consistent")
		var bt0 = bz0["tex"]
		_check(bt0 != null and bt0.get_width() == 640 and
			bt0.get_height() == 480, "hud: bezel texture 640x480")
		_check(h0b["bezel"]["tex"] == bt0,
			"hud: bezel texture reused")
	# Scalar echoes — the composed pixels already carry this state;
	# the fields exist so tests can prove the presentation tracks
	# core without re-deriving anything.
	_check(h0.has("health") and h0.has("wpn0") and
		h0.has("wpn1") and h0.has("ammo") and
		h0.has("inv_count") and h0.has("inv_sel") and
		h0.has("inv_timer") and h0.has("timer") and
		h0.has("timer_latch") and h0.has("level_id"),
		"hud: scalar echo fields present")
	_check(PackedInt32Array(h0["ammo"]).size() == 6,
		"hud: 6 ammo slots echoed")
	_check(int(h0["health"]) == 150, "hud: seeded health echoed")
	# Weapon-scan exercise — itemNext (internal code 27) is an edge
	# input that scans wpnSel1 forward, skipping ammo<=0 slots and
	# landing on 0 (unconditional). Predict the target from the
	# echoed ammo and prove the echo follows the core write.
	var ammo0: PackedInt32Array = h0["ammo"]
	var w10 := int(h0["wpn1"])
	_step_n({"keys": PackedInt32Array([27])}, 1)
	var hn0: Dictionary = bridge.get_hud_snapshot()
	var wscan := 0
	for w in range(w10 + 1, 6):
		if int(ammo0[w]) > 0:
			wscan = w
			break
	_check(int(hn0["wpn1"]) == wscan,
		"hud: wpn1 echo tracks the itemNext weapon scan")
	# itemPrev scans the other way; weapon-0 hotkey (code 2) then
	# re-pins the pending selection so the reload below starts at
	# the same pending state the spawn had anyway.
	_step_n({"keys": PackedInt32Array([26])}, 1)
	_step_n({"keys": PackedInt32Array([2])}, 1)
	_check(int(bridge.get_hud_snapshot()["wpn1"]) == 0,
		"hud: weapon-0 hotkey is unconditional")
	# View gates at spawn — unscoped normal view.
	_check(not bool(h0["scoped"]) and not bool(h0["sniper_view"]),
		"hud: unscoped at rest")
	var vr0: Rect2i = h0["view_rect"]
	_check(vr0.size.x == 600 and vr0.size.y == 360,
		"hud: full-frame view rect unscoped")
	var srect: Rect2i = h0["scope_rect"]
	_check(srect.position == Vector2i(108, 80) and
		srect.size == Vector2i(384, 280),
		"hud: authored aperture rect")
	# The overlay node presents only when a snapshot is live; the
	# smoke path bypasses _process, so apply explicitly.
	_apply_hud_snapshot()
	_check($HudLayer/HudRect.visible and
		$HudLayer/HudRect.texture == ht0,
		"hud: overlay node shows the composed texture")
	_check(not $BezelLayer/BezelRect.visible and
		not $ScopeLayer/ScopeRect.visible,
		"hud: no bezel/scope while unscoped")
	# Reload once more — the traversal checks below assume the
	# pristine spawn pose/RNG, which the canonical stream consumed.
	_check(bridge.load_level("TRAVERSE/LEVEL3/LEVEL3.DTI"),
		"hud: reload back to spawn")
	_check(bridge.load_arena("HMO_1"), "hud: spawn arena rebind")
	bridge.step_frame_input(0.0, {})
	player = bridge.get_player_snapshot()

	# ---- G2 deterministic traversal checks (dt fixed -> exact) ----
	# Order matters — several checks need the pristine rest pose or
	# open space:
	#   * isolation needs a static core position -> run at spawn;
	#   * W runs along the spawn corridor's open axis first;
	#   * the strafe checks then turn ~90 deg so the lateral axis
	#     lines up with that same corridor — BOTH perpendicular
	#     sides of the platform are bounded by climbable steps
	#     (~0.3 units of free run), so an unturned strafe is
	#     collision-deflected almost immediately;
	#   * the original jump gate requires exact vertVel==0 &&
	#     grounded -> run before any risky walk, then re-arm the
	#     jump latch via the proven look event for the RMB check.
	var base: Vector3 = player["pos"]
	$PlayerRoot.global_transform = Transform3D(Basis(),
		Vector3(999, -999, 777))
	_step_n({}, 2)
	var p_iso: Dictionary = bridge.get_player_snapshot()
	_check(Vector3(p_iso["pos"]).distance_to(base) < 0.01,
		"PlayerRoot writes cannot reach core state")
	_apply_player_snapshot()  # restore the visible pose

	# Mouse X -> 'A' turn: dx>0 turns right (yaw decreases).
	var yaw0: float = player["yaw_deg"]
	_step_n({"mouse_dx": 24}, 4)
	var y1: float = bridge.get_player_snapshot()["yaw_deg"]
	_check(y1 < yaw0 - 0.05, "mouse dx>0 turns right (yaw decreases)")
	_step_n({"mouse_dx": -24}, 4)
	var y2: float = bridge.get_player_snapshot()["yaw_deg"]
	_check(y2 > y1 + 0.05, "mouse dx<0 turns left (yaw increases)")
	_step_n({}, 6)

	# Mouse Y -> 'B' move: dy>0 backward (moveVel < 0), dy<0 forward.
	# Runs at spawn — the lower floor's landing surface can carry
	# the movement-blocker flag, which legitimately gates the move
	# channel later in the sequence.
	_step_n({"mouse_dy": 24}, 6)
	_check(float(bridge.get_player_snapshot()["move_vel"]) < -0.05,
		"mouse dy>0 moves backward (moveVel<0)")
	_step_n({"mouse_dy": -24}, 6)
	_check(float(bridge.get_player_snapshot()["move_vel"]) > 0.05,
		"mouse dy<0 moves forward (moveVel>0)")
	_step_n({}, 8)

	# W — forward: the move channel goes positive and displacement
	# runs along the facing (-Z basis column). 8 held frames cover
	# the accel ramp (one-frame input latency costs the first).
	_step_n({"actions": ACT_FORWARD}, 8)
	var p1: Dictionary = bridge.get_player_snapshot()
	var disp: Vector3 = Vector3(p1["pos"]) - base
	var fwd_amt := disp.dot(-pt.basis.z)
	_check(float(p1["move_vel"]) > 0.1, "W drives moveVel > 0")
	_check(fwd_amt > 0.5, "W moves player forward")
	_check(abs(disp.dot(pt.basis.x)) < fwd_amt,
		"W displacement dominantly forward")
	_step_n({}, 6)  # let the move channel decay

	# Turn ~90 deg left (bounded) so strafing below travels the
	# corridor axis instead of the stepped platform edges. The
	# +82 break lands the facing within ~8 deg of the corridor's
	# inverse, so the lateral axis stays inside the walked line.
	var yaw_turn0: float = p1["yaw_deg"]
	for i in 60:
		_step_n({"actions": ACT_TURN_LEFT}, 1)
		if (float(bridge.get_player_snapshot()["yaw_deg"]) >
				yaw_turn0 + 82.0):
			break
	_step_n({}, 6)

	# E — strafe right along the corridor (the axis W just
	# walked). The spawn platform's open reach is ~2 units and it
	# ends in a drop, so displacement is accumulated over GROUNDED
	# frames only — airborne frames carry fall momentum rather
	# than the strafe channel's work. What the check proves: the
	# strafe channel produced lateral displacement in the right
	# direction, dominant over the longitudinal component, while
	# the sweep had ground under it.
	var sb: Basis = bridge.get_player_snapshot()["transform"].basis
	base = bridge.get_player_snapshot()["pos"]
	var lat := 0.0
	var lon := 0.0
	var gnd_frames := 0
	var strafe_tables := {}
	for i in 6:
		_step_n({"actions": ACT_STRAFE_RIGHT}, 1)
		var cur: Dictionary = bridge.get_player_snapshot()
		if cur["grounded"]:
			var d: Vector3 = cur["pos"] - base
			lat += d.dot(sb.x)
			lon += abs(d.dot(-sb.z))
			gnd_frames += 1
		var km5: Dictionary = bridge.get_kurt_snapshot()["main"]
		if not km5.is_empty():
			strafe_tables[String(km5["table_name"])] = true
		base = cur["pos"]
	var p3: Dictionary = bridge.get_player_snapshot()
	_check(float(p3["strafe_vel"]) > 0.1,
		"E drives strafeVel > 0 (right)")
	_check(gnd_frames >= 3 and lat > 0.15 and lon < lat,
		"E displacement lateral-right dominant")
	_check(strafe_tables.has("K_SIDE"),
		"E strafe -> K_SIDE main frame")
	_step_n({}, 6)
	_wait_rest()  # land wherever the strafe ended up

	# Q — strafe left: the opposite lateral sign on whatever
	# floor the player now stands (the lower floor is open).
	sb = bridge.get_player_snapshot()["transform"].basis
	base = bridge.get_player_snapshot()["pos"]
	lat = 0.0
	lon = 0.0
	gnd_frames = 0
	for i in 6:
		_step_n({"actions": ACT_STRAFE_LEFT}, 1)
		var cur2: Dictionary = bridge.get_player_snapshot()
		if cur2["grounded"]:
			var d: Vector3 = cur2["pos"] - base
			lat += -d.dot(sb.x)
			lon += abs(d.dot(-sb.z))
			gnd_frames += 1
		base = cur2["pos"]
	var p2: Dictionary = bridge.get_player_snapshot()
	_check(float(p2["strafe_vel"]) < -0.1,
		"Q drives strafeVel < 0 (left)")
	_check(gnd_frames >= 3 and lat > 0.15 and lon < lat,
		"Q displacement lateral-left dominant")
	_step_n({}, 6)

	# Space — jump: vertical channel rises / player leaves ground.
	_wait_rest()  # the gate needs grounded && exact vertVel==0
	base = bridge.get_player_snapshot()["pos"]
	var rose := false
	var air_tables := {}
	for i in 8:
		_step_n({"actions": ACT_JUMP}, 1)
		var pj: Dictionary = bridge.get_player_snapshot()
		if float(pj["vert_vel"]) > 1.0 or \
				Vector3(pj["pos"]).y > base.y + 0.3:
			rose = true
		var km2: Dictionary = bridge.get_kurt_snapshot()["main"]
		if not km2.is_empty():
			air_tables[String(km2["table_name"])] = true
	_check(rose, "Space initiates jump (vertical rise)")
	# Standing jump -> K_JUMP (0x2be); a descent that exhausts the
	# vertVel threshold table releases to K_FALL (0x2bc). Either is
	# the core-selected airborne table.
	_check(air_tables.has("K_JUMP") or air_tables.has("K_RJMP") or
		air_tables.has("K_FALL"),
		"airborne -> K_JUMP/K_RJMP/K_FALL main frame")
	# Grounded on a jump/fall frame -> locoState 0xc8 + K_LAND frame
	# 0 the same tick; the table then plays out per frameStep. Step
	# while grounded to observe it (replaces _wait_rest here).
	var land_seen := false
	for i in 60:
		var p5: Dictionary = bridge.get_player_snapshot()
		var km3: Dictionary = bridge.get_kurt_snapshot()["main"]
		if not km3.is_empty() and \
				String(km3["table_name"]) == "K_LAND":
			land_seen = true
		if bool(p5["grounded"]) and float(p5["vert_vel"]) == 0.0:
			for j in 12:
				_step_n({}, 1)
				var km4: Dictionary = bridge.get_kurt_snapshot()["main"]
				if not km4.is_empty() and \
						String(km4["table_name"]) == "K_LAND":
					land_seen = true
			break
		_step_n({}, 1)
	_check(land_seen, "landing -> K_LAND main frame")

	# Re-arm the jump gate for the RMB check below. After a SOFT
	# landing the core posts no event, so locoState stays latched
	# on a jump code and jumpActive holds — the original clears it
	# only when the dispatched state leaves {0x2be,0x2bf}, which
	# needs a priority-8 event (motion/turn are only 6). A held
	# look key posts the proven 0x324 look event and unlatches it.
	_step_n({"actions": ACT_LOOK_DOWN}, 3)
	_step_n({}, 4)

	# Mouse buttons -> configured action masks (echo of the merged
	# 0x4ce control block — proves the bits reach consumeGameplayInput).
	# Runs on the lower floor while the player is at rest: RMB's
	# mapped-jump check needs the same grounded gate as Space.
	var ir: Dictionary = bridge.step_frame_input(0.0,
		{"mouse_buttons": 1})  # LMB -> mask 1 = kBtnFire
	_check(ir["input"]["fire"] == true, "LMB -> mapped fire flag")
	bridge.step_frame_input(0.0, {})
	_wait_rest()
	base = bridge.get_player_snapshot()["pos"]
	rose = false
	for i in 8:
		_step_n({"mouse_buttons": 2}, 1)  # RMB -> mask 4 = kBtnJump
		var pj2: Dictionary = bridge.get_player_snapshot()
		if float(pj2["vert_vel"]) > 1.0 or \
				Vector3(pj2["pos"]).y > base.y + 0.3:
			rose = true
	_check(rose, "RMB -> mapped jump (visible rise)")
	_wait_rest()
	ir = bridge.step_frame_input(0.0, {"mouse_buttons": 4})
	_check(ir["input"]["sniper_pulse"] == true,
		"MMB -> sniper pulse edge")
	ir = bridge.step_frame_input(0.0, {"mouse_buttons": 4})
	_check(ir["input"]["sniper_pulse"] == false,
		"sniper edge held -> no repeat")
	# Unscope — the entry pulse above leaves the player scoped: the
	# scope phases advance one per frame and a pulse arriving while
	# ca0==0 is swallowed, so the held-check's second edge could not
	# toggle back out. Idle lets the scope settle; a fresh edge then
	# runs the proven manual-unscope branch (FUN_0046ca84 seam).
	_step_n({}, 6)
	_step_n({"mouse_buttons": 4}, 1)
	_step_n({}, 6)
	ir = bridge.step_frame_input(0.0, {"mouse_buttons": 8})
	_check(not ir["input"]["fire"] and not ir["input"]["jump"] and
		not ir["input"]["sniper_pulse"],
		"button4 unmapped (factory mask 0)")
	_step_n({}, 4)

	# A — turn left: yaw increases (positive turnVel decreases yaw;
	# left turn = turnVel < 0).
	var yaw_a0: float = bridge.get_player_snapshot()["yaw_deg"]
	_step_n({"actions": ACT_TURN_LEFT}, 8)
	var p4: Dictionary = bridge.get_player_snapshot()
	_check(float(p4["yaw_deg"]) > yaw_a0 + 0.05,
		"A turns left (yaw increases)")

	# ---- G3: dynamic objects — the HMO_9 XGS (real spawn record,
	# real RuntimeModel geometry). Diagnostic re-anchor mirrors
	# mdk-inspect's --arena/--start selftest path.
	# OBSERVED (Phase 15A): FUN_0045bac0's kill plane is the arena's
	# REAL +0x44e deepFloorZ - 200, not a flat -200 — HMO_9's minZ is
	# -361 so its plane is -561 and the XGS at z=-293 survives. The
	# snapshot checks run before the first stepped frame; the stepped
	# check afterwards asserts survival ABOVE the real plane (the
	# below-plane reap is exercised by the connector transients and
	# pinned natively).
	var ds9: Dictionary = bridge.diagnostic_start(8,
		Vector3(-174.0, 2635.0, -293.0), 0.0)
	_check(ds9.get("ok", false), "diagnostic_start into HMO_9")
	var dsp9: Dictionary = bridge.get_display_snapshot()
	_check(int(dsp9["cur_arena"]) == 8, "display cur == HMO_9")
	var objs: Array = bridge.get_object_snapshots()
	_check(objs.size() == 1, "HMO_9 enumerates 1 object")
	var oid := -1
	var o_plane := 0.0
	var o_z := 0.0
	if objs.size() == 1:
		var o: Dictionary = objs[0]
		oid = int(o["id"])
		o_plane = float(o["floor_plane"])
		o_z = float(o["pos_mdk"].z)
		_check(String(o["enemy_name"]) == "XGS",
			"enemy-table name == XGS")
		_check(String(o["model"]) == "XG_BOD",
			"RuntimeModel name table == XG_BOD")
		_check(int(o["enemy_index"]) == 30 and
			int(o["spawn_id"]) == 9, "XGS enemy 30 spawn 9")
		_check(int(o["arena"]) == 8, "object arena == 8")
		_check(int(o["elem_count"]) == 25, "XGS elem_count == 25")
		_check(oid > 0 and oid < 0x1000000,
			"opaque object id (counter, not a pointer)")
		var t0: Transform3D = o["transform"]
		_check(abs(t0.basis.determinant() - 1.0) < 1e-3,
			"object basis orthonormal")
		var omp: Vector3 = o["pos_mdk"]
		_check(omp.distance_to(Vector3(-174.0, 2635.0, -293.0)) < 0.5,
			"XGS pos_mdk == spawn record")
		# AABB conversion: godot min = (-maxy, minz, -maxx).
		var ab: PackedFloat32Array = o["aabb_mdk"]
		var ga: AABB = o["aabb"]
		_check(abs(ga.position.x + ab[4]) < 1e-3 and
			abs(ga.position.y - ab[2]) < 1e-3 and
			abs(ga.position.z + ab[3]) < 1e-3,
			"object AABB MDK->Godot")
		# Real RuntimeModel geometry — one surface per element.
		var g: Dictionary = bridge.get_object_geometry(oid)
		_check(not g.is_empty(), "object geometry resolved")
		if not g.is_empty():
			_check(int(g["elem_count"]) == 25,
				"geometry elem_count == 25")
			_check(int(g["vert_count"]) == int(o["vert_count"]) and
				int(g["tri_count"]) == int(o["tri_count"]),
				"geometry counts == snapshot counts")
			_check(int(g["geom_key"]) == int(o["geom_key"]),
				"geom_key == snapshot key")
			var gm: ArrayMesh = g["mesh"]
			_check(gm != null and gm.get_surface_count() ==
				PackedInt32Array(g["surface_elems"]).size(),
				"mesh surfaces == surface_elems")
			print("first_model=%s elems=%d verts=%d tris=%d" % [
				String(g["model"]), int(g["elem_count"]),
				int(g["vert_count"]), int(g["tri_count"])])
		# Presentation node mirrors the snapshot transform.
		_apply_object_snapshots()
		var onode := $DynamicObjectRoot.get_node_or_null(
			"Object_%d" % oid)
		_check(onode != null, "Object_<id> node exists")
		if onode != null:
			_check(onode.transform.is_equal_approx(t0),
				"object node transform == snapshot")
			_check(onode.get_child_count() ==
				PackedInt32Array(g["surface_elems"]).size(),
				"object node has per-element meshes")
		# Stale/unknown ids resolve to nothing — never an alias.
		_check(bridge.get_object_geometry(0).is_empty() and
			bridge.get_object_geometry(999999).is_empty(),
			"stale/unknown ids -> empty geometry")
		# F2 object debug — AABB wire + label per object.
		obj_debug = true
		_apply_object_snapshots()
		_check(obj_wires.has(oid) and obj_tags.has(oid),
			"object debug wire+tag created")
		obj_debug = false
		_clear_object_debug(oid)
		_check(not obj_wires.has(oid) and not obj_tags.has(oid),
			"object debug cleared")
		# Element-disable mask: XGS mask is 0 -> every element shows.
		for c in onode.get_children():
			_check(c.visible,
				"elem_mask=0 -> all elements visible")
	# OBSERVED lifecycle (corrected Phase-15A semantics): the kill
	# plane is the arena's REAL deepFloorZ - 200, not a flat -200 —
	# HMO_9's AABB minZ is -361, so the plane is -561 and the XGS at
	# z=-293 sits ABOVE it: it survives and stays enumerated, as it
	# does in MDK95. The below-plane death + FUN_0045cf18 reap is
	# exercised live by the CHMO_2 connector-door transients below
	# and pinned natively (above/below-plane boundary test).
	_check(o_plane < o_z,
		"XGS sits above the real kill plane (deepFloorZ-200)")
	_step_n({}, 3)
	var _objs_left: Array = bridge.get_object_snapshots()
	_check(_objs_left.size() == 1 and
		int(_objs_left[0]["id"]) == oid,
		"above-plane object survives (FUN_0045bac0, real floor)")

	# ---- G3: corridor door + portal crossing + arena transfer ----
	# CHMO_2 has no MTO render block — the partner's geometry carries
	# the view while its script-spawned XCORDOOR door presents from
	# the corridor's own object list. Walking +y crosses the proven
	# y=1237 portal into HMO_3 (arena 11 -> 2).
	# OBSERVED lifecycle on this route (FUN_004572ac pass order +
	# FUN_0045bac0 kill plane + FUN_0045cf18 reap): the corridor door
	# spawns at z=-935 — below CHMO_2's real deepFloorZ-200 plane —
	# so each connector is
	# a ~2-frame transient: the CHMO_2 door opens (attaching HMO_3)
	# and dies; HMO_3's script then respawns a fresh connector (the
	# dedup scan sees only named records, so the corpse cannot
	# suppress it); that door self-migrates toward cur and dies too.
	# The authentic enumeration contract is therefore sequential
	# transient doors — at most one live connector per snapshot, and
	# an id that leaves the enumeration never returns — not a single
	# stable id.
	var ds11: Dictionary = bridge.diagnostic_start(11,
		Vector3(3.0, 1230.0, -929.0), 90.0)
	_check(ds11.get("ok", false), "diagnostic_start into CHMO_2")
	var door_id := int(-1)
	var door_arena0 := -1
	var door_states := {}
	var door_ids_seen := {}
	var door_gone := {}           # ids that have left the enumeration
	var door_multi := false       # >1 live connector in one snapshot
	var door_revived := false     # a vanished connector id returned
	var live_door := -1           # connector id enumerated last frame
	var corridor_checked := false
	var crossed := false
	var run_tables := {}
	var dspc: Dictionary = bridge.get_display_snapshot()
	for i in 90:
		_step_n({"actions": ACT_FORWARD}, 1)
		dspc = bridge.get_display_snapshot()
		# 16B: a grounded corridor run posts eventMag 0x258 -> K_RUN.
		var kmr: Dictionary = bridge.get_kurt_snapshot()["main"]
		if not kmr.is_empty():
			run_tables[String(kmr["table_name"])] = true
		var conn_id := -1
		var conn_count := 0
		for od in bridge.get_object_snapshots():
			if bool(od["connector"]):
				conn_count += 1
				conn_id = int(od["id"])
				if door_id < 0:
					door_id = conn_id
				if door_arena0 < 0:
					door_arena0 = int(od["arena"])
				door_states[int(od["conn_state"])] = true
				door_ids_seen[conn_id] = true
		if conn_count > 1:
			door_multi = true
		if conn_count == 1:
			if conn_id != live_door:
				if live_door >= 0:
					door_gone[live_door] = true
				if door_gone.has(conn_id):
					door_revived = true
				live_door = conn_id
		elif live_door >= 0:
			door_gone[live_door] = true
			live_door = -1
		if int(dspc["cur_arena"]) == 11 and not corridor_checked:
			corridor_checked = true
			_check(int(dspc["primary"]) != 11,
				"corridor cur -> partner geometry displayed")
		if int(dspc["cur_arena"]) == 2:
			crossed = true
			break
	# A grounded corridor run selects K_RUN (the spawn platform's
	# open reach is too short — W there goes airborne -> K_FALL).
	_check(run_tables.has("K_RUN"),
		"corridor run -> K_RUN main frame")
	_check(door_id >= 0, "connector door enumerated (XCORDOOR)")
	_check(door_arena0 == 11, "door starts on corridor list")
	_check(not door_multi, "at most one live connector per snapshot")
	_check(not door_revived, "dead connector id never re-enumerates")
	_check(door_states.size() >= 2, "door conn_state transitions")
	if crossed:
		_check(int(dspc["portals_crossed"]) >= 1,
			"portal crossing counted")
		# Post-crossing view: cur==HMO_3 and CHMO_2 is no longer the
		# active partner, so the corridor's object list leaves the
		# presentation set — no seen door id may alias onto another
		# arena's object.
		for od in bridge.get_object_snapshots():
			_check(not door_ids_seen.has(int(od["id"])) or
				int(od["arena"]) == 11,
				"door id never aliases another arena")
		print("  note: object_migrations=%d door_ids=%d (sequential transients)" %
			[int(dspc["object_migrations"]), door_ids_seen.size()])
	else:
		_check(false, "player crossed CHMO_2 -> HMO_3 portal")
	# Object presentation survives the arena hops without dupes.
	_apply_object_snapshots()
	var seen_ids := {}
	var dupes := false
	for od in bridge.get_object_snapshots():
		var k := int(od["id"])
		if seen_ids.has(k):
			dupes = true
		seen_ids[k] = true
	_check(not dupes, "no duplicate object ids in snapshot")

	# ---- 16B: texture-cache stability + firing overlay ----
	# Same {table,frame} re-presented -> the SAME cached
	# ImageTexture (stable identity, no per-frame rebuild).
	var seen_tex := {}
	var reused := false
	for i in 120:
		_step_n({}, 1)
		var kf: Dictionary = bridge.get_kurt_snapshot()
		var mf: Dictionary = kf["main"]
		if mf.is_empty():
			continue
		var key := "%s/%d" % [String(mf["table_name"]),
			int(mf["frame"])]
		if seen_tex.has(key):
			if seen_tex[key] == mf["tex"]:
				reused = true
		else:
			seen_tex[key] = mf["tex"]
	_check(reused, "repeated frame identity reuses ImageTexture")

	# Firing: LMB holds produce K_SHOT/K_RUNFIR main frames. The
	# standing shot state (0x12c) has NO muzzle block in the
	# original — K_MUZZF is written only by the strafe/turn/fall/
	# chute/jump branches while the c74 fire latch is held, on
	# odd-parity frames (disasm: 0x46239e / 0x462491 / ...). Turn
	# + fire keeps eventMag at 0x190, so K_TRN45 carries the
	# overlay — core-selected in both cases.
	var fired_table := false
	for i in 24:
		_step_n({"mouse_buttons": 1}, 1)
		var kb: Dictionary = bridge.get_kurt_snapshot()
		var mb: Dictionary = kb["main"]
		if not mb.is_empty() and \
				String(mb["table_name"]) in ["K_SHOT", "K_RUNFIR"]:
			fired_table = true
	_check(fired_table, "fire -> K_SHOT/K_RUNFIR main frame")
	var saw_overlay := false
	var saw_turn_fire := false
	for i in 32:
		_step_n({"actions": ACT_TURN_LEFT, "mouse_buttons": 1}, 1)
		var kb2: Dictionary = bridge.get_kurt_snapshot()
		var mb2: Dictionary = kb2["main"]
		if not mb2.is_empty() and \
				String(mb2["table_name"]) == "K_TRN45":
			saw_turn_fire = true
		var ob: Dictionary = kb2["overlay"]
		if not ob.is_empty():
			saw_overlay = true
			if String(ob["table_name"]) != "K_MUZZF":
				_check(false, "overlay table == K_MUZZF")
	_step_n({}, 8)
	_check(saw_turn_fire, "turn+fire -> K_TRN45 main frame")
	_check(saw_overlay, "muzzle overlay presented (K_MUZZF)")

	# ---- Phase 17A: traversal combat presentation -----------------
	# The scoped sniper fire path — the only shot-render path in the
	# original (0x436dd3: flagC9c && transitionPhase > 1). Re-anchor
	# in HMO_9 so a live object is enumerated for the remnant/corpse
	# events, then scope in via the real input path. Yaw 270 is a
	# probe-verified clear line — it points away from the XGS spawn
	# (yaw 90 sent the tracer straight into it, killing the object
	# before the kill-diagnostic section could enumerate it).
	var pre_c: Dictionary = bridge.get_player_snapshot()
	var dsc: Dictionary = bridge.diagnostic_start(8,
		Vector3(-174.0, 2625.0, -293.0), 270.0)
	_check(dsc.get("ok", false), "combat: diagnostic_start into HMO_9")
	var ss0: Dictionary = bridge.get_shot_snapshots()
	_check(not ss0.is_empty() and ss0["shots"].size() == 3,
		"combat: shot snapshot carries the 3-slot pool")
	_step_n({"mouse_buttons": 4}, 1)     # MMB edge -> scope toggle
	_step_n({}, 8)                       # transitionPhase advances
	var ss1: Dictionary = bridge.get_shot_snapshots()
	if not bool(ss1["scoped"]):
		# A pulse arriving inside a transition can be swallowed —
		# retry the edge after settling.
		_step_n({"mouse_buttons": 4}, 1)
		_step_n({}, 8)
		ss1 = bridge.get_shot_snapshots()
	_check(bool(ss1["scoped"]), "combat: scoped after MMB pulse")
	# Phase 17B.2 — the scoped view presentation follows the same
	# core gates: sniper_view open, the authored aperture pose on the
	# camera, bezel beneath the scope viewport, overlay on top.
	var hs1: Dictionary = bridge.get_hud_snapshot()
	_check(bool(hs1["sniper_view"]), "hud: sniper_view gate open")
	var vr1: Rect2i = hs1["view_rect"]
	_check(vr1.size.x <= 400 and vr1.size.y <= 300,
		"hud: scoped view rect is the aperture size")
	_apply_hud_snapshot()
	_check($BezelLayer/BezelRect.visible,
		"hud: bezel shown while scoped")
	_check($ScopeLayer/ScopeRect.visible,
		"hud: scope viewport shown while scoped")
	_check(scope_vp.render_target_update_mode ==
		SubViewport.UPDATE_ALWAYS,
		"hud: scope viewport live while scoped")
	_check($HudLayer/HudRect.visible,
		"hud: composed overlay still on top while scoped")
	# shotWinFill — the core's per-slot indicator channel drives the
	# same window fills Phase 17A already draws.
	var wf: PackedInt32Array = hs1["win_fill"]
	_check(wf.size() == 3, "hud: win_fill carries 3 slots")
	# 0x464a56's gate is level-triggered (ctrl.fire != 0) but also
	# needs fireCadence == 0 — the earlier unscoped punches left the
	# cadence timer hot, and it only decays while scoped. Hold LMB
	# through the decay; the first shot may die the same tick (an
	# HMO_9 spawn sits on the yaw-90 line), so the state-1 transient
	# is caught per-frame rather than on the first nonzero slot.
	var ss2: Dictionary
	var live := -1
	var saw_state1 := false
	var mesh_node_ok := false
	var win_ok := false
	var fill_ok := false
	var saw_kind := {}
	var shards0 := int(fx_stats["shards"])
	for i in 90:
		_step_n({"mouse_buttons": 1}, 1)
		ss2 = bridge.get_shot_snapshots()
		_drain_combat_fx()
		for ev in fx_recent:
			saw_kind[int(ev["kind"])] = ev
		fx_recent.clear()
		_apply_shot_snapshots()
		for j in 3:
			var sj: Dictionary = ss2["shots"][j]
			if int(sj["state"]) != 0 and live < 0:
				live = j
			if int(sj["state"]) == 1:
				saw_state1 = true
				var sn := $ShotRoot.get_node_or_null("Shot_%d" % j)
				if sn != null and sn.visible and \
						sn.get_child_count() > 0:
					mesh_node_ok = true
			if bool(sj["window_active"]) and shot_wins[j].visible:
				win_ok = true
			if bool(ss2["hud_active"]) and \
					int(sj["state"]) == 0 and shot_fills[j].visible:
				fill_ok = true
		if saw_state1 and mesh_node_ok:
			break
	_check(live >= 0, "combat: scoped LMB spawned a shot")
	_check(saw_state1,
		"combat: state==1 -> world-mesh submit gate open")
	if live >= 0:
		var sv: Dictionary = ss2["shots"][live]
		_check(int(sv["class_idx"]) == -1,
			"combat: default shot binds class slot -1 (KURT)")
	_check(mesh_node_ok,
		"combat: shot world-mesh node built+visible")
	_check(win_ok, "combat: bullet-cam window texture-rect shown")
	_check(fill_ok, "combat: free-slot HUD indicator fill drawn")
	# Phase 17B.2 — weapon/ammo echoes track the post-fire core.
	# The default tracer (wpnSel0==0) consumes no ammo slot — the
	# OBSERVED decrement paths are weapons 1..4/5, none of which
	# hold ammo at this spawn (pickup-granted). Assert the echo
	# shape/values instead; the composed digits themselves are
	# covered by the canonical fb digest.
	var hf: Dictionary = bridge.get_hud_snapshot()
	var ammo1: PackedInt32Array = hf["ammo"]
	_check(ammo1.size() == 6 and int(hf["wpn0"]) >= 0 and
		int(hf["wpn0"]) <= 5 and int(hf["wpn1"]) >= 0 and
		int(hf["wpn1"]) <= 5,
		"hud: weapon/ammo echoes valid post-fire")
	var ammo_dropped := false
	for i in mini(ammo0.size(), ammo1.size()):
		if int(ammo1[i]) < int(ammo0[i]):
			ammo_dropped = true
	if ammo_dropped:
		print("  hud: ammo echo decremented post-fire")
	var sg: Dictionary = bridge.get_shot_geometry(-1)
	_check(not sg.is_empty() and int(sg["vert_count"]) > 0,
		"combat: KURT geometry resolves from STREAM.BNI")
	if live >= 0:
		# Fly to impact — a tracer dies by wall hit (kShotWallImpact),
		# object hit (kShotObjectImpact), or lifetime expiry (no
		# event). LMB released: whatever remains in the pool decays.
		for i in 90:
			_step_n({}, 1)
			_apply_shot_snapshots()
			_drain_combat_fx()
			_tick_combat_fx(1.0 / 30.0)
			for ev in fx_recent:
				saw_kind[int(ev["kind"])] = ev
			fx_recent.clear()
			var any_live := false
			for sj in bridge.get_shot_snapshots()["shots"]:
				if int(sj["state"]) != 0:
					any_live = true
			if saw_kind.has(0) or saw_kind.has(1) or not any_live:
				break
		if saw_kind.has(0) or saw_kind.has(1):
			var iev: Dictionary = saw_kind.get(0, saw_kind.get(1))
			_check(int(iev["arena_index"]) >= 0,
				"combat: impact event carries its arena")
			_check(int(fx_stats["shards"]) > shards0,
				"combat: impact spawned FUN_00404108 shards")
		# else: expired in flight — no event is the OBSERVED contract
		# for types 0/1.

	# Remnant + corpse events — the XGS (enemy 30) is the guaranteed
	# enumerated object in HMO_9. diagnostic_shockwave runs the real
	# FUN_004575fc seam; diagnostic_kill runs FUN_00458140's boundary.
	var cobj := -1
	for od in bridge.get_object_snapshots():
		cobj = int(od["id"])
		break
	_check(cobj > 0, "combat: live object enumerated for FX events")
	if cobj > 0:
		var rem0 := remnants.size()
		var wv: Dictionary = bridge.diagnostic_shockwave(cobj)
		_check(bool(wv.get("ok", false)),
			"combat: shockwave diagnostic ran")
		_drain_combat_fx()
		var det_ev := {}
		for ev in fx_recent:
			if int(ev["kind"]) == 4:
				det_ev = ev
		fx_recent.clear()
		_check(not det_ev.is_empty(),
			"combat: kDetonation event drained")
		if not det_ev.is_empty():
			_check(abs(float(det_ev["scale"]) - 2.0) < 1e-4,
				"combat: detonation remnant scale == 2.0")
			_check(det_ev.has("transform"),
				"combat: detonation carries the spawn transform")
			_check(remnants.size() > rem0,
				"combat: remnant node spawned under FxRoot")
		# Kill — the XGS binds a deathRef (+0x110), so the first
		# boundary call posts the script-handoff event; the teardown
		# (kind 6) follows when the script or a second boundary call
		# reaches the FUN_00457cf4 path.
		var shards1 := int(fx_stats["shards"])
		var rem1 := remnants.size()
		var sw0: Dictionary = bridge.diagnostic_kill(cobj)
		_check(bool(sw0.get("ok", false)),
			"combat: kill diagnostic ran on the object")
		var saw5 := false
		var saw6 := false
		for i in 40:
			_step_n({}, 1)
			_drain_combat_fx()
			for ev in fx_recent:
				var k := int(ev["kind"])
				if k == 5:
					saw5 = true
				elif k == 6:
					saw6 = true
			fx_recent.clear()
			if saw6:
				break
		if not saw6:
			# +0x110 already consumed — a second boundary call lands
			# on the teardown path directly.
			bridge.diagnostic_kill(cobj)
			_drain_combat_fx()
			for ev in fx_recent:
				if int(ev["kind"]) == 6:
					saw6 = true
			fx_recent.clear()
		_check(saw5 or saw6,
			"combat: death boundary event drained (handoff or teardown)")
		_check(saw6, "combat: kObjectTeardown event drained")
		if saw6:
			_check(int(fx_stats["shards"]) >= shards1 + TEAR_SHARDS,
				"combat: teardown emitted the 16-shard burst")
			_check(remnants.size() > rem1,
				"combat: teardown spawned the EXPLODE corpse")
	# Shard reap — the +0x196 countdown (60+(rand>>9) ticks) expires
	# every live shard; the pool empties back to zero.
	for i in 8:
		_tick_combat_fx(1.0)
		if shards.is_empty():
			break
	_check(shards.is_empty(), "combat: shards reaped on ttl expiry")
	# Unscope, then restore the pre-combat anchor — the damage/death
	# blocks below are palette-epoch sensitive (the Kurt texture
	# cache keys on the displayed arena's palette).
	for i in 4:
		if not bool(bridge.get_shot_snapshots()["scoped"]):
			break
		_step_n({"mouse_buttons": 4}, 1)
		_step_n({}, 8)
	_check(not bool(bridge.get_shot_snapshots()["scoped"]),
		"combat: unscoped for downstream blocks")
	# Phase 17B.2 — scope exit drops the bezel/aperture surfaces; the
	# composed overlay persists (it carries the unscoped HUD too).
	_apply_hud_snapshot()
	_check(not bool(last_hud["sniper_view"]),
		"hud: sniper_view closed on unscope")
	_check(not $BezelLayer/BezelRect.visible and
		not $ScopeLayer/ScopeRect.visible,
		"hud: bezel/scope hidden on unscope")
	_check(scope_vp.render_target_update_mode ==
		SubViewport.UPDATE_DISABLED,
		"hud: scope viewport idle on unscope")
	if not pre_c.is_empty():
		bridge.diagnostic_start(int(pre_c["arena"]),
			pre_c["pos_mdk"], float(pre_c["yaw_deg"]))
		_step_n({}, 8)

	# ---- 16B.1: authentic damage/death dispatch ----
	# Damage enters ONLY through the core producer
	# (diagnostic_damage -> playerDamageApply == FUN_0046771c); the
	# dispatcher (FUN_00463608, 0x463f9d..0x46422f) consumes the
	# accumulator on the NEXT frame and posts {9,0x385} or
	# {10,0x3ea}; Phase 16A selection maps those to K_BANG/K_BFLIP.
	# Nothing below writes loco/anim state from GDScript.
	var dd0: Dictionary = bridge.diagnostic_damage(0)
	var hp0 := int(dd0["health"])
	var dd1: Dictionary = bridge.diagnostic_damage(15)
	_check(bool(dd1.get("ok", false)), "damage producer applies")
	_check(int(dd1["health"]) < hp0 and int(dd1["health"]) > 0,
		"nonlethal hit reduced health")
	_check(float(dd1["accum"]) >= 5.0,
		"accumulator past the 5.0 post gate")
	# Phase 17B.2 — the damage state is already in the composed
	# buffer; the snapshot echoes the same fields the pixels show.
	var hd1: Dictionary = bridge.get_hud_snapshot()
	_check(int(hd1["health"]) == int(dd1["health"]),
		"hud: health field echoed post-hit")
	_check(int(hd1["field_dac"]) > 0,
		"hud: damage accumulator echoed post-hit")
	var saw_bang := false
	var saw_bflip := false
	var tumble_state := false
	var ks0: Dictionary = bridge.get_kurt_snapshot()
	var ktex0 := int(ks0["tex_cache"])
	var kpal0 := int(ks0["pal_key"])
	# Texture-cache invariant, measured directly: each snapshot call
	# requests {pal_key,table,frame} textures for its main/overlay
	# frames. Collect the exact key set requested during the window —
	# the cache may only grow by keys it was actually asked for, and
	# already-seen keys must reuse their ImageTexture (no re-entry).
	var collect := func(kd: Dictionary, into: Dictionary) -> void:
		if kd.is_empty():
			return
		var pk := int(kd["pal_key"])
		for part in [&"main", &"overlay"]:
			var pd: Dictionary = kd[part]
			if not pd.is_empty() and pd.get("tex") != null:
				into["%x:%d:%d" % [pk, int(pd["table"]),
						int(pd["frame"])]] = true
	var kset := {}
	for i in 130:
		_step_n({}, 1)
		var kd: Dictionary = bridge.get_kurt_snapshot()
		collect.call(kd, kset)
		if int(kd["loco_state"]) == 0x385:
			tumble_state = true
		var md: Dictionary = kd["main"]
		if not md.is_empty():
			if String(md["table_name"]) == "K_BANG":
				saw_bang = true
			elif String(md["table_name"]) == "K_BFLIP":
				saw_bflip = true
	_check(tumble_state, "dispatch posted 0x385 tumble state")
	_check(saw_bang, "tumble presented K_BANG main frames")
	_check(saw_bflip, "tumble chained into K_BFLIP")
	# Settle past the suppression window, then the lethal hit —
	# the producer refuses damage while e10>0 / loco==0x385.
	_wait_rest(120)
	var dd2: Dictionary = bridge.diagnostic_damage(500)
	_check(int(dd2["health"]) == 0, "lethal hit floored health at 0")
	# Phase 17B.2 — the SKULL/death presentation is composed into
	# the fb by the core; the overlay must visibly change and the
	# scalar echoes must track the dispatch.
	var hud_dg_pre := int(bridge.get_hud_snapshot()["digest"])
	var death_state := false
	var death_hud := false
	for i in 40:
		_step_n({}, 1)
		var ke: Dictionary = bridge.get_kurt_snapshot()
		collect.call(ke, kset)
		if int(ke["loco_state"]) == 0x3ea:
			death_state = true
		var hd: Dictionary = bridge.get_hud_snapshot()
		if int(hd["loco_state"]) == 0x3ea:
			death_hud = true
		var me: Dictionary = ke["main"]
		if not me.is_empty():
			_check(String(me["table_name"]) == "K_BANG",
				"death presents K_BANG")
			_check(bool(ke["drawn"]), "death sprite drawn")
	_check(death_state, "dispatch posted 0x3ea death state")
	_check(death_hud, "hud: snapshot echoes 0x3ea death state")
	var hd3: Dictionary = bridge.get_hud_snapshot()
	_check(int(hd3["health"]) == 0, "hud: health 0 post-lethal")
	_check(int(hd3["digest"]) != hud_dg_pre,
		"hud: composed fb changed on the death path")
	# Terminal: 0x3ea never releases; the dead-check fade countdown
	# is armed and counting (a 0-amount call is a producer no-op —
	# hp==0 && gate==0 — used here only as a live-state read).
	var dd3: Dictionary = bridge.diagnostic_damage(0)
	_check(int(dd3["loco_state"]) == 0x3ea,
		"0x3ea terminal, no re-entry")
	_check(int(dd3["event_priority"]) == 10,
		"death event priority 10")
	_check(int(dd3["fade"]) > 0, "death fade armed (eb8 countdown)")
	_check(int(hd3["field_eb8"]) != 0 or int(dd3["fade"]) > 0,
		"hud: death-fade field live")
	var ks1: Dictionary = bridge.get_kurt_snapshot()
	var ktex1 := int(ks1["tex_cache"])
	var kpal1 := int(ks1["pal_key"])
	# Growth bound: delta may never exceed the count of unique
	# {palette,table,frame} keys actually requested — any larger
	# delta means unrequested entries or duplicate inserts.
	_check(ktex1 - ktex0 <= kset.size(),
		"tex cache grew only for requested keys " +
		"(delta=%d unique_keys=%d pal=%x->%x arena=%d)" %
		[ktex1 - ktex0, kset.size(), kpal0, kpal1,
			int(bridge.get_player_snapshot()["arena"])])
	_check(ktex1 <= 4096,
		"tex cache within the 4096-entry bound")
	# Same-key reuse: a second snapshot with no intervening step
	# re-requests the identical key set -> zero new entries.
	var ktex_r := int(bridge.get_kurt_snapshot()["tex_cache"])
	_check(ktex_r == ktex1,
		"tex cache reuses identical key (delta=%d)" %
		[ktex_r - ktex1])
	# Replay: keep stepping the terminal death presentation — every
	# repeated key must hit the cache; only genuinely new
	# {palette,table,frame} tuples may add entries.
	var kset_b := {}
	var ktex_b0 := int(bridge.get_kurt_snapshot()["tex_cache"])
	for i in 40:
		_step_n({}, 1)
		collect.call(bridge.get_kurt_snapshot(), kset_b)
	var ktex_b1 := int(bridge.get_kurt_snapshot()["tex_cache"])
	var new_b := 0
	for k in kset_b:
		if not kset.has(k):
			new_b += 1
	_check(ktex_b1 - ktex_b0 <= new_b,
		"replay adds only newly-requested keys " +
		"(delta=%d new_keys=%d)" % [ktex_b1 - ktex_b0, new_b])
	var km_end: TextureRect = $KurtLayer/KurtViewport/KurtMain
	_check(km_end.visible and km_end.texture != null,
		"KurtMain still presents during death")

	# ---- Phase 17B.2: level transition ----
	# shutdown() drops the HUD/bezel cache; a reload must rebuild
	# every surface from the fresh runtime — nothing stale survives.
	var hpre: Dictionary = bridge.get_hud_snapshot()
	var tex_pre = hpre["tex"]
	_check(bridge.load_level("TRAVERSE/LEVEL3/LEVEL3.DTI"),
		"transition: LEVEL3 reload")
	_check(bridge.load_arena("HMO_1"), "transition: arena rebind")
	_step_n({}, 2)
	var hpost: Dictionary = bridge.get_hud_snapshot()
	_check(not hpost.is_empty(),
		"transition: HUD snapshot live post-reload")
	_check(int(hpost["tex_uploads"]) >= 1,
		"transition: HUD texture re-uploaded for new runtime")
	_check(hpost["tex"] != tex_pre or int(hpre["tex_uploads"]) != \
		int(hpost["tex_uploads"]),
		"transition: texture cache did not carry stale key")
	_apply_hud_snapshot()
	_check($HudLayer/HudRect.visible,
		"transition: overlay node rebuilt post-reload")
	_check(not $BezelLayer/BezelRect.visible and
		not $ScopeLayer/ScopeRect.visible,
		"transition: no stale bezel/scope post-reload")

	print("smoke: %d failure(s)" % failures)


func _run_smoke_generic(level: String, arena: String) -> void:
	# Cross-level object/display smoke — no golden numbers; verifies
	# the enumeration contract on whatever the spawn/--start view
	# holds (used for LEVEL6/LEVEL8 and diagnostic starts).
	print("smoke(generic): level=%s arena=%s" % [level, arena])
	_check(bridge.is_level_loaded(), "level loaded")
	_check(bridge.get_arena_names().size() > 0, "arena count > 0")
	# 16B — authoritative Kurt presentation on whatever level the
	# launcher loaded.
	var kg: Dictionary = bridge.get_kurt_snapshot()
	_check(not kg.is_empty(), "kurt snapshot non-empty")
	_check(int(kg["decoded_tables"]) > 0, "K_ tables decoded")
	_check(Array(kg["table_errors"]).is_empty(),
		"no K_ table decode errors")
	var mg: Dictionary = kg["main"]
	_check(not mg.is_empty() and mg["tex"] != null,
		"kurt main frame + texture")
	_check(kg["drawn"] == true, "kurt drawn")
	_check(not $PlayerRoot/DebugBody.visible, "debug proxy hidden")
	var dsp: Dictionary = bridge.get_display_snapshot()
	_check(not dsp.is_empty(), "display snapshot non-empty")
	_check(int(dsp["cur_arena"]) >= 0, "current arena resolved")
	var objs0: Array = bridge.get_object_snapshots()
	var ids := {}
	for o in objs0:
		_check(int(o["id"]) > 0 and int(o["id"]) < 0x10000000,
			"opaque object id (counter, not a pointer)")
		_check(o.has("transform") and o.has("aabb") and
			o.has("elem_count"), "object dict shape")
		ids[int(o["id"])] = true
	_step_n({}, 5)
	var objs1: Array = bridge.get_object_snapshots()
	for oid in ids.keys():
		var still := false
		for o in objs1:
			if int(o["id"]) == oid:
				still = true
		_check(still, "object id %d persists across frames" % oid)
	for o in objs1:
		var g: Dictionary = bridge.get_object_geometry(int(o["id"]))
		_check(not g.is_empty(), "geometry resolves for live id")
		if not g.is_empty():
			_check(int(g["geom_key"]) == int(o["geom_key"]),
				"geom key == snapshot key")
		var t: Transform3D = o["transform"]
		_check(abs(t.basis.determinant()) > 1e-6,
			"object basis non-degenerate")
	# Element-disable mask — when a masked object is present (e.g.
	# SW_DUMMY), its masked children must be hidden in the node.
	_apply_object_snapshots()
	for o in objs1:
		var mask := int(o["elem_mask"])
		if mask == 0:
			continue
		var mn := $DynamicObjectRoot.get_node_or_null(
			"Object_%d" % int(o["id"]))
		if mn != null:
			for c in mn.get_children():
				var e := int(c.get_meta("elem"))
				_check(c.visible == ((mask & (1 << e)) == 0),
					"elem_mask bit %d -> child visibility" % e)
	# Object nodes exist for every live id after an apply.
	_apply_object_snapshots()
	for o in objs1:
		var n := $DynamicObjectRoot.get_node_or_null(
			"Object_%d" % int(o["id"]))
		_check(n != null, "Object node for id %d" % int(o["id"]))
		if n != null:
			_check(n.transform.is_equal_approx(o["transform"]),
				"object node transform == snapshot")
	# Mover watch — type-4 flags14a&0x20 objects get their transform
	# rebuilt by the core each frame (FUN_0045612c in the update
	# pass); whether they visibly move is the driving script's seam.
	# Whatever the core transform does, the node must mirror it.
	var movers := {}
	for o in objs1:
		if bool(o["mover"]):
			movers[int(o["id"])] = o["transform"]
	if not movers.is_empty():
		_step_n({}, 20)
		var moved := 0
		for o in bridge.get_object_snapshots():
			var k := int(o["id"])
			if movers.has(k):
				if not Transform3D(o["transform"]).is_equal_approx(
						movers[k]):
					moved += 1
		print("  movers: %d watched, %d transform-changed" %
			[movers.size(), moved])
		_apply_object_snapshots()
		for o in bridge.get_object_snapshots():
			var k := int(o["id"])
			if movers.has(k):
				var n := $DynamicObjectRoot.get_node_or_null(
					"Object_%d" % k)
				_check(n != null and
					n.transform.is_equal_approx(o["transform"]),
					"mover node transform == core snapshot")
	print("smoke(generic): %d object(s) enumerated, %d failure(s)" %
		[objs1.size(), failures])
	# Phase 17A closeout — a bounded combat exercise on whatever
	# arena the launcher brought up (LEVEL6/LEVEL8 closeout runs it
	# on OLYM_1/GUNT_1). Skipped only when the player can't scope
	# (a --start anchored mid-air).
	_run_combat_exercise("generic")


func _run_combat_exercise(tag: String) -> void:
	# The same scoped-fire/drain/tick path _run_smoke exercises on
	# LEVEL3, level-agnostic: scope in, catch a state-1 shot's world
	# mesh + bullet-cam window, fly it to the OBSERVED wall/object
	# impact (kind 0/1 -> the FUN_00437444 shard burst), then run
	# the death boundary on whatever object the view set enumerates
	# (kind 5/6 -> teardown 16-shard burst + EXPLODE remnant).
	# Everything asserted here is presentation; gameplay is core's.
	var pre_c: Dictionary = bridge.get_player_snapshot()
	if not bool(pre_c.get("grounded", false)):
		print("  combat(%s): skipped — player not grounded" % tag)
		return
	# Palette sanity — the level's own composed palette feeds shard
	# colors; never a LEVEL3 carry-over.
	_apply_shot_snapshots()
	_check(fx_palette.size() == 768,
		"combat(%s): active palette resolves (768)" % tag)
	# Phase 17B.2 — the composed HUD is level-agnostic: a 600x360
	# buffer + SNIPERS1 bezel exist on every traversal level.
	var hg0: Dictionary = bridge.get_hud_snapshot()
	_check(not hg0.is_empty() and int(hg0["w"]) == 600 and
		int(hg0["h"]) == 360,
		"combat(%s): HUD snapshot 600x360" % tag)
	_check(not hg0["bezel"].is_empty(),
		"combat(%s): SNIPERS1 bezel bound" % tag)
	_apply_hud_snapshot()
	_check($HudLayer/HudRect.visible,
		"combat(%s): HUD overlay presented" % tag)
	var shards0 := int(fx_stats["shards"])
	_step_n({"mouse_buttons": 4}, 1)     # MMB edge -> scope toggle
	_step_n({}, 8)                       # transitionPhase advances
	var ss1: Dictionary = bridge.get_shot_snapshots()
	if not bool(ss1["scoped"]):
		# A pulse inside a transition can be swallowed — retry.
		_step_n({"mouse_buttons": 4}, 1)
		_step_n({}, 8)
		ss1 = bridge.get_shot_snapshots()
	_check(bool(ss1["scoped"]),
		"combat(%s): scoped after MMB pulse" % tag)
	# Phase 17B.2 — scoped view presentation on this level.
	var hg1: Dictionary = bridge.get_hud_snapshot()
	_check(bool(hg1["sniper_view"]),
		"combat(%s): sniper_view open" % tag)
	_apply_hud_snapshot()
	_check($ScopeLayer/ScopeRect.visible and
		$BezelLayer/BezelRect.visible,
		"combat(%s): scope view + bezel shown" % tag)
	var ss2 := {}
	var saw_state1 := false
	var mesh_node_ok := false
	var win_ok := false
	var saw_hud := false
	var live := -1
	for i in 90:
		_step_n({"mouse_buttons": 1}, 1)
		ss2 = bridge.get_shot_snapshots()
		_drain_combat_fx()
		fx_recent.clear()
		_apply_shot_snapshots()
		for j in 3:
			var sj: Dictionary = ss2["shots"][j]
			if int(sj["state"]) != 0 and live < 0:
				live = j
			if int(sj["state"]) == 1:
				saw_state1 = true
				var sn := $ShotRoot.get_node_or_null("Shot_%d" % j)
				if sn != null and sn.visible and \
						sn.get_child_count() > 0:
					mesh_node_ok = true
			if bool(sj["window_active"]) and shot_wins[j].visible:
				win_ok = true
			if bool(ss2["hud_active"]):
				saw_hud = true
		if saw_state1 and mesh_node_ok:
			break
	_check(live >= 0,
		"combat(%s): scoped LMB spawned a shot" % tag)
	_check(saw_state1,
		"combat(%s): state==1 -> world-mesh gate open" % tag)
	if live >= 0:
		var sv: Dictionary = ss2["shots"][live]
		_check(int(sv["class_idx"]) == -1,
			"combat(%s): default shot binds class -1 (KURT)" % tag)
	_check(mesh_node_ok,
		"combat(%s): shot world-mesh node built+visible" % tag)
	_check(win_ok,
		"combat(%s): bullet-cam window texture-rect shown" % tag)
	if saw_hud:
		var fill_ok := false
		for j in 3:
			if int(ss2["shots"][j]["state"]) == 0 and \
					shot_fills[j].visible:
				fill_ok = true
		_check(fill_ok,
			"combat(%s): free-slot HUD indicator fill drawn" % tag)
	var sg: Dictionary = bridge.get_shot_geometry(-1)
	_check(not sg.is_empty() and int(sg["vert_count"]) > 0,
		"combat(%s): KURT geometry resolves (STREAM.BNI)" % tag)
	# Enemy-side projectiles present through the same object
	# snapshot path — count any the fight spawns (BOLT family et al);
	# absence is a reachability note, not a failure.
	var enemy_proj := {}
	# Flight — bounded by the shot's full +0x11e lifetime (~240
	# ticks); kinds 0/1 are the OBSERVED wall/object impacts and
	# lifetime expiry is silent for types 0/1.
	var impact := {}
	for i in 280:
		_step_n({}, 1)
		_apply_shot_snapshots()
		_drain_combat_fx()
		_tick_combat_fx(1.0 / 30.0)
		for ev in fx_recent:
			var k := int(ev["kind"])
			if k == 0 or k == 1:
				impact = ev
		fx_recent.clear()
		for o in bridge.get_object_snapshots():
			var en := String(o["enemy_name"])
			if en.contains("BOLT"):
				enemy_proj[en] = true
		var any_live := false
		for sj in bridge.get_shot_snapshots()["shots"]:
			if int(sj["state"]) != 0:
				any_live = true
		if not impact.is_empty() or not any_live:
			break
	# The queue is one-shot — a second drain in the same frame must
	# be empty (no duplicate visual events).
	_check(bridge.drain_combat_fx().is_empty(),
		"combat(%s): combatFx drains once (no replay)" % tag)
	if not impact.is_empty():
		_check(int(impact["arena_index"]) >= 0,
			"combat(%s): impact event carries its arena" % tag)
		_check(int(fx_stats["shards"]) > shards0,
			"combat(%s): impact spawned the shard burst" % tag)
	else:
		print("  combat(%s): shot expired in flight — no impact" % tag)
	if not enemy_proj.is_empty():
		print("  combat(%s): enemy projectile(s) seen: %s" %
			[tag, str(enemy_proj.keys())])
	# Death boundary on whatever the view set enumerates — movers
	# included (the FUN_00457cf4 teardown path is generic).
	var cobj := -1
	for od in bridge.get_object_snapshots():
		cobj = int(od["id"])
		break
	if cobj > 0:
		var shards1 := int(fx_stats["shards"])
		var rem1 := remnants.size()
		var sw0: Dictionary = bridge.diagnostic_kill(cobj)
		_check(bool(sw0.get("ok", false)),
			"combat(%s): kill diagnostic ran" % tag)
		var saw5 := false
		var saw6 := false
		for i in 40:
			_step_n({}, 1)
			_drain_combat_fx()
			for ev in fx_recent:
				var k := int(ev["kind"])
				if k == 5:
					saw5 = true
				elif k == 6:
					saw6 = true
			fx_recent.clear()
			if saw6:
				break
		if not saw6:
			# +0x110 already consumed — a second boundary call lands
			# on the teardown path directly.
			bridge.diagnostic_kill(cobj)
			_drain_combat_fx()
			for ev in fx_recent:
				if int(ev["kind"]) == 6:
					saw6 = true
			fx_recent.clear()
		_check(saw5 or saw6,
			"combat(%s): death boundary event drained" % tag)
		_check(saw6,
			"combat(%s): kObjectTeardown event drained" % tag)
		if saw6:
			_check(int(fx_stats["shards"]) >= shards1 + TEAR_SHARDS,
				"combat(%s): teardown 16-shard burst" % tag)
			# The EXPLODE corpse binds a level enemy-table record —
			# only assert the node when this level carries one.
			if not bridge.get_named_geometry("EXPLODE").is_empty():
				_check(remnants.size() > rem1,
					"combat(%s): teardown EXPLODE remnant" % tag)
	else:
		print("  combat(%s): no object enumerated — kill skipped" %
			tag)
	# Reap — the +0x196 countdown expires every shard.
	for i in 8:
		_tick_combat_fx(1.0)
		if shards.is_empty():
			break
	_check(shards.is_empty(),
		"combat(%s): shards reaped on ttl expiry" % tag)
	# No duplicate combat nodes: the pool is exactly 3 slots and
	# FxRoot carries only live transients (reaped shards are
	# queue-pending until a frame boundary — excluded here).
	_check($ShotRoot.get_child_count() == 3,
		"combat(%s): shot node pool stays 3" % tag)
	var fx_live := 0
	for c in $FxRoot.get_children():
		if not c.is_queued_for_deletion():
			fx_live += 1
	_check(fx_live == remnants.size() + shards.size(),
		"combat(%s): FxRoot holds only live transients" % tag)
	# Unscope + restore the launch anchor for any later blocks.
	for i in 4:
		if not bool(bridge.get_shot_snapshots()["scoped"]):
			break
		_step_n({"mouse_buttons": 4}, 1)
		_step_n({}, 8)
	_check(not bool(bridge.get_shot_snapshots()["scoped"]),
		"combat(%s): unscoped" % tag)
	_apply_hud_snapshot()
	_check(not $ScopeLayer/ScopeRect.visible and
		not $BezelLayer/BezelRect.visible,
		"combat(%s): scope view + bezel hidden on exit" % tag)
	if not pre_c.is_empty():
		bridge.diagnostic_start(int(pre_c["arena"]),
			pre_c["pos_mdk"], float(pre_c["yaw_deg"]))
		_step_n({}, 8)


func _run_smoke_restore() -> void:
	# Phase 17A closeout — the bounded save->restore golden:
	#   combat active + live shot -> save_game_full() ->
	#   keep fighting (the timeline diverges: new FX, a death) ->
	#   restore_save() -> presentation rebuilt from the restored
	#   authoritative BULL/ALIE/DAMP state ONLY.
	# Asserts: the saved shot re-appears at its serialized state,
	# pre-restore transient FX neither survive nor replay, stale ids
	# resolve to nothing, and post-restore events render exactly
	# once. Anchored to LEVEL3 HMO_9 — the proven combat arena.
	print("smoke(restore): save->restore presentation golden")
	var dsc: Dictionary = bridge.diagnostic_start(8,
		Vector3(-174.0, 2625.0, -293.0), 270.0)
	_check(dsc.get("ok", false), "restore: diagnostic_start into HMO_9")
	_step_n({"mouse_buttons": 4}, 1)
	_step_n({}, 8)
	if not bool(bridge.get_shot_snapshots()["scoped"]):
		_step_n({"mouse_buttons": 4}, 1)
		_step_n({}, 8)
	_check(bool(bridge.get_shot_snapshots()["scoped"]),
		"restore: scoped")
	# Catch a live state-1 shot; remember its serialized fields.
	var saved_shot := {}
	var saved_slot := -1
	for i in 90:
		_step_n({"mouse_buttons": 1}, 1)
		_apply_shot_snapshots()
		var ss: Dictionary = bridge.get_shot_snapshots()
		for j in 3:
			var sj: Dictionary = ss["shots"][j]
			if int(sj["state"]) == 1:
				saved_shot = sj
				saved_slot = j
		if saved_slot >= 0:
			break
	_check(saved_slot >= 0, "restore: live shot at save time")
	var pre_ids := {}
	for od in bridge.get_object_snapshots():
		pre_ids[int(od["id"])] = true
	var stale_id: int = pre_ids.keys()[0] if not pre_ids.is_empty() \
		else -1
	# Diagnostic counts — pre-save.
	var pre_shots := 0
	for sj in bridge.get_shot_snapshots()["shots"]:
		if int(sj["state"]) != 0:
			pre_shots += 1
	print(("  restore: pre-save shots=%d objs=%d " +
		"shot_nodes=%d fx_nodes=%d") % [pre_shots, pre_ids.size(),
		$ShotRoot.get_child_count(), $FxRoot.get_child_count()])
	# ---- save ----
	var save: PackedByteArray = bridge.save_game_full()
	_check(save.size() > 100, "restore: save_game_full wrote bytes")
	# ---- diverge the timeline ----
	# More frames + LMB: the saved shot flies on (may impact — the
	# transient shards are pre-restore evidence), and a shockwave +
	# boundary kill add remnant/teardown transients.
	for i in 12:
		_step_n({"mouse_buttons": 1}, 1)
		_apply_shot_snapshots()
		_drain_combat_fx()
		_tick_combat_fx(1.0 / 30.0)
	if stale_id > 0:
		bridge.diagnostic_shockwave(stale_id)
		_drain_combat_fx()
		fx_recent.clear()
		bridge.diagnostic_kill(stale_id)
		for i in 30:
			_step_n({}, 1)
			_drain_combat_fx()
			_tick_combat_fx(1.0 / 30.0)
		fx_recent.clear()
	var fx_pre := $FxRoot.get_child_count()
	_check(fx_pre > 0,
		"restore: transient combat nodes exist pre-restore")
	# ---- restore ----
	var rep: Dictionary = bridge.restore_save(save)
	_check(bool(rep.get("ok", false)), "restore: core restore ran")
	_check(bool(rep.get("identity_ok", false)),
		"restore: CMI identity check passed")
	_check(int(rep.get("level_dir", -1)) == 3,
		"restore: save resolved to LEVEL3")
	_check(int(rep.get("shots_active", -1)) >= 1,
		"restore: BULL carried >=1 active shot")
	_reset_presentation_for_restore()
	# Teardown proved out — every transient node is gone and no
	# pre-restore event replays out of the fresh runtime.
	_check($FxRoot.get_child_count() == 0,
		"restore: no transient FX nodes survive")
	# Phase 17B.2 — no HUD/view surface survives the boundary: the
	# overlay, bezel, and scope viewport all dropped with the
	# discarded runtime's presentation.
	_check(not $HudLayer/HudRect.visible and
		not $BezelLayer/BezelRect.visible and
		not $ScopeLayer/ScopeRect.visible,
		"restore: HUD/view surfaces dropped")
	_check(last_hud.is_empty(),
		"restore: HUD snapshot cache cleared")
	_check(bridge.drain_combat_fx().is_empty(),
		"restore: no pre-restore combat events replay")
	for i in 3:
		_check(shot_nodes[i].get_child_count() == 0 and
			not shot_nodes[i].visible,
			"restore: shot slot %d cleared" % i)
	# Stale ids resolve to nothing — the old object's id must not
	# run a boundary call on the restored world.
	if stale_id > 0:
		var stale: Dictionary = bridge.diagnostic_kill(stale_id)
		_check(not bool(stale.get("ok", false)),
			"restore: stale object id does not resolve")
	# ---- rebuilt presentation from the restored runtime ----
	var rss: Dictionary = bridge.get_shot_snapshots()
	_apply_shot_snapshots()
	var rshot := {}
	var rslot := -1
	var rlive := 0
	for j in 3:
		var sj: Dictionary = rss["shots"][j]
		if int(sj["state"]) != 0:
			rlive += 1
			if int(sj["state"]) == 1 and rslot < 0:
				rslot = j
				rshot = sj
	_check(rlive >= 1,
		"restore: a BULL-restored shot is live again")
	# The saved slot's shot re-appears at its serialized pos —
	# post-save flight never happened on this timeline.
	if saved_slot >= 0 and rslot >= 0:
		_check(rslot == saved_slot,
			"restore: restored shot keeps its pool slot")
		var rp: Vector3 = rshot["pos_mdk"]
		var sp: Vector3 = saved_shot["pos_mdk"]
		_check(rp.distance_to(sp) < 0.01,
			"restore: restored shot at its serialized pos")
		_check(int(rshot["class_idx"]) ==
			int(saved_shot["class_idx"]),
			"restore: restored shot keeps class binding")
	if rslot >= 0:
		var sn := $ShotRoot.get_node_or_null("Shot_%d" % rslot)
		_check(sn != null and sn.visible and
			sn.get_child_count() > 0,
			"restore: restored shot world-mesh node built")
	# The discarded post-save timeline leaves no phantom shots —
	# slot liveness comes verbatim from BULL state.
	var phantom := 0
	for j in 3:
		var sj: Dictionary = rss["shots"][j]
		var sn := $ShotRoot.get_node_or_null("Shot_%d" % j)
		var shown: bool = sn != null and sn.visible
		if shown != (int(sj["state"]) == 1):
			phantom += 1
	_check(phantom == 0,
		"restore: shot nodes mirror BULL state exactly")
	# Authoritative objects rebuilt — the node set mirrors the
	# restored view set exactly (ids re-mint from an empty map;
	# counter values are session-scoped, so the pre/post id sets are
	# not comparable — the NODES are what matter).
	_step_n({}, 2)
	_apply_object_snapshots()
	var post_ids := {}
	for od in bridge.get_object_snapshots():
		var oid := int(od["id"])
		post_ids[oid] = true
		var nd := $DynamicObjectRoot.get_node_or_null(
			"Object_%d" % oid)
		_check(nd != null,
			"restore: object node rebuilt for id %d" % oid)
	_check(post_ids.size() > 0,
		"restore: restored objects enumerated")
	_check($DynamicObjectRoot.get_child_count() == post_ids.size(),
		"restore: object nodes == restored snapshot set")
	# Phase 17B.2 — the HUD/view rebuilds from the restored runtime:
	# the save was taken scoped, so the restored core state reopens
	# the aperture and the composed fb is live again.
	_apply_camera_snapshot()
	_apply_hud_snapshot()
	_check(not last_hud.is_empty() and $HudLayer/HudRect.visible,
		"restore: HUD overlay rebuilt from restored state")
	# Whatever gate the restored core reports, the presentation
	# mirrors it — the save was taken scoped, so this exercises the
	# aperture path when the restored phase says so.
	_check($ScopeLayer/ScopeRect.visible ==
		bool(last_hud["sniper_view"]),
		"restore: scope viewport follows restored sniper_view")
	_check($BezelLayer/BezelRect.visible ==
		(bool(last_hud["sniper_view"]) and
			not last_hud["bezel"].is_empty()),
		"restore: bezel follows restored sniper_view")
	print(("  restore: post shots=%d objs=%d " +
		"shot_nodes=%d fx_nodes=%d geom_cache=%d") %
		[rlive, post_ids.size(), $ShotRoot.get_child_count(),
		$FxRoot.get_child_count(), geom_cache.size()])
	# ---- post-restore events render exactly once ----
	# A fresh wall impact: the restored shot (or a new one) dies
	# against geometry — one event, one burst, queue empty after.
	var shards_r0 := int(fx_stats["shards"])
	var got_hit := false
	for i in 300:
		_step_n({"mouse_buttons": 1}, 1)
		_apply_shot_snapshots()
		_drain_combat_fx()
		_tick_combat_fx(1.0 / 30.0)
		for ev in fx_recent:
			var k := int(ev["kind"])
			if k == 0 or k == 1:
				got_hit = true
		fx_recent.clear()
		if got_hit:
			break
	_check(got_hit,
		"restore: post-restore impact event drained")
	_check(int(fx_stats["shards"]) > shards_r0,
		"restore: post-restore impact spawned shards once")
	_check(bridge.drain_combat_fx().is_empty(),
		"restore: post-restore drain is one-shot")
	# A fresh death boundary on a restored object — the 5/6 event
	# sequence and the corpse appear exactly once.
	var nobj := -1
	for od in bridge.get_object_snapshots():
		nobj = int(od["id"])
		break
	if nobj > 0:
		var rem_r := remnants.size()
		var shards_r1 := int(fx_stats["shards"])
		bridge.diagnostic_kill(nobj)
		var saw6 := false
		for i in 40:
			_step_n({}, 1)
			_drain_combat_fx()
			for ev in fx_recent:
				if int(ev["kind"]) == 6:
					saw6 = true
			fx_recent.clear()
			if saw6:
				break
		if not saw6:
			bridge.diagnostic_kill(nobj)
			_drain_combat_fx()
			for ev in fx_recent:
				if int(ev["kind"]) == 6:
					saw6 = true
			fx_recent.clear()
		_check(saw6,
			"restore: post-restore teardown event drained once")
		if saw6:
			_check(int(fx_stats["shards"]) >=
				shards_r1 + TEAR_SHARDS,
				"restore: post-restore teardown burst once")
			_check(remnants.size() == rem_r + 1,
				"restore: post-restore remnant exactly once")
	# Display set + palette rebound to the restored level.
	_apply_arena_snapshots()
	_check($ArenaRoot.get_child_count() > 0,
		"restore: arena nodes rebuilt for the display set")
	_check(bridge.get_active_palette().size() == 768,
		"restore: active palette rebound (768)")
	print("smoke(restore): %d failure(s)" % failures)
