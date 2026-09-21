extends Node3D

# Phase 8 (G2) acceptance scene: load LEVEL3, render the HMO_1 arena
# through the MdkBridge GDExtension, and show the player as a debug
# proxy driven purely by traversal-runtime snapshots. mdk_core owns
# data/semantics; this script only wires returned snapshots into
# Node3D objects — nothing here integrates motion or drives state.
#
# Per frame:
#   raw device input -> bridge.step_frame_input -> core step
#   -> player snapshot -> PlayerRoot transform (presentation only)
#
# CLI (after --):
#   --data-path DIR   data root (default: MDK_DATA_ROOT env, else
#                     <repo>/original/installed relative to res://)
#   --level RELDTI    default TRAVERSE/LEVEL3/LEVEL3.DTI
#   --arena NAME      default HMO_1 ("" -> spawn arena)
#   --smoke           headless deterministic check, then quit
#   --screenshot P    after 8 frames, save a PNG capture then quit
#                     (requires a real renderer — not --headless)
#   --frames N        run N process frames then quit (startup proof)
#
# Keys: WASD move, Q/E strafe, A/D turn, R/F look up/down, Space
# jump, Shift turbo, F1 collision wire toggle, F3 debug text toggle.
# Interactive runs capture the mouse; Esc releases the capture once,
# then quits. Mouse deltas/buttons are forwarded to the core raw-
# input path — the configured W-set mapping (axes "ABG", scales
# 16/16/50, button masks 1/4/2/0) lives entirely in mdk_core.

# Variant on purpose: keeping the MdkBridge reference untyped lets
# the script still parse when the GDExtension is missing, so the
# failure surfaces as an actionable error instead of a parse abort.
var bridge = null
var last_order_digest := -1
var smoke := false
var interactive := false
var failures := 0
var shot_path := ""
var shot_frames_left := 0
var frames_left := 0

# Raw mouse accumulators — device deltas for the next frame only.
var mouse_dx := 0
var mouse_dy := 0
var mouse_dz := 0

# Presentation resources (built in _build_player_proxy).
var body_mat: StandardMaterial3D
var marker_mat: StandardMaterial3D
var wire_mat: StandardMaterial3D
var col_mat: StandardMaterial3D

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

	_build_player_proxy()

	# One idle frame settles the deterministic spawn camera.
	bridge.step_frame_input(0.0, {})
	_apply_arena_snapshot()
	_apply_collision_snapshot()
	_apply_player_snapshot()
	_apply_camera_snapshot()
	_update_debug_label()

	if smoke:
		_run_smoke(data_root)
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
		"Esc release/quit)") % [arena, bridge.get_arena_names().size()])


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
	# CollisionDebug — the arena's collision poly edges (F1 toggles).
	col_mat = StandardMaterial3D.new()
	col_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	col_mat.albedo_color = Color(0.9, 0.2, 0.9)
	$CollisionDebug.material_override = col_mat
	$CollisionDebug.visible = false


func _apply_arena_snapshot() -> void:
	var snap: Dictionary = bridge.get_arena_render_snapshot()
	if snap.is_empty():
		return
	$ArenaMesh.mesh = snap["mesh"]
	$ArenaMesh.set_surface_override_material(0, snap["material"])
	last_order_digest = snap["stats"]["order_digest"]


func _apply_collision_snapshot() -> void:
	var col: Dictionary = bridge.get_collision_snapshot()
	if col.is_empty():
		return
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = col["lines"]
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	$CollisionDebug.mesh = am


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
	$DebugUI/DebugLabel.text = (
		"pos_mdk %.2f %.2f %.2f   yaw %.1f  pitch %.1f\n" %
		[mp.x, mp.y, mp.z, p["yaw_deg"], p["pitch_deg"]] +
		"grounded %s  loco %d  arena %d->%d\n" %
		[p["grounded"], p["loco_state"], p["arena"],
		p["arena_display"]] +
		"vel move %.2f  strafe %.2f  vert %.2f  turn %.2f" %
		[p["move_vel"], p["strafe_vel"], p["vert_vel"],
		p["turn_vel"]])


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
		elif event.keycode == KEY_F3:
			$DebugUI.visible = not $DebugUI.visible


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
	_update_debug_label()
	# Rebuild the ordered mesh only when the BSP submission order
	# actually changed (camera-dependent painter's order), or the
	# core portal-swapped arenas (the digest changes either way).
	var d = bridge.get_arena_order_digest()
	if d != last_order_digest:
		_apply_arena_snapshot()
		_apply_collision_snapshot()


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
	for i in 6:
		_step_n({"actions": ACT_STRAFE_RIGHT}, 1)
		var cur: Dictionary = bridge.get_player_snapshot()
		if cur["grounded"]:
			var d: Vector3 = cur["pos"] - base
			lat += d.dot(sb.x)
			lon += abs(d.dot(-sb.z))
			gnd_frames += 1
		base = cur["pos"]
	var p3: Dictionary = bridge.get_player_snapshot()
	_check(float(p3["strafe_vel"]) > 0.1,
		"E drives strafeVel > 0 (right)")
	_check(gnd_frames >= 3 and lat > 0.15 and lon < lat,
		"E displacement lateral-right dominant")
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
	for i in 8:
		_step_n({"actions": ACT_JUMP}, 1)
		var pj: Dictionary = bridge.get_player_snapshot()
		if float(pj["vert_vel"]) > 1.0 or \
				Vector3(pj["pos"]).y > base.y + 0.3:
			rose = true
	_check(rose, "Space initiates jump (vertical rise)")
	_wait_rest()  # release + land/settle

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

	print("smoke: %d failure(s)" % failures)
