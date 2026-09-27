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
#   --smoke           headless deterministic check, then quit
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

# Raw mouse accumulators — device deltas for the next frame only.
var mouse_dx := 0
var mouse_dy := 0
var mouse_dz := 0

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
	if not bridge.load_level(level):
		printerr("MdkBridge.load_level failed: ", bridge.get_last_error())
		get_tree().quit(1)
		return
	if not bridge.load_arena(arena):
		printerr("MdkBridge.load_arena failed: ", bridge.get_last_error())
		get_tree().quit(1)
		return
	if not start.is_empty():
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

	# One idle frame settles the deterministic spawn camera.
	bridge.step_frame_input(0.0, {})
	_apply_arena_snapshots()
	_apply_object_snapshots()
	_apply_player_snapshot()
	_apply_camera_snapshot()
	_apply_kurt_snapshot()
	_update_debug_label()

	if smoke:
		if level == "TRAVERSE/LEVEL3/LEVEL3.DTI" and \
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

	print(("mdk-godot: arena=%s arenas=%d  (WASD/QE move+strafe, " +
		"AD turn, RF look, Space jump, mouse=captured, F1 collision, " +
		"F2 object debug, Esc release/quit)") %
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
		p["turn_vel"]] + portal + anim)


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
	if shot_path.is_empty():
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
		bridge.step_frame_input(delta * 1000.0, input)
	_apply_player_snapshot()
	_apply_camera_snapshot()
	_apply_object_snapshots()
	_apply_kurt_snapshot()
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
		_check(String(m0["table_name"]) == "K_IDLE",
			"spawn anim table == K_IDLE")
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
	var saw_bang := false
	var saw_bflip := false
	var tumble_state := false
	var ktex0 := int(bridge.get_kurt_snapshot()["tex_cache"])
	for i in 130:
		_step_n({}, 1)
		var kd: Dictionary = bridge.get_kurt_snapshot()
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
	var death_state := false
	for i in 40:
		_step_n({}, 1)
		var ke: Dictionary = bridge.get_kurt_snapshot()
		if int(ke["loco_state"]) == 0x3ea:
			death_state = true
		var me: Dictionary = ke["main"]
		if not me.is_empty():
			_check(String(me["table_name"]) == "K_BANG",
				"death presents K_BANG")
			_check(bool(ke["drawn"]), "death sprite drawn")
	_check(death_state, "dispatch posted 0x3ea death state")
	# Terminal: 0x3ea never releases; the dead-check fade countdown
	# is armed and counting (a 0-amount call is a producer no-op —
	# hp==0 && gate==0 — used here only as a live-state read).
	var dd3: Dictionary = bridge.diagnostic_damage(0)
	_check(int(dd3["loco_state"]) == 0x3ea,
		"0x3ea terminal, no re-entry")
	_check(int(dd3["event_priority"]) == 10,
		"death event priority 10")
	_check(int(dd3["fade"]) > 0, "death fade armed (eb8 countdown)")
	var ktex1 := int(bridge.get_kurt_snapshot()["tex_cache"])
	_check(ktex1 - ktex0 < 64,
		"texture cache bounded through damage/death")
	var km_end: TextureRect = $KurtLayer/KurtViewport/KurtMain
	_check(km_end.visible and km_end.texture != null,
		"KurtMain still presents during death")

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
