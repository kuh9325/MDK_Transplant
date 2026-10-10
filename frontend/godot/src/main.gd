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
#                     the persisted user://config.cfg choice, else
#                     ~/Library/Application Support/MDK/data, else
#                     <repo>/original/installed relative to res://;
#                     interactive launches with none of these show
#                     a folder picker and persist the answer)
#   --level RELDTI    default TRAVERSE/LEVEL3/LEVEL3.DTI
#   --arena NAME      default HMO_1 ("" -> spawn arena)
#   --start X Y Z     diagnostic re-anchor into --arena (native
#                     diagnostic — test/QA path, not original flow)
#   --start-yaw DEG   yaw for --start (default 0)
#   --freefall N      mode-2 freefall course 0..4 instead of --level
#                     (Phase 16C; hands off to traversal on landing,
#                     or the frontend route on death)
#   --stream N        mode-5 intermission course 0..4 instead of
#                     --level (Phase 19B.1; indexed framebuffer +
#                     palette presentation, model/ribbon deferred)
#   --campaign N      real campaign handoff route for course 0..4
#                     (Phase 19B.3A): traversal -> diagnostic END_LEVEL
#                     -> mode 5 -> natural exit -> mode 6 loader ->
#                     mode 2/0. Smoke-only; boots the frontend shell
#                     but drives the steps itself.
#   --skill N         difficulty 0..2 for --freefall/--stream
#                     (default 0)
#   --seed N          RNG seed for --freefall/--stream (default
#                     0xC0FFEE — the mdk-inspect digest seed)
#   --frontend        boot the authoritative frontend menu (mode 0)
#                     instead of a level — Godot presents + routes,
#                     FrontendShell owns all menu semantics
#   --save-dir DIR    writable save root for --frontend (default
#                     user://saves — NEVER the original data dir)
#   --smoke           headless deterministic check, then quit
#   --save-restore    with --smoke: run the save->restore combat
#                     golden (LEVEL3/HMO_9) instead of the smoke
#   --combat-demo     scripted scoped-fire input for real-renderer
#                     runs (pairs with --frames / interactive)
#   --screenshot P    after 8 frames, save a PNG capture then quit
#                     (requires a real renderer — not --headless)
#   --frames N        run N process frames then quit (startup proof)
#
# Keys: the factory binding block (keyboard_menu.h defaults) —
# arrows move+turn, LAlt jump, LCtrl fire, Space sniper, X sidestep,
# LShift turbo, Caps set-turbo, A/Z look+zoom, number row = weapons.
# The in-game Keyboard screen rebinds all of it. F1 collision wire,
# F2 dynamic-object debug (AABB wires + name/id/arena tags), F3
# debug text. Interactive runs capture the mouse; Esc releases the
# capture once, then quits. Held physical keys are translated to the
# original's internal code domain (the FUN_0046b688 bitmap) and sent
# as "keys" each frame; mouse deltas/buttons go through the raw
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
var stream_shot_dir := ""    # --stream-shots DIR (19B.1A evidence)
var shot_frames_left := 0
var frames_left := 0
var obj_debug := false
var proxy_debug := false      # F4 — legacy capsule/wire proxy
var last_kurt := {}           # last applied kurt snapshot (diag)
var combat_demo := false      # --combat-demo: scripted scoped-fire
                              # input under the real renderer
var chute_demo := false       # --chute-demo: hold jump from frame 2
                              # (the sustain->K_CHUTE evidence path)
var step_shot := false        # --step: keep stepping under --screenshot
                              # (for deep-frame freefall/traversal probes)
var ff_steer := false         # --ff-steer: scripted weave via real
                              # key events (production poll path)
var ff_trace := false         # --ff-trace: hp/missile-distance log
var ff_shot_pass := false     # --ff-shot-pass: --screenshot waits for a close missile approach
var ff_dump_dir := ""         # --ff-dump-dir: consecutive-frame dump (game output only)
var ff_dump_every := 1        # --ff-dump-every N: capture every Nth rendered frame
var _ff_dump_count := 0
var _shot_pass_lo := -160.0   # --shot-pass-window LO HI: dz trigger window
var _shot_pass_hi := -25.0
var _ff_held := {}            # synthetic key state for --ff-steer
var _ff_jink := 0             # committed jink keycode (0 = none)
var demo_frame := 0

# Phase 16C — mode-2 freefall presentation state. All gameplay lives
# in the core; these are view-side caches only.
var freefall := false        # --freefall launcher flag
var ff_materials := {}       # "m:<name>" / "pen:<n>" -> StandardMaterial3D
var ff_palette := PackedByteArray()  # FALLP_<c+1> bytes (768)
var ff_handoff_seen := false # printed the mode transition once
var _ff_steps := 0            # mode-2 sim steps this session
var _ff_wall0 := 0            # wall-clock anchor for the descent
var fe_run := false          # --fe-run: confirm once -> production New Game route
var fe_brief_natural := false  # --fe-brief-natural: unforced briefing pace
var _mode6_hold_n := 0         # post-page hold counter for the above
var _fe_run_done := false
var _ff_backdrop_warned := 0 # one-shot NOT-READY resource report
var ff_backdrop: MeshInstance3D = null  # camera-locked backdrop quad
var ff_backdrop_img: Image = null
var ff_backdrop_tex: ImageTexture = null
var ff_key_colors := PackedByteArray()  # 64x3 LUT keyframe ramp
var ff_flare_px := PackedByteArray()    # FLARE4 indexed pixels
var ff_flare_wh := Vector2i.ZERO
var ff_pick_tex: ImageTexture = null    # kind-1 PICK sprite
var ff_pick_wh := Vector2i.ZERO         # PICK src dims (for 0x46d680 scale)

# Phase 19B.1 — mode-5 intermission presentation. The StreamScene
# core owns simulation; the bridge owns the indexed framebuffer.
# st_* are view-side texture caches for the presented RGBA frame.
var stream := false            # --stream N launcher flag
var stream_course := 0         #   parsed --stream value
var st_img: Image = null
var st_tex: ImageTexture = null

# Raw mouse accumulators — device deltas for the next frame only.
var mouse_dx := 0
var mouse_dy := 0
var mouse_dz := 0

# --- Phase 18B.2A — frontend presentation -------------------------
# Godot presents + routes only: every menu transition lives in
# mdk::FrontendShell (C++). These fields are presentation state —
# edge queues, the composed-frame texture, the transition timer.
var frontend := false          # --frontend launcher flag
var data_root_path := ""       # resolved --data-path (smoke checks)
var fe_active := false         # frontend route owns the frame
var fe_sub := 0                # last snapshot's sub_mode (input routing)
var fe_edges := {}             # semantic edge fields for this frame
var fe_typed := PackedInt32Array()   # queued typed chars (1/frame)
var fe_raw := PackedInt32Array()     # internal-domain key edges
var fe_img: Image = null
var fe_tex: ImageTexture = null
# Entry-transition playback window: fe_transition_ms < 0 = idle;
# >= 0 counts the elapsed FUN_0041e554 timeline while fe_transition_
# total_ms holds the record's nominal duration (0 when INTRO1A is
# absent — the window then closes on the first tick).
var fe_transition_ms := -1.0
var smoke_save_dir := ""  # globalized --save-dir for smoke checks
var fe_transition_total_ms := 0.0
var fe_transition_acks := 0         # frontend_transition_complete calls
var fe_stage_hold := 0         # intermission placeholder hold
var fe_fx_counts := {}         # FrontendFx id -> count (diag/smoke)
var fe_req_counts := {}        # request id -> count (diag/smoke)
var fe_quit := false           # Quit request drained (app-owned)
const FE_STAGE_HOLD := 45          # ~0.75s intermission placeholder

# Phase 19C.1 — Mode-5 host pacing. The scene's own limiter
# (FUN_0042fb68 -> 42fcd0) reads in.nowMs; its OBSERVED pace-wait arm
# (0x5414ac) is dead — the original free-ran one sim step per
# dispatcher tick (~30Hz on a period machine). This gate is the host
# half of that contract: one sim step per ~33.3ms of monotonic wall
# time, NOT one per display refresh (a 60Hz desktop otherwise runs
# the tunnel at 2x). in.nowMs keeps its step-quantized +33 feed —
# at this cadence it IS the monotonic source the limiter measures.
# Catch-up is bounded at 4 — the limiter's own t1>4 resync bound —
# with the excess dropped: a long stall skips presentation time
# rather than spiraling. Deterministic harnesses bypass the gate by
# driving step_frame_input directly (fixed 33.333ms), so goldens
# are untouched.
const STREAM_STEP_MS := 1000.0 / 30.0
const STREAM_MAX_CATCHUP := 4
var stream_pace_ms := 0.0
var stream_pace_armed := false
# Mode 2/3 gameplay pacing — the same host contract as the mode-5
# pacer above (19C.1): the original ran one sim step per dispatcher
# tick (~30Hz), FUN_0042fdc8's frameStep is per-CALL catch-up math,
# not a per-render-frame grant. A 60/120Hz display otherwise runs
# the world 2-4x fast while deltaSec collapses toward 0 (rawDelta
# truncates to 0 below ~9ms frames). One sim step per ~33.3ms of
# wall time; catch-up bounded at 4 — the limiter's own resync bound.
# Deterministic harnesses drive step_frame_input directly, so
# goldens are untouched.
var game_pace_ms := 0.0
var game_pace_armed := false
# --pace-trace: bounded wall-clock evidence for the mode-5 limiter —
# sim steps vs monotonic elapsed + presented frames, once per second.
var pace_trace := false
var pace_wall0 := -1
var pace_last_log := -1
var pace_steps := 0

func _stream_pace_run(delta_ms: float) -> int:
	# Steps the live StreamScene at the paced cadence; returns the
	# number of sim steps actually run this display frame.
	if not bridge.stream_active():
		stream_pace_armed = false
		return 0
	if not stream_pace_armed:
		# First live observation — present frame 0 immediately
		# instead of holding a 33ms dead window at scene install.
		stream_pace_armed = true
		stream_pace_ms = STREAM_STEP_MS
	stream_pace_ms += delta_ms
	var n := int(stream_pace_ms / STREAM_STEP_MS)
	if n > STREAM_MAX_CATCHUP:
		n = STREAM_MAX_CATCHUP
		stream_pace_ms = 0.0   # slow-frame policy: drop the backlog
	else:
		stream_pace_ms -= n * STREAM_STEP_MS
	var ran := 0
	while ran < n and bridge.stream_active():
		bridge.step_frame_input(STREAM_STEP_MS,
			{"actions": _input_mask(), "keys": _gameplay_keys()})
		ran += 1
	pace_steps += ran
	if pace_trace and bridge.stream_active():
		var wall := Time.get_ticks_msec()
		if pace_wall0 < 0:
			pace_wall0 = wall
			pace_last_log = wall
		elif wall - pace_last_log >= 1000:
			var el := wall - pace_wall0
			var fr: Dictionary = bridge.stream_frame()
			print("pace: wall=%dms steps=%d rate=%.2f presented=%s" %
				[el, pace_steps, pace_steps * 1000.0 / el,
				str(fr.get("seq", "?"))])
			pace_last_log = wall
	return ran

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

# Phase 17C.2 — traversal audio. mdk_core owns every audio semantic
# (the TraversalAudioMixer voice pool + the ported FUN_0040282c/
# 00402b00 updater math); this presenter only maps voice-slot handles
# to AudioStreamPlayer nodes and applies the computed dB/pan/pitch.
# Per-voice pan runs through a dedicated AudioEffectPanner bus —
# AudioStreamPlayer has no pan channel and AudioStreamPlayer3D would
# apply Godot's own distance model, which is NOT the original's.
const AUDIO_VOICES := 63        # the original's pool cap (FUN_00402604)
var audio_players := []         # [63] AudioStreamPlayer | null
var audio_panners := []         # [63] AudioEffectPanner (bus effect)
var audio_stats := {"cmds": 0, "starts": 0, "params": 0,
	"stops": 0, "names": {}, "starts_by_name": {},
	"stops_by_name": {}}      # --smoke/diag counters

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
var geom_cache := {}        # geom_key -> [{elem,mesh,mat,pen}]
var elem_mats := {}         # elem name -> StandardMaterial3D (fallback)
var obj_mats := {}          # "o:<oid>|<name>|<pen>" -> StandardMaterial3D
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


# Phase 19C.2 — data-root discovery + the interactive picker. The
# candidate order is: explicit override, persisted choice, the
# per-user install dir, the dev-checkout fallback. An explicit
# override is honored verbatim only while it still validates — a
# stale one must fall through (otherwise a picked-and-persisted
# choice could never win and the picker would loop forever).
func _data_root_valid(dir: String) -> bool:
	# The traversal level-3 payload is the marker every launch path
	# needs; the same validity test as tests/test_godot_frontend.py.
	return FileAccess.file_exists(
		dir.path_join("TRAVERSE/LEVEL3/LEVEL3.DTI"))


func _resolve_data_root(args: PackedStringArray) -> String:
	var p := _arg_value(args, "--data-path",
		OS.get_environment("MDK_DATA_ROOT"))
	if not p.is_empty():
		if p.is_relative_path():
			# Godot chdir's into the project dir; resolve user paths
			# against the launch directory ($PWD survives the chdir).
			var launch_dir := OS.get_environment("PWD")
			if not launch_dir.is_empty():
				p = launch_dir.path_join(p).simplify_path()
		if _data_root_valid(p):
			return p
		printerr("warning: --data-path/MDK_DATA_ROOT '", p,
			"' fails the LEVEL3 marker check — falling through ",
			"to discovery")
	var cfg := ConfigFile.new()
	if cfg.load("user://config.cfg") == OK:
		var saved: String = cfg.get_value("paths", "data_root", "")
		if not saved.is_empty() and _data_root_valid(saved):
			return saved
	# The per-user install dir — user:// maps to
	# ~/Library/Application Support/MDK via the custom_user_dir
	# settings in project.godot; data sits beside saves/ and
	# config.cfg rather than inside either.
	var support := OS.get_user_data_dir().path_join("data")
	if _data_root_valid(support):
		return support
	# Dev checkout fallback — dead weight in an exported app (res://
	# lives inside the bundle), so it just never validates there.
	var repo := ProjectSettings.globalize_path(
		"res://../../original/installed")
	if _data_root_valid(repo):
		return repo
	return ""


var _picker_label: Label = null
var _picker_dialog: FileDialog = null

func _show_data_picker() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	var layer := CanvasLayer.new()
	layer.layer = 100
	layer.name = "DataPickerLayer"
	add_child(layer)
	var dim := ColorRect.new()
	dim.color = Color(0.03, 0.04, 0.07, 1.0)
	dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	layer.add_child(dim)
	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.position -= Vector2(300, 90)
	panel.custom_minimum_size = Vector2(600, 0)
	layer.add_child(panel)
	var vb := VBoxContainer.new()
	vb.add_theme_constant_override("separation", 14)
	panel.add_child(vb)
	var title := Label.new()
	title.text = "MDK — original game data required"
	title.add_theme_font_size_override("font_size", 20)
	vb.add_child(title)
	_picker_label = Label.new()
	_picker_label.autowrap_mode = TextServer.AUTOWRAP_WORD
	_picker_label.text = "Point MDK at the folder holding the " + \
		"installed original data (it must contain " + \
		"TRAVERSE/LEVEL3/LEVEL3.DTI).\n\nChecked already: " + \
		OS.get_user_data_dir().path_join("data")
	vb.add_child(_picker_label)
	var hb := HBoxContainer.new()
	hb.add_theme_constant_override("separation", 12)
	vb.add_child(hb)
	var choose := Button.new()
	choose.text = "Choose folder…"
	choose.pressed.connect(_on_data_picker_choose)
	hb.add_child(choose)
	var quitb := Button.new()
	quitb.text = "Quit"
	quitb.pressed.connect(func(): get_tree().quit(1))
	hb.add_child(quitb)
	_picker_dialog = FileDialog.new()
	_picker_dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_picker_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_picker_dialog.use_native_dialog = true
	_picker_dialog.title = "Select the MDK data folder"
	_picker_dialog.dir_selected.connect(_on_data_dir_picked)
	layer.add_child(_picker_dialog)


func _on_data_picker_choose() -> void:
	if _picker_dialog == null:
		return
	_picker_dialog.popup_centered_ratio(0.6)


func _on_data_dir_picked(dir: String) -> void:
	if _data_root_valid(dir):
		var cfg := ConfigFile.new()
		cfg.load("user://config.cfg")
		cfg.set_value("paths", "data_root", dir)
		cfg.save("user://config.cfg")
		# Reboot through the normal path: the persisted choice now
		# resolves, and _ready runs the whole boot unchanged.
		get_tree().reload_current_scene()
		return
	if _picker_label != null:
		_picker_label.text = "\"" + dir + "\" does not look like " + \
			"an MDK install — expected " + \
			"TRAVERSE/LEVEL3/LEVEL3.DTI inside it.\n\n" + \
			"Pick the folder that holds the game's TRAVERSE/, " + \
			"FALL3D/ and MISC/ data directories."


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
	chute_demo = "--chute-demo" in args
	step_shot = "--step" in args
	ff_steer = "--ff-steer" in args
	ff_trace = "--ff-trace" in args
	ff_shot_pass = "--ff-shot-pass" in args
	fe_run = "--fe-run" in args
	# --fe-brief-natural: with --fe-run, mode 6 runs the natural
	# input path — no forced keys: 15c/s typing, the post-page
	# bf04 hold (30 visible hold frames), then one pulsed key.
	fe_brief_natural = "--fe-brief-natural" in args
	ff_dump_dir = _arg_value(args, "--ff-dump-dir", "")
	ff_dump_every = maxi(1, int(_arg_value(args, "--ff-dump-every", "1")))
	if not ff_dump_dir.is_empty():
		# The arg is a host path — resolve against the OS cwd and
		# create it so a missing dir can't silently drop captures.
		if not ff_dump_dir.is_absolute_path():
			ff_dump_dir = OS.get_environment("PWD") + "/" + ff_dump_dir
		DirAccess.make_dir_recursive_absolute(ff_dump_dir)
	var spw := _arg_value(args, "--shot-pass-window", "")
	if not spw.is_empty():
		var parts := spw.split(",")
		if parts.size() == 2:
			_shot_pass_lo = float(parts[0])
			_shot_pass_hi = float(parts[1])
	data_root_path = _resolve_data_root(args)
	var level := _arg_value(args, "--level", "TRAVERSE/LEVEL3/LEVEL3.DTI")
	var arena := _arg_value(args, "--arena", "HMO_1")
	var start := _arg_value(args, "--start", "")
	var start_yaw := float(_arg_value(args, "--start-yaw", "0"))
	var ff_course := _arg_value(args, "--freefall", "")
	ff_skill = int(_arg_value(args, "--skill", "0"))
	var st_course := _arg_value(args, "--stream", "")
	var cp_course := _arg_value(args, "--campaign", "")
	var ending := "--ending" in args
	end_abort_at = int(_arg_value(args, "--ending-abort", "-1"))
	end_repeat = "--ending-repeat" in args
	pace_trace = "--pace-trace" in args
	if end_abort_at >= 0 or end_repeat:
		print("ending QA: abort_at=", end_abort_at, " repeat=", end_repeat)
	frontend = "--frontend" in args
	# 0xC0FFEE — the same default mdk-inspect's --freefall-runtime
	# digest runs use, so driven courses are cross-checkable.
	ff_seed = int(_arg_value(args, "--seed", "12648430"))
	freefall = not ff_course.is_empty()
	if freefall:
		ff_course_id = int(ff_course)
	stream = not st_course.is_empty()
	var campaign := not cp_course.is_empty()
	var campaign_course := 0
	if stream:
		stream_course = int(st_course)
	if campaign:
		campaign_course = int(cp_course)
	shot_path = _arg_value(args, "--screenshot", "")
	stream_shot_dir = _arg_value(args, "--stream-shots", "")
	if shot_path.is_relative_path() and not shot_path.is_empty():
		var launch_dir := OS.get_environment("PWD")
		if not launch_dir.is_empty():
			shot_path = launch_dir.path_join(shot_path).simplify_path()
	if stream_shot_dir.is_relative_path() and not stream_shot_dir.is_empty():
		var shot_cwd := OS.get_environment("PWD")
		if not shot_cwd.is_empty():
			stream_shot_dir = shot_cwd.path_join(stream_shot_dir).simplify_path()
	frames_left = int(_arg_value(args, "--frames", "0"))
	interactive = not smoke and shot_path.is_empty() and frames_left <= 0

	# 19C.2 — the packaged build is the game: with no mode flag at
	# all it lands on the frontend menu, not the LEVEL3 dev shortcut.
	# The editor/dev binary is untouched — its flagless default still
	# loads the level directly for iteration.
	if interactive and not OS.has_feature("editor") and \
			not freefall and not stream and not campaign and \
			not ending and \
			_arg_value(args, "--level", "").is_empty():
		frontend = true

	# 19C.2 — no usable data root: an interactive launch shows the
	# folder picker and defers boot (the chosen dir is persisted to
	# user://config.cfg and the scene reloads); non-interactive
	# launches keep the hard error — a CI run must not block on UI.
	if data_root_path.is_empty():
		if interactive:
			_show_data_picker()
			return
		printerr("no MDK data root — pass --data-path DIR, set ",
			"MDK_DATA_ROOT, or install data under ",
			OS.get_user_data_dir().path_join("data"))
		get_tree().quit(1)
		return

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
	if not bridge.initialize(data_root_path):
		printerr("MdkBridge.initialize failed: ", bridge.get_last_error())
		get_tree().quit(1)
		return
	if frontend:
		# The writable save root is NEVER the original data dir:
		# user://saves by default, --save-dir for tests. Smoke runs
		# always use the dedicated user://saves_smoke root (wiped —
		# it only ever holds files this harness wrote). The real-save
		# corpus check is the one exception: it points the store at
		# the installed SAVES dir and runs a read-only scenario (no
		# write call exists on that path).
		var real_saves := smoke and "--smoke-real-saves" in args
		var save_dir := _arg_value(args, "--save-dir",
			"user://saves_smoke" if smoke else "user://saves")
		if real_saves:
			save_dir = data_root_path.path_join("SAVES")
		if save_dir.is_relative_path() and not \
				save_dir.begins_with("user://") and not \
				save_dir.begins_with("res://"):
			var launch_dir := OS.get_environment("PWD")
			if not launch_dir.is_empty():
				save_dir = launch_dir.path_join(save_dir) \
					.simplify_path()
		save_dir = ProjectSettings.globalize_path(save_dir)
		smoke_save_dir = save_dir
		# --smoke-continue is the relaunch half of the two-process
		# save->quit->load QA: boot on an existing save root, no wipe.
		if smoke and not real_saves and \
				not "--smoke-continue" in args:
			DirAccess.make_dir_recursive_absolute(save_dir)
			var da := DirAccess.open(save_dir)
			if da != null:
				for f in da.get_files():
					if f.get_extension() == "SAV":
						da.remove(f)
		if not bridge.frontend_boot(save_dir):
			printerr("MdkBridge.frontend_boot failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return
		# Boot = the FUN_0041d85c(0) fresh entry.
		bridge.frontend_enter(false)
		fe_active = true
	elif freefall:
		if not bridge.load_freefall(int(ff_course), ff_skill, ff_seed):
			printerr("MdkBridge.load_freefall failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return
	elif campaign:
		# Phase 19B.3A — the real campaign handoff route: traversal
		# -> mode 5 -> mode 6 loader -> mode 2/3/0. The frontend
		# shell must exist (the mode-6 pump and the mode-0 exit need
		# feShell_) but fe_active stays false — the smoke drives
		# step_frame_input + frontend_progression_step directly,
		# the same calls _process/_frontend_frame make.
		var cdir := "user://saves_smoke" if smoke else "user://saves"
		cdir = ProjectSettings.globalize_path(cdir)
		if smoke:
			DirAccess.make_dir_recursive_absolute(cdir)
		if not bridge.frontend_boot(cdir):
			printerr("MdkBridge.frontend_boot failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return
	elif stream:
		if not bridge.load_stream(stream_course, ff_skill, ff_seed):
			printerr("MdkBridge.load_stream failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return
	elif ending:
		# Phase 19D — the QA boot straight into the mode-8 ending.
		# The frontend shell must exist (the FUN_0041d85c return at
		# the boundary needs feShell_) but fe_active stays off — the
		# standalone mode-8 presenter owns the frame.
		var edirs := "user://saves_ending" if smoke else "user://saves"
		edirs = ProjectSettings.globalize_path(edirs)
		if smoke:
			DirAccess.make_dir_recursive_absolute(edirs)
		if not bridge.frontend_boot(edirs):
			printerr("MdkBridge.frontend_boot failed: ",
				bridge.get_last_error())
			get_tree().quit(1)
			return
		if not bridge.load_ending():
			printerr("MdkBridge.load_ending failed: ",
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
		# The traversal death route (LASTGAME.SAV write + mode-0
		# frontend entry) needs the host services even on a bare
		# --level run. fe_active stays false — the shell is dormant
		# until the route enters it. Save root: --save-dir, else the
		# same user:// convention the campaign branch uses; boot
		# failure is a warning, not a launch blocker (dev path).
		var tdir := _arg_value(args, "--save-dir",
			"user://saves_smoke" if smoke else "user://saves")
		tdir = ProjectSettings.globalize_path(tdir)
		if smoke:
			DirAccess.make_dir_recursive_absolute(tdir)
			var tda := DirAccess.open(tdir)
			if tda != null:
				for tf in tda.get_files():
					if tf.get_extension() == "SAV":
						tda.remove(tf)
			smoke_save_dir = tdir
		if not bridge.frontend_boot(tdir):
			printerr("frontend_boot (death route) failed: ",
				bridge.get_last_error())
	if not frontend and not freefall and not stream and \
			not start.is_empty():
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
	_build_audio_presenter()

	if frontend:
		# The frontend owns the screen; all gameplay layers stay
		# hidden until a request lands a runtime mode.
		_frontend_show()
	elif not freefall and not stream and not campaign:
		# One idle frame settles the deterministic spawn camera.
		bridge.step_frame_input(0.0, {})
	if freefall:
		_apply_freefall()
	elif stream:
		_apply_stream()
	elif not frontend and not campaign:
		_apply_arena_snapshots()
		_apply_object_snapshots()
		_apply_player_snapshot()
		_apply_camera_snapshot()
		_apply_kurt_snapshot()
		_update_debug_label()

	if smoke:
		if frontend:
			if "--smoke-real-saves" in args:
				_run_smoke_real_saves()
			elif "--smoke-continue" in args:
				_run_smoke_continue()
			else:
				_run_smoke_frontend()
		elif "--save-restore" in args:
			# Phase 17A closeout — the save->restore golden is bound
			# to the canonical LEVEL3 combat arena (HMO_9).
			if freefall or level != "TRAVERSE/LEVEL3/LEVEL3.DTI":
				printerr("--save-restore smoke requires the " +
					"default LEVEL3 launch")
				get_tree().quit(2)
				return
			_run_smoke_restore()
		elif "--smoke-input" in args:
			# Live-input regression — real InputEvents -> the same
			# dict _process builds -> the configured-binding fold.
			# Runs on the default LEVEL3/HMO_1 load.
			_run_smoke_input()
		elif freefall:
			_run_smoke_freefall(int(ff_course), ff_skill, ff_seed)
		elif campaign:
			_run_smoke_campaign(campaign_course)
		elif stream:
			_run_smoke_stream(stream_course)
		elif level == "TRAVERSE/LEVEL3/LEVEL3.DTI" and \
				arena == "HMO_1" and start.is_empty():
			_run_smoke(data_root_path)
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
		# --shot-frames N defers the capture (e.g. past the mode-8
		# FLIC into the MVE stage); default 8 settles the pipeline.
		# --ff-shot-pass instead arms the trigger: the countdown is
		# set to 1 by _apply_freefall when an inbound missile crosses
		# the close window, so the capture lands on the visible pass.
		shot_frames_left = -1 if ff_shot_pass else \
			int(_arg_value(args, "--shot-frames", "8"))
	if interactive and not frontend:
		# The frontend needs the OS cursor for its hit-test mouse.
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

	if frontend:
		print("mdk-godot: FRONTEND  (arrows/nav, Enter select, " +
			"Esc cancel/back, F1 help, F2 save, F3 saves, " +
			"F10 abort, F12 options)")
	elif freefall:
		_ff_wall0 = Time.get_ticks_msec()
		print(("mdk-godot: FREEFALL course=%d skill=%d seed=%08x  " +
			"(arrows/WASD steer, F3 debug, Esc release/quit)") %
			[int(ff_course), ff_skill, ff_seed])
	elif stream:
		print(("mdk-godot: STREAM course=%d skill=%d seed=%08x  " +
			"(mode-5 intermission; Esc quits)") %
			[stream_course, ff_skill, ff_seed])
	else:
		# Factory bindings (the original defaults — the in-game
		# Keyboard screen rebinds them): arrows move+turn, LAlt jump,
		# LCtrl fire, Space sniper scope, A/Z look+zoom, X sidestep,
		# LShift turbo; LMB fire / RMB jump / MMB sniper.
		print(("mdk-godot: arena=%s arenas=%d  (arrows move, " +
			"LAlt jump, LCtrl fire, Space sniper, LMB/RMB fire/" +
			"jump, MMB sniper; Esc release/quit)") %
			[arena, bridge.get_arena_names().size()])


func _exit_tree() -> void:
	# Live players hold AudioStreamPlayback objects server-side —
	# stop them before the tree teardown. (Under the headless Dummy
	# audio driver the server never mixes, so its playback list
	# isn't reaped until shutdown — a bounded exit-time artifact;
	# the real driver reaps them on stop.)
	_reset_audio()
	# Drop the per-voice panner buses added in the build pass.
	for i in AUDIO_VOICES:
		var idx := AudioServer.get_bus_index("mdkfx%d" % i)
		if idx >= 0:
			AudioServer.remove_bus(idx)


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
		# Translucent LUT-remap arm (fx12970 force-field/shimmer polys)
		# renders in its own alpha-blend pass so the opaque soup keeps
		# depth-writing. Created only when the arena submits any.
		var fxnode: MeshInstance3D = $ArenaRoot.get_node_or_null(
			"Arena_%d_fx" % idx)
		if bool(s.get("has_fx", false)):
			if fxnode == null:
				fxnode = MeshInstance3D.new()
				fxnode.name = "Arena_%d_fx" % idx
				$ArenaRoot.add_child(fxnode)
			fxnode.mesh = s["mesh_fx"]
			fxnode.set_surface_override_material(0, s["material_fx"])
			live["%d_fx" % idx] = true
		elif fxnode != null:
			fxnode.queue_free()
		lines.append_array(s["collision_lines"])
	for child in $ArenaRoot.get_children():
		# Base nodes key as int idx; translucent nodes key as "<idx>_fx".
		var cn := String(child.name)
		var key: Variant = cn.trim_prefix("Arena_") if cn.ends_with("_fx") \
			else int(cn.trim_prefix("Arena_"))
		if not live.has(key):
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
		surface_elems: PackedInt32Array,
		surface_mats: PackedStringArray,
		surface_pen: PackedInt32Array) -> Array:
	# One single-surface mesh per (element, material-index) group —
	# the element-disable mask toggles visibility per element (the
	# "elem" meta), while each surface binds its own resolved
	# material (the model tri record's s16 @+6 -> matlkup name).
	var out := []
	for s in src.get_surface_count():
		var arrays: Array = src.surface_get_arrays(s)
		var m := ArrayMesh.new()
		m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		out.append({"elem": int(surface_elems[s]), "mesh": m,
			"mat": String(surface_mats[s]) if s < surface_mats.size() else "",
			"pen": int(surface_pen[s]) if s < surface_pen.size() else -1})
	return out


func _elem_material(elem_name: String) -> StandardMaterial3D:
	# Deterministic per-element debug material — retained ONLY as
	# the last-resort fallback when the bridge cannot resolve a
	# surface at all (stale id, missing banks). Real surfaces go
	# through _object_material (matlkup-resolved textures/pens).
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


func _object_material(oid: int, mat_name: String, pen: int,
		fallback: String) -> StandardMaterial3D:
	# Surface material for one (element, material-index) group —
	# the traversal sibling of _ff_material. oid <= 0 resolves the
	# level-model context (shots/named geometry) in the current
	# display set. Materials cache per (oid, name, pen); the bridge
	# dedupes the underlying ImageTextures per arena set.
	var ck := "o:%d|%s|%d" % [oid, mat_name, pen]
	if obj_mats.has(ck):
		return obj_mats[ck]
	var d: Dictionary = bridge.get_object_material(oid, mat_name, pen)
	if d.is_empty() or not bool(d.get("valid", false)):
		var fb := _elem_material(fallback)
		obj_mats[ck] = fb
		return fb
	if int(d.get("palette_index", -1)) >= 0:
		# Flat pen / index record — the color is the palette entry.
		# palette_index >= 256 = the no-draw slot (NONE).
		if bool(d.get("no_draw", false)):
			var nd := _ff_no_draw_material()
			obj_mats[ck] = nd
			return nd
		var pm := StandardMaterial3D.new()
		pm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		pm.cull_mode = BaseMaterial3D.CULL_DISABLED
		pm.albedo_color = d.get("palette_color", Color(1, 1, 1))
		obj_mats[ck] = pm
		return pm
	# Texture record — pixel-space UVs are normalized by uv1_scale.
	# Nearest filter: the software rasterizer texel-fetches.
	var tm := StandardMaterial3D.new()
	tm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	tm.cull_mode = BaseMaterial3D.CULL_DISABLED
	tm.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	# Index 0 is the fill's transparent texel key — the EXPLODE remnant
	# and the FIRE/splat bursts are transparent-surround sprites. Opaque
	# textures stay in the depth-writing opaque pass; only alpha-bearing
	# textures enter the transparent pass (else index 0 renders black).
	if bool(d.get("has_alpha", false)):
		tm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	tm.albedo_texture = d["tex"]
	var tw := float(d.get("w", 0))
	var th := float(d.get("h", 0))
	if tw > 0.0 and th > 0.0:
		tm.uv1_scale = Vector3(1.0 / tw, 1.0 / th, 1.0)
	obj_mats[ck] = tm
	return tm


func _object_geom_meshes(g: Dictionary) -> Array:
	var key := int(g["geom_key"])
	if geom_cache.has(key):
		return geom_cache[key]
	var split := _split_elem_meshes(g["mesh"], g["surface_elems"],
		g["surface_mats"], g["surface_pen"])
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
					mi.material_override = _object_material(
						oid, String(sm["mat"]), int(sm["pen"]),
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
	# Mode-boundary frames can carry a not-yet-stepped pose (zeroed
	# scale -> singular basis, out-of-range fov). Applying it spams
	# invert/set_fov errors — the next stepped frame is correct.
	var fov := float(cam["fov_deg"])
	if absf(cam["transform"].basis.determinant()) < 1e-9 or \
			fov <= 1.0 or fov >= 179.0:
		return
	$Camera3D.global_transform = cam["transform"]
	$Camera3D.fov = fov


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
		# A camera inside a SubViewport is NOT auto-picked — without
		# current the viewport never renders and the bound
		# ViewportTexture shows the unrendered checkerboard. Must be
		# set once the subtree is in the scene tree.
		cam.current = true
		shot_vps.append(vp)
		shot_cams.append(cam)
		# viewport.get_texture() binds the render-target RID directly —
		# a hand-made ViewportTexture with viewport_path never resolves
		# outside the editor and shows the checkerboard placeholder.
		var vt := vp.get_texture()
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
		mi.material_override = _object_material(
			0, String(sm["mat"]), int(sm["pen"]),
			String(sm["name"]))
		n.add_child(mi)
	$FxRoot.add_child(n)
	remnants.append(n)
	fx_stats["remnants"] = int(fx_stats["remnants"]) + 1


func _load_geom_entry(g: Dictionary) -> Dictionary:
	# Normalize a get_*_geometry dict into {key, meshes:[{mesh,name,
	# mat,pen}]} for per-surface material binding (same convention
	# as objects).
	if g.is_empty():
		return {"key": -1, "meshes": []}
	var meshes := []
	var names: PackedStringArray = g["elem_names"]
	for sm in _object_geom_meshes(g):
		var nm := ""
		var ei := int(sm["elem"])
		if ei >= 0 and ei < names.size():
			nm = names[ei]
		meshes.append({"mesh": sm["mesh"], "name": nm,
			"mat": String(sm["mat"]), "pen": int(sm["pen"])})
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
					mi.material_override = _object_material(
						0, String(sm["mat"]), int(sm["pen"]),
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


# --- Phase 17C.2 — traversal audio presenter -----------------------
# One AudioEffectPanner bus per voice slot — the pool is bounded to
# the original's 63 instances, so the bus count is bounded too. All
# mixing math (attenuation, cone, doppler, volume-domain conversion)
# is computed in mdk_core; the dicts carry ready dB/pan/pitch values.
func _build_audio_presenter() -> void:
	audio_players.resize(AUDIO_VOICES)
	audio_players.fill(null)
	for i in AUDIO_VOICES:
		var bus := AudioServer.bus_count
		AudioServer.add_bus(bus)
		AudioServer.set_bus_name(bus, "mdkfx%d" % i)
		var panner := AudioEffectPanner.new()
		AudioServer.add_bus_effect(bus, panner)
		AudioServer.set_bus_send(bus, "Master")
		audio_panners.append(panner)


func _drain_audio_fx() -> void:
	# The bridge applies the core event batch to its voice pool and
	# ticks the updater once — this consumes the original's
	# drain-once contract exactly like _drain_combat_fx. Mode 5's
	# event feed and sweep already ran inside the stream step; the
	# drain itself is mode-agnostic (the pool is process-global) so
	# the mode-5 teardown tail still delivers its stops after the
	# mode flips.
	var cmds: Array = bridge.drain_audio_fx()
	if cmds.is_empty():
		if int(bridge.get_mode()) != 3 and \
				int(bridge.get_mode()) != 5 and \
				int(bridge.get_mode()) != 2:
			# Frontend/progression/post-handoff — no non-owning mode
			# can hold a live voice; if one survived a mode flip,
			# drop its node (the census survives — _reset_audio's
			# stat wipe is only for real reset boundaries).
			for p in audio_players:
				if p != null:
					_free_audio_players()
					break
		return
	for c in cmds:
		audio_stats["cmds"] = int(audio_stats["cmds"]) + 1
		var names: Dictionary = audio_stats["names"]
		var nm := String(c["name"])
		names[nm] = int(names.get(nm, 0)) + 1
		var id := int(c["id"])
		match String(c["op"]):
			"start":
				var p: AudioStreamPlayer = audio_players[id]
				if p == null:
					p = AudioStreamPlayer.new()
					p.name = "voice%d" % id
					p.bus = "mdkfx%d" % id
					$AudioRoot.add_child(p)
					audio_players[id] = p
				if c.has("stream"):
					p.stream = c["stream"]
				p.volume_db = float(c.get("db", 0.0))
				p.pitch_scale = float(c.get("pitch", 1.0))
				audio_panners[id].pan = float(c.get("pan", 0.0))
				if p.stream != null:
					p.play()
					audio_stats["starts"] = \
						int(audio_stats["starts"]) + 1
					var sbn: Dictionary = \
						audio_stats["starts_by_name"]
					sbn[nm] = int(sbn.get(nm, 0)) + 1
			"params":
				var p: AudioStreamPlayer = audio_players[id]
				if p != null:
					p.volume_db = float(c.get("db", p.volume_db))
					p.pitch_scale = float(
						c.get("pitch", p.pitch_scale))
					audio_panners[id].pan = float(c.get("pan", 0.0))
					audio_stats["params"] = \
						int(audio_stats["params"]) + 1
			"stop":
				var p: AudioStreamPlayer = audio_players[id]
				if p != null:
					p.stop()
					audio_stats["stops"] = \
						int(audio_stats["stops"]) + 1
					var tbn: Dictionary = \
						audio_stats["stops_by_name"]
					tbn[nm] = int(tbn.get(nm, 0)) + 1


func _audio_live_count() -> int:
	# Players still presenting a stream (a stopped player remains a
	# node until _reset_audio frees it — "active" means playing).
	var n := 0
	for p in audio_players:
		if p != null and is_instance_valid(p) and p.playing:
			n += 1
	return n


func _stream_audio_census(tag: String, d: Dictionary) -> void:
	# 19B.3B1 — the mode-5 audio census: what the host consumed, the
	# play/stop split, the listener feed, the miss counters the
	# closure gate requires at zero, and the player-side truth.
	var wind_start := int(
		audio_stats["starts_by_name"].get("WIND", 0))
	var wind_stop := int(
		audio_stats["stops_by_name"].get("WIND", 0))
	var live := _audio_live_count()
	print(("  audio%s: ev=%d plays=%d(ensure=%d,restart=%d,loop=%d," +
		"pos=%d) stops=%d lstn=%d active=%d cap=%d " +
		"miss=%d/%d/%d cmds=%d starts=%d stops=%d live=%d " +
		"wind=%d/%d") % [
		tag,
		int(d["snd_events"]), int(d["snd_plays"]),
		int(d["snd_ensure_plays"]), int(d["snd_restart_plays"]),
		int(d["snd_loop_plays"]), int(d["snd_positional"]),
		int(d["snd_stops"]), int(d["snd_listener_updates"]),
		int(d["snd_active"]), int(d["snd_pool_exhausted"]),
		int(d["snd_unknown_tags"]), int(d["snd_resolve_misses"]),
		int(d["snd_decode_misses"]),
		int(audio_stats["cmds"]), int(audio_stats["starts"]),
		int(audio_stats["stops"]), live, wind_start, wind_stop])
	for e in d["snd_names"]:
		print("    asnd %s plays=%d stops=%d" % [
			String(e["name"]), int(e["plays"]), int(e["stops"])])
	_check(int(d["snd_unknown_tags"]) == 0,
		"audio: no unregistered sound tags")
	_check(int(d["snd_resolve_misses"]) == 0,
		"audio: no resource misses")
	_check(int(d["snd_decode_misses"]) == 0,
		"audio: no decode misses")
	_check(int(d["snd_pool_exhausted"]) == 0,
		"audio: no dropped voices (pool cap)")


func _free_audio_players() -> void:
	# Stop + free every player node — the transition sweep's half of
	# _reset_audio. The census (audio_stats) survives: a mid-route
	# mode flip frees stale nodes but must not erase the entry's own
	# counters before the smoke reads them.
	for i in audio_players.size():
		var p: AudioStreamPlayer = audio_players[i]
		if p != null and is_instance_valid(p):
			p.stop()
			p.stream = null
			audio_players[i] = null
			p.free()


func _reset_audio() -> void:
	# Restore/transition boundary — every live player belongs to the
	# discarded timeline. The bridge-side pool is already reset; the
	# GDScript side only stops and clears its nodes. Nothing audio
	# is serialized or resurrected.
	_free_audio_players()
	audio_stats = {"cmds": 0, "starts": 0, "params": 0,
		"stops": 0, "names": {}, "starts_by_name": {},
		"stops_by_name": {}}


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
	# Phase 17C.2 — audio voices from the discarded timeline stop;
	# nothing crosses the restore boundary.
	_reset_audio()
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
	obj_mats.clear()
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
	scope_cam.current = true   # same contract as the shot cams —
	                           # no current -> never-rendered texture
	# Same binding contract as the shot windows — get_texture(), not a
	# ViewportTexture.new() + viewport_path (that path never resolves
	# at runtime and the rect shows the checkerboard placeholder).
	sr.texture = scope_vp.get_texture()


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
#   kind-3 BANG frame-block overlay (+0x110 explodeFlag — the
#   EXPLODE model mesh renders), the ZOOM%04d intro sprite sequence
#   (zoomFrame surfaced as state), and palette cycling (the
#   0.5·frameUnits accumulator surfaces as palette_cycle but is not
#   applied to the presented palette). Presented elsewhere in this
#   file: the procedural FUN_00411f48 type-3 RADAR wedge, kind-4
#   trails + kind-5 launch glow (screen-reading LUT veils), and the
#   19C.3 sound bank set (freefallDrainAudio_).
# ---------------------------------------------------------------------------


func _ff_input() -> Dictionary:
	# Mode-2 direction channels — the dict booleans land directly on
	# FreefallInput (the same fold FUN_00407e50 gives the bound
	# direction keys: left=-X right=+X up=+Y down=-Y). Only TRUE
	# entries are emitted — the dict overrides the bound-key held()
	# state in stepFreefall_, so a false would mask a real bound-key
	# press that _gameplay_keys() already delivered.
	var d := {}
	if Input.is_key_pressed(KEY_LEFT) or Input.is_key_pressed(KEY_A):
		d["left"] = true
	if Input.is_key_pressed(KEY_RIGHT) or Input.is_key_pressed(KEY_D):
		d["right"] = true
	if Input.is_key_pressed(KEY_UP) or Input.is_key_pressed(KEY_W):
		d["up"] = true
	if Input.is_key_pressed(KEY_DOWN) or Input.is_key_pressed(KEY_S):
		d["down"] = true
	return d


func _ff_away_key(bo: Vector2) -> int:
	# Direction that runs AWAY from the bearing's dominant axis —
	# crossing the missile's track at the largest angle.
	if abs(bo.x) > abs(bo.y):
		return KEY_DOWN if bo.y > 0.0 else KEY_UP
	return KEY_LEFT if bo.x > 0.0 else KEY_RIGHT


func _inject_key(keycode: int, pressed: bool) -> void:
	# One synthetic device event: the polled keycode AND the physical
	# code are set, matching real hardware events — _ff_input polls
	# is_key_pressed, _gameplay_keys polls is_physical_key_pressed.
	var ev := InputEventKey.new()
	ev.keycode = keycode
	ev.physical_keycode = keycode
	ev.pressed = pressed
	Input.parse_input_event(ev)


func _ff_steer_inject(frame: int) -> void:
	# Reactive dodge, the way a player would steer: the nearest
	# inbound missile's lateral bearing decides the jink — move
	# PERPENDICULAR to it once the missile closes (the homing blend
	# vel*0.8+dir*0.2 can't re-track a 90-degree break at short
	# range); keep drifting far out so the lead never settles.
	var want := {}
	var s: Dictionary = bridge.get_freefall_snapshot()
	if s.has("player"):
		var p: Vector3 = s["player"]["pos_mdk"]
		var bd := -1.0
		var bo := Vector2.ZERO
		var have := false
		for o in bridge.get_freefall_object_snapshots():
			if int(o["type"]) != 1 or not o["alive"]:
				continue
			var m: Vector3 = o["pos_mdk"]
			var dz := m.z - p.z
			# Any live missile is a threat — they spawn ~5000 below
			# and slingshot up. Nearest-to-intercept = largest dz
			# still below the pass line (-5).
			if not have or dz > bd:
				bd = dz
				bo = Vector2(m.x - p.x, m.y - p.y)
				have = true
		if have:
			# Hold the perpendicular run through the intercept —
			# accelChannel sign-reversal RESETS velocity to the
			# ~12 increment, so reversing inside the lethal window
			# would stop the player dead. Sustained ~117 u/s
			# laterally is the only motion the homing blend
			# (vel*0.8+dir*0.2) cannot match while z-closing.
			var away := _ff_away_key(bo)
			want[away] = true
		else:
			# No inbound missile — release everything.
			_ff_jink = 0
	for k in [KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN]:
		var held: bool = _ff_held.get(k, false)
		if want.get(k, false) and not held:
			_inject_key(k, true)
			_ff_held[k] = true
		elif held and not want.get(k, false):
			_inject_key(k, false)
			_ff_held[k] = false


var _ff_trace_prev_hp := -1
var _ff_trace_prev_locks := -1
var _ff_trace_prev_rearms := -1
var _ff_trace_n := 0
var _ff_trace_hdr_done := false
var _ff_pickup_chute := {}
var ff_course_id := 0
var ff_skill := 0
var ff_seed := 12648430

# Build identity for trace headers — the producing commit when the
# launcher exports it (MDK_BUILD env), else the app version string.
func _ff_build_id() -> String:
	var b := OS.get_environment("MDK_BUILD")
	if not b.is_empty():
		return b
	return str(ProjectSettings.get_setting(
		"application/config/version", "dev"))

func _ff_trace_tick() -> void:
	# hp + nearest missile lateral separation, ~1 Hz sim cadence.
	_ff_trace_n += 1
	var s: Dictionary = bridge.get_freefall_snapshot()
	if s.is_empty() or not s.has("player"):
		return
	var p: Vector3 = s["player"]["pos_mdk"]
	# Units: sim seconds (dtSec accumulator), wall seconds,
	# sim tick (mode-2 steps), rendered frame, all pos in MDK units.
	var t := float(s["timeline"])
	var tw := Time.get_ticks_msec() / 1000.0
	var tick := _ff_steps
	var rframe := Engine.get_process_frames()
	if not _ff_trace_hdr_done:
		_ff_trace_hdr_done = true
		print("ff-trace-hdr build=%s course=%d skill=%d seed=%s wall0=%.2f" %
			[_ff_build_id(), ff_course_id, ff_skill,
			 "%08x" % ff_seed, tw])
	var n := 0
	var mind := -1.0
	var radar_submitted := false
	for o in bridge.get_freefall_object_snapshots():
		if not o["alive"]:
			continue
		var ty := int(o["type"])
		if ty == 4:
			# Pickup chute lifecycle — deploy edge + glide verification.
			var slot := int(o["pool_slot"]) if o.has("pool_slot") else int(o.get("slot", -1))
			var ch := bool(o["chute"])
			if ch and not _ff_pickup_chute.has(slot):
				_ff_pickup_chute[slot] = true
				var v: Vector3 = o["vel_mdk"]
				print("ff-trace CHUTE deploy slot=%d vz=%.1f timer=%d" %
					[slot, v.z, int(o["timer"])])
			elif ch and _ff_trace_n % 60 == 0:
				var v2: Vector3 = o["vel_mdk"]
				print("ff-trace CHUTE glide slot=%d vz=%.1f" % [slot, v2.z])
			continue
		if ty == 3 and o.has("beam_mdk"):
			# Type-3 radar — the lock test is the 2D distance² on the
			# scan plane (beam tx/ty vs player xy), threshold 15² = 225.
			# beam_mdk = tx/ty/tz; aux_mdk = aux0/aux1 (wander target)
			# + aux2 (scan plane z). MDK units, same frame as the test.
			var bm: Vector3 = o["beam_mdk"]
			var ax: Vector3 = o["aux_mdk"]
			var d2 := Vector2(bm.x - p.x, bm.y - p.y).length_squared()
			var phase := "scan"
			if bm.z < ax.z - 1.0:
				phase = "rise"
			elif int(o["timer"]) < 0:
				phase = "sink"
			var twn := int(s.get("radar_twin", 0)) > 0
			var verts := int(s.get("radar_verts", 0))
			if twn and verts > 0:
				radar_submitted = true
			if _ff_trace_n % 20 == 0 or d2 < 225.0:
				print("ff-radar t=%.2f w=%.2f tk=%d f=%d ph=%s beam=(%.1f,%.1f,%.1f) plane_z=%.1f tgt=(%.1f,%.1f) pl=(%.1f,%.1f,%.1f) d2=%.0f th=225 sub=%d" %
					[t, tw, tick, rframe, phase,
					 bm.x, bm.y, bm.z, ax.z, ax.x, ax.y,
					 p.x, p.y, p.z, d2,
					 1 if radar_submitted else 0])
			continue
		if ty != 1:
			continue
		n += 1
		var m: Vector3 = o["pos_mdk"]
		var dz := m.z - p.z
		var lat := Vector2(m.x - p.x, m.y - p.y).length()
		var trn := 0
		if o.has("trail"):
			trn = int(o["trail"].get("count", 0))
		var b: PackedFloat32Array = o["basis_mdk"]
		var v: Vector3 = o["vel_mdk"]
		var align := 0.0
		var nose_align := 0.0
		if b.size() >= 9 and v.length() > 0.001:
			# Row-major 3x3: column 1 (the missile's nose, local +Y)
			# sits at indices 1,4,7 and must equal norm(vel).
			align = Vector3(b[1], b[4], b[7]).normalized().dot(v.normalized())
			# The transformed-nose proof: run the actual element-0
			# max-Y vertex through the presented basis (geometry,
			# not the column scalar) — it must track velocity too.
			if o.has("nose_local"):
				var nl: Vector3 = o["nose_local"]
				var nw := Vector3(
					b[0] * nl.x + b[1] * nl.y + b[2] * nl.z,
					b[3] * nl.x + b[4] * nl.y + b[5] * nl.z,
					b[6] * nl.x + b[7] * nl.y + b[8] * nl.z)
				if nw.length() > 0.001:
					nose_align = nw.normalized().dot(v.normalized())
		if _ff_trace_n % 15 == 0 or (dz > -60.0 and dz < 60.0):
			var npens := -1
			if o.has("trail") and o["trail"].has("pens"):
				npens = int(o["trail"]["pens"].size())
			print("ff-msl t=%.2f rel=%.0f,%.0f,%.0f vel=%.0f,%.0f,%.0f col1=%.2f,%.2f,%.2f align=%.2f nose=%.2f trl=%d pens=%d fl=%d" %
				[float(s["timeline"]), m.x - p.x, m.y - p.y, dz,
				 v.x, v.y, v.z, b[1] if b.size() >= 9 else 0.0,
				 b[4] if b.size() >= 9 else 0.0, b[7] if b.size() >= 9 else 0.0,
				 align, nose_align, trn, npens, int(o.get("flare", 0))])
		if dz < -5.0 or dz > 600.0:
			continue   # passed or not yet inbound
		if mind < 0.0 or lat < mind:
			mind = lat
	# Causal-loop transitions — lock -> wave armed -> missiles ->
	# sink -> re-arm. Printed only on change so a quiet scan and an
	# absent radar read differently in the log.
	var locks := int(s.get("radar_locks", 0))
	var rearms := int(s.get("radar_rearms", 0))
	if locks != _ff_trace_prev_locks:
		if _ff_trace_prev_locks >= 0:
			print("ff-radar t=%.2f w=%.2f tk=%d LOCK #%d waves=%d budget=%d" %
				[t, tw, tick, locks, int(s.get("waves_armed", 0)),
				 int(s.get("missile_budget", 0))])
	if rearms != _ff_trace_prev_rearms:
		if _ff_trace_prev_rearms >= 0:
			print("ff-radar t=%.2f w=%.2f tk=%d REARM #%d timer=%d" %
				[t, tw, tick, rearms, int(s.get("radar_timer", 0))])
	_ff_trace_prev_locks = locks
	_ff_trace_prev_rearms = rearms
	if _ff_trace_n % 60 == 0:
		var keys: PackedInt32Array = _gameplay_keys()
		print("ff-radar t=%.2f w=%.2f tk=%d active=%d twin=%d verts=%d timer=%d budget=%d spawns=%d keys=%s" %
			[t, tw, tick, int(s.get("radar_active", 0)),
			 int(s.get("radar_twin", 0)),
			 int(s.get("radar_verts", 0)),
			 int(s.get("radar_timer", 0)),
			 int(s.get("missile_budget", 0)),
			 int(s.get("missiles_spawned", 0)),
			 str(keys)])
		print("ff-veil tk=%d ops=%d elems=%d cov=%d multi=%d ovf=%d" %
			[tick, int(ff_mask_diag.get("ops", -1)),
			 int(ff_mask_diag.get("elems", -1)),
			 int(ff_mask_diag.get("covered", -1)),
			 int(ff_mask_diag.get("multi", -1)),
			 int(ff_mask_diag.get("overflow", -1))])
	var hp := int(s["health"])
	if _ff_trace_prev_hp < 0:
		_ff_trace_prev_hp = hp
	elif hp != _ff_trace_prev_hp:
		print("ff-trace t=%.2f w=%.2f tk=%d hp %d->%d HIT msl=%d" %
			[t, tw, tick, _ff_trace_prev_hp, hp, n])
		_ff_trace_prev_hp = hp
	if _ff_trace_n % 60 == 0 or (mind >= 0.0 and mind < 30.0):
		print("ff-trace t=%.2f w=%.2f tk=%d hp=%d msl=%d mind_xy=%.1f pos=%.1f,%.1f" %
			[t, tw, tick, hp, n, mind, p.x, p.y])


# Physical keys the gameplay path polls every frame — every keycode
# _fe_internal_code can translate (the FUN_0046b688 device bitmap
# domain). Held state is polled, not event-latched, so window-focus
# transitions can't strand a key.
const _KEY_POLLED := [
	KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O,
	KEY_P, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K,
	KEY_L, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_0, KEY_ENTER, KEY_KP_ENTER, KEY_ESCAPE, KEY_BACKSPACE,
	KEY_TAB, KEY_SPACE, KEY_MINUS, KEY_EQUAL, KEY_BRACKETLEFT,
	KEY_BRACKETRIGHT, KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE,
	KEY_QUOTELEFT, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
	KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_PRINT, KEY_SCROLLLOCK,
	KEY_PAUSE, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE,
	KEY_END, KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
	KEY_NUMLOCK, KEY_KP_DIVIDE, KEY_KP_MULTIPLY, KEY_KP_SUBTRACT,
	KEY_KP_ADD, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5,
	KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_0, KEY_KP_PERIOD,
	KEY_CTRL, KEY_SHIFT, KEY_ALT, KEY_META,
]


func _gameplay_keys() -> PackedInt32Array:
	# Held physical keys -> the original's internal key codes (the
	# same domain the keyboard-capture menu writes into bindings).
	# This is the ONLY live keyboard path: KeySniper, KeyJump, the
	# item/weapon/zoom rows and every rebind resolve through the
	# configured table — nothing is hard-coded past the device map.
	var out := PackedInt32Array()
	for k in _KEY_POLLED:
		if Input.is_physical_key_pressed(k) or Input.is_key_pressed(k):
			var c := _fe_internal_code(k)
			if c >= 0:
				out.append(c)
	return out


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
	# Index 0 is the fill's transparent texel key (OBSERVED — the
	# EXPLODE fireball is a transparent-surround sprite). Opaque
	# textures stay in the depth-writing opaque pass; only
	# alpha-bearing textures enter the transparent pass.
	if bool(d.get("has_alpha", false)):
		tm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	tm.albedo_texture = d["tex"]
	var tw := float(d["w"])
	var th := float(d["h"])
	if tw > 0.0 and th > 0.0:
		tm.uv1_scale = Vector3(1.0 / tw, 1.0 / th, 1.0)
	ff_materials[mk] = tm
	return tm


func _ff_lut_material(row: int) -> ShaderMaterial:
	# FUN_0040c860 negative-pen class mi < -1028: dst = lut[row][dst]
	# — presented through the painter-last veil shader (the wedge's
	# object key sorts z' ~= 0, so the original composites it over
	# every already-drawn pixel: no depth test, top priority).
	var pk := "lut:%d" % row
	if ff_materials.has(pk):
		return ff_materials[pk]
	var m := ShaderMaterial.new()
	m.shader = ff_veil_top_shader if ff_veil_top_shader != null \
		else ff_veil_shader
	_ff_veil_params(m)
	m.set_shader_parameter("lut_row", clampi(row, 0, 63))
	m.render_priority = 1     # above trail/flare veils (priority 0)
	ff_materials[pk] = m
	return m


func _ff_bind_mesh(mi: MeshInstance3D, g: Dictionary) -> void:
	# The bridge mesh carries one surface per (element, material
	# index) group — materialize each through the shared bank.
	var mesh: ArrayMesh = g["mesh"]
	var names: PackedStringArray = g["surface_mats"]
	var pens: PackedInt32Array = g["surface_pen"]
	var midx: PackedInt32Array = g.get("surface_mat_idx",
		PackedInt32Array())
	for s in mesh.get_surface_count():
		var raw_mi := int(midx[s]) if s < midx.size() else 0
		if raw_mi < -1028:
			mesh.surface_set_material(s, _ff_lut_material(-1029 - raw_mi))
			continue
		var nm := String(names[s]) if s < names.size() else ""
		var pn := int(pens[s]) if s < pens.size() else -1
		mesh.surface_set_material(s, _ff_material(nm, pn))
	mi.mesh = mesh


# FUN_00412530 — the rendered backdrop. The core's freefall
# presentation step produces the original's 600x360 indexed frame
# (LEVEL%d scroll-sample through POD%d + the ZOOM span table + the
# generated 6-bank LUT + the L%d_C000%d pod chunk); the bridge
# palette-expands it to RGBA. The quad is a camera child sized to
# the frustum at far depth — the framebuffer IS the whole view in
# mode 2, so objects occlude it through the normal depth buffer.
func _ff_backdrop_quad() -> void:
	if ff_backdrop != null:
		return
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	var qm := QuadMesh.new()
	qm.material = mat
	ff_backdrop = MeshInstance3D.new()
	ff_backdrop.name = "FfBackdrop"
	ff_backdrop.mesh = qm
	$Camera3D.add_child(ff_backdrop)


func _ff_backdrop_frame(ff: Dictionary) -> void:
	_ff_backdrop_quad()
	var bdf: Dictionary = bridge.get_freefall_backdrop_frame()
	if bdf.is_empty():
		return
	# Required-resource readiness — a rejected ZOOM table or missing
	# LEVEL/POD must never pass as a valid all-black backdrop.
	if not bool(bdf.get("ready", false)):
		if _ff_backdrop_warned == 0:
			_ff_backdrop_warned += 1
			print("ff-backdrop NOT READY zoom=%s/16 mask=%s " %
					[bdf.get("zoom_count", -1),
					 bdf.get("zoom_mask", -1)] +
					"level=%s pod=%s lut=%s pal=%s chunks=%s " %
					[bdf.get("res_level", false),
					 bdf.get("res_pod", false),
					 bdf.get("res_lut", false),
					 bdf.get("res_palette", false),
					 bdf.get("res_chunks", -1)] +
					"flare4=%s pick=%s" %
					[bdf.get("res_flare4", false),
					 bdf.get("res_pick", false)])
		if ff_backdrop != null:
			ff_backdrop.visible = false
		return
	if ff_key_colors.is_empty():
		ff_key_colors = bdf["key_colors"]
	_ff_veil_setup(bdf)
	var w := int(bdf["w"])
	var h := int(bdf["h"])
	ff_backdrop_img = Image.create_from_data(w, h, false,
		Image.FORMAT_RGBA8, bdf["rgba"])
	if ff_backdrop_tex == null:
		ff_backdrop_tex = ImageTexture.create_from_image(
			ff_backdrop_img)
		(ff_backdrop.mesh.material as StandardMaterial3D) \
			.albedo_texture = ff_backdrop_tex
	else:
		ff_backdrop_tex.update(ff_backdrop_img)
	# Frustum-covering size at the quad's depth — 25000 units sits
	# behind every freefall object (world z max ~5300).
	var d := 25000.0
	var vh := 2.0 * d * tan(deg_to_rad($Camera3D.fov) * 0.5)
	var vw := vh * float(ff.get("aspect", 1.6667))
	(ff_backdrop.mesh as QuadMesh).size = Vector2(vw, vh)
	ff_backdrop.position = Vector3(0, 0, -d)
	ff_backdrop.visible = true


# Kind-4 — the missile trail veil is a world-space ImmediateMesh of
# section quads (o["trail"].edges/pens, the same world anchors the
# native compositor consumes) presented through ff_veil.gdshader —
# dst = lut[row][dst] on the already-drawn screen pixels, depth-
# tested against the 3D bodies. That replaces BOTH the key-color
# ribbon approximation and the under-all-bodies backdrop bake; the
# compositor stays in core for the headless path + tests.

var ff_idx_tex: ImageTexture = null   # 256x256 rgb565 -> index
var ff_lut_tex: ImageTexture = null   # 256x64 index -> remapped
var ff_pal_tex: ImageTexture = null   # 256x1 index -> RGB
var ff_veil_shader: Shader = null
var ff_veil_top_shader: Shader = null
var ff_veil_flare_shader: Shader = null
var ff_veil_mats := {}                # row -> ShaderMaterial (top)
var ff_flare_idx_tex: ImageTexture = null  # FLARE4 raw indices
# §4C — the serial veil-ordering target: per-px ordered {row,z'}
# records for every veil op this frame (RGBAH, 4 texels/px = 8
# records). The veil shaders replay the chain gated by the winning
# opaque depth instead of a single screen_texture remap.
var ff_mask_img: Image = null         # 2400x360 RGBAH
var ff_mask_tex: ImageTexture = null
var ff_mask_diag := {}                # ops/elems/overflow for traces


func _ff_veil_setup(bdf: Dictionary) -> void:
	if ff_veil_shader == null:
		ff_veil_shader = load("res://src/ff_veil.gdshader")
		ff_veil_top_shader = load("res://src/ff_veil_top.gdshader")
		ff_veil_flare_shader = load("res://src/ff_veil_flare.gdshader")
	if ff_pal_tex != null or ff_palette.is_empty():
		return
	# One-time LUT/lookup upload (fixed for the course): pal_tex
	# resolves index->RGB, idx_tex inverts exact palette colors
	# back to their index, lut_tex is the remap table itself.
	var pal_img := Image.create(256, 1, false, Image.FORMAT_RGB8)
	for i in 256:
		pal_img.set_pixel(i, 0, Color(ff_palette[i * 3] / 255.0,
			ff_palette[i * 3 + 1] / 255.0,
			ff_palette[i * 3 + 2] / 255.0))
	ff_pal_tex = ImageTexture.create_from_image(pal_img)
	var idx_img := Image.create(256, 256, false, Image.FORMAT_R8)
	for i in 256:
		var r: int = ff_palette[i * 3]
		var g: int = ff_palette[i * 3 + 1]
		var b: int = ff_palette[i * 3 + 2]
		var k: int = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
		idx_img.set_pixel(k & 255, k >> 8, Color(i / 255.0, 0, 0))
	ff_idx_tex = ImageTexture.create_from_image(idx_img)
	var lut: PackedByteArray = bdf.get("lut", PackedByteArray())
	if lut.size() >= 64 * 256:
		var lut_img := Image.create_from_data(256, 64, false,
			Image.FORMAT_R8, lut)
		ff_lut_tex = ImageTexture.create_from_image(lut_img)


func _ff_veil_params(m: ShaderMaterial) -> ShaderMaterial:
	m.set_shader_parameter("idx_tex", ff_idx_tex)
	m.set_shader_parameter("lut_tex", ff_lut_tex)
	m.set_shader_parameter("pal_tex", ff_pal_tex)
	m.set_shader_parameter("mask_tex", ff_mask_tex)
	return m


# §4C — per-frame mask upload. The bridge rasterizes this frame's
# ordered veil ops (trail sections, kind-5 flare, radar wedge tris)
# into per-px {row,z'} records; the shader walks them so a px under
# K veils gets the serial chain L_k[...L_1[p]], gated by the opaque
# depth winner (a buried element was overwritten, not remapped).
func _ff_mask_update() -> void:
	var m: Dictionary = bridge.ff_veil_mask()
	if m.is_empty():
		return
	ff_mask_diag = {
		"ops": m.get("ops", 0),
		"elems": m.get("elems", 0),
		"overflow": m.get("overflow", 0),
		"covered": m.get("covered", 0),
		"multi": m.get("multi", 0),
	}
	ff_mask_img = Image.create_from_data(int(m["w"]), int(m["h"]),
		false, Image.FORMAT_RGBAH, m["data"])
	if ff_mask_tex == null:
		ff_mask_tex = ImageTexture.create_from_image(ff_mask_img)
		# Materials created before the first upload hold a null
		# mask_tex — rebuild them once it exists.
		ff_veil_mats.clear()
		ff_flare_veil_mats.clear()
		ff_materials.clear()   # lut: wedge entries ride the mask too
	else:
		ff_mask_tex.update(ff_mask_img)


func _ff_veil_trail_mat() -> ShaderMaterial:
	if not ff_veil_mats.has("trail"):
		var m := ShaderMaterial.new()
		m.shader = ff_veil_shader
		_ff_veil_params(m)
		m.set_shader_parameter("row_from_color", true)
		m.render_priority = 0
		ff_veil_mats["trail"] = m
	return ff_veil_mats["trail"]


# Kind-4 trail ribbon — edges are Godot-world pairs per ring slot
# (oldest->newest, taper already applied); each section s pairs
# slots s-1,s and carries pen -> LUT row (-1029 - pen).
func _ff_apply_trail(node: Node3D, o: Dictionary) -> void:
	var mi: MeshInstance3D = node.get_node_or_null("Trail")
	var t: Dictionary = o.get("trail", {})
	var edges: PackedVector3Array = t.get("edges", PackedVector3Array())
	var pens: PackedInt32Array = t.get("pens", PackedInt32Array())
	var nsec: int = min(pens.size(), int(t.get("count", 0)) - 1)
	if nsec < 1 or edges.size() < (nsec + 1) * 2 or \
			ff_lut_tex == null or not bool(o.get("presented", false)):
		if mi != null:
			mi.visible = false
		return
	if mi == null:
		mi = MeshInstance3D.new()
		mi.name = "Trail"
		mi.mesh = ImmediateMesh.new()
		mi.top_level = true
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		node.add_child(mi)
	var im := mi.mesh as ImmediateMesh
	im.clear_surfaces()
	im.surface_begin(Mesh.PRIMITIVE_TRIANGLES, _ff_veil_trail_mat())
	for s in nsec:
		var pen := int(pens[s])
		if pen >= -1028:
			continue      # -1028 textured cap — no veil op in mode 2
		var row := -1029 - pen
		if row < 0 or row > 63:
			continue
		var col := Color(row / 255.0, 0.0, 0.0)
		var a0: Vector3 = edges[s * 2]
		var b0: Vector3 = edges[s * 2 + 1]
		var a1: Vector3 = edges[s * 2 + 2]
		var b1: Vector3 = edges[s * 2 + 3]
		im.surface_set_color(col)
		im.surface_add_vertex(a0)
		im.surface_set_color(col)
		im.surface_add_vertex(b0)
		im.surface_set_color(col)
		im.surface_add_vertex(b1)
		im.surface_set_color(col)
		im.surface_add_vertex(a0)
		im.surface_set_color(col)
		im.surface_add_vertex(b1)
		im.surface_set_color(col)
		im.surface_add_vertex(a1)
	im.surface_end()
	mi.visible = true


# Kind-5 — the launch FLARE. FLARE4's pixels (0..8) index the LUT
# at row 6 + obj+0x108 + srcPx (OBSERVED 0x410dfb-0x410e10: the blit
# LUT arg = 0x4edc34+0x600 + count<<8; 0x46d6d1: px0 transparent,
# else dst = lut[row][dst]) — presented by ff_veil_flare.gdshader:
# the source texel rides the row term, the sampled dst rides the
# remap, so the presented pixels ARE the indexed operation rather
# than a pre-baked key-color billboard.
var ff_flare_veil_mats := {}

func _ff_flare_veil_mat(count: int) -> ShaderMaterial:
	var key: int = clampi(count, 1, 8)
	if ff_flare_veil_mats.has(key):
		return ff_flare_veil_mats[key]
	var m := ShaderMaterial.new()
	m.shader = ff_veil_flare_shader
	_ff_veil_params(m)
	m.set_shader_parameter("src_tex", ff_flare_idx_tex)
	m.set_shader_parameter("base_row", 6.0 + float(key))
	m.render_priority = 0
	ff_flare_veil_mats[key] = m
	return m


func _ff_apply_flare(node: Node3D, o: Dictionary) -> void:
	var fl := int(o.get("flare", 0))
	var mi: MeshInstance3D = node.get_node_or_null("Flare")
	if fl <= 0 or ff_flare_idx_tex == null or \
			not bool(o.get("presented", false)):
		if mi != null:
			mi.visible = false
		return
	if mi == null:
		mi = MeshInstance3D.new()
		mi.name = "Flare"
		mi.mesh = QuadMesh.new()
		mi.top_level = true
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		node.add_child(mi)
	(mi.mesh as QuadMesh).material = _ff_flare_veil_mat(fl)
	# 0x46d6d1's scale arg 0x80 -> 32x32 SCREEN px at the projected
	# pos (src 64x64 * 0x80 >> 8); convert to world units at the
	# missile's depth. The draw sorts at z+10 — a small camera-
	# ward offset reproduces the in-front-of-body layering.
	var wp: Vector3 = node.transform.origin
	var cam: Vector3 = $Camera3D.global_position
	var dist := cam.distance_to(wp)
	var wpp := 2.0 * dist * tan(deg_to_rad($Camera3D.fov) * 0.5) \
		/ 360.0
	var s := 32.0 * wpp
	(mi.mesh as QuadMesh).size = Vector2(s, s)
	mi.global_position = wp + (cam - wp).normalized() * 10.0
	mi.look_at(cam)   # billboard — the veil shader has no billboard mode
	mi.visible = true


# Kind-1 — the +0x10c marker: the PICK sprite through 0x46d680's
# center-pos scaled blit (OBSERVED 0x410bf2):
#   scale = trunc(viewW*32.0 / (z'*zoom)) = trunc(8000/z')  [z' = camZ - pz,
#           the raw M2 row2 (0,0,-1) view depth; gate z' > 0]
#   outPx = srcPx * scale >> 8                            [0x403a40]
# The constant sits in the exe image as double 32.0 at 0x494d30 —
# fmul QWORD reads all 8 bytes (00..00 40 40 = 32.0; the dword-only
# view reads 0, and 3.0 would encode 00..00 08 40).
# Presented as a billboard quad sized so it projects to outPx
# pixels — same world-per-pixel convention the FLARE quad uses.
func _ff_apply_marker(node: Node3D, o: Dictionary, ff: Dictionary) -> void:
	var mi: MeshInstance3D = node.get_node_or_null("Marker")
	var pm: Vector3 = o.get("pos_mdk", Vector3.ZERO)
	var zp := float(ff["camera_pos_mdk"].z) - pm.z
	var shown := bool(o.get("marker", false)) and \
		ff_pick_tex != null and bool(o.get("presented", false)) and \
		zp > 0.0 and ff_pick_wh.x > 0
	var sc := 0
	var ox := 0
	var oy := 0
	if shown:
		sc = int(8000.0 / zp)         # trunc(600*32.0/(z'*2.4))
		ox = (ff_pick_wh.x * sc) >> 8
		oy = (ff_pick_wh.y * sc) >> 8
		shown = ox > 0 and oy > 0
	if not shown:
		if mi != null:
			mi.visible = false
		return
	if mi == null:
		mi = MeshInstance3D.new()
		mi.name = "Marker"
		var qm := QuadMesh.new()
		var m := StandardMaterial3D.new()
		m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		m.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
		m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		m.albedo_texture = ff_pick_tex
		qm.material = m
		mi.mesh = qm
		mi.top_level = true
		node.add_child(mi)
	var wp: Vector3 = node.transform.origin
	var dist: float = $Camera3D.global_position.distance_to(wp)
	var wpp: float = 2.0 * dist * tan(deg_to_rad($Camera3D.fov) * 0.5) \
		/ 360.0
	(mi.mesh as QuadMesh).size = Vector2(ox * wpp, oy * wpp)
	mi.global_position = wp
	mi.visible = true


func _ff_sprites_load() -> void:
	var d: Dictionary = bridge.get_freefall_sprites()
	if d.has("flare4") and ff_flare_px.is_empty():
		var s: Dictionary = d["flare4"]
		ff_flare_wh = Vector2i(int(s["w"]), int(s["h"]))
		ff_flare_px = s["pixels"]
		# Raw-index upload for the veil shader's srcPx row term.
		var fimg := Image.create_from_data(ff_flare_wh.x,
			ff_flare_wh.y, false, Image.FORMAT_R8, ff_flare_px)
		ff_flare_idx_tex = ImageTexture.create_from_image(fimg)
	if d.has("pick") and ff_pick_tex == null:
		var s: Dictionary = d["pick"]
		var w := int(s["w"])
		var h := int(s["h"])
		ff_pick_wh = Vector2i(w, h)
		var px: PackedByteArray = s["pixels"]
		var rgba := PackedByteArray()
		rgba.resize(w * h * 4)
		for i in w * h:
			var c := int(px[i])
			if c == 0 or ff_palette.size() != 768:
				rgba[i * 4 + 3] = 0
				continue
			rgba[i * 4] = ff_palette[c * 3]
			rgba[i * 4 + 1] = ff_palette[c * 3 + 1]
			rgba[i * 4 + 2] = ff_palette[c * 3 + 2]
			rgba[i * 4 + 3] = 255
		var img := Image.create_from_data(w, h, false,
			Image.FORMAT_RGBA8, rgba)
		ff_pick_tex = ImageTexture.create_from_image(img)


func _apply_freefall() -> void:
	var ff: Dictionary = bridge.get_freefall_snapshot()
	if ff.is_empty():
		return
	# Traversal HUD/view is mode-3 only — never over freefall.
	_hide_hud()
	# A prior mode-5 frame never overlaps the freefall view.
	if $StreamLayer.visible:
		$StreamLayer.visible = false
	# Course-change invalidation (§4B): FALLP_<c+1> differs between
	# courses (123-128 of 256 entries), so every palette-derived cache
	# must rebuild when the snapshot's palette changes — the inverse
	# rgb565 map, the pal/lut textures, the row-keyed veil materials
	# (they hold the OLD texture objects), and the palette-expanded
	# PICK sprite. Stale entries would map ~119 colors to index 0.
	var pal_now: PackedByteArray = ff.get("palette", PackedByteArray())
	if bool(ff.get("palette_ok", false)) and pal_now.size() == 768 and \
			(ff_palette.is_empty() or ff_palette != pal_now):
		ff_palette = pal_now
		ff_pal_tex = null
		ff_idx_tex = null
		ff_lut_tex = null
		ff_veil_mats.clear()
		ff_flare_veil_mats.clear()
		ff_materials.clear()   # pen:/m:/lut: entries are palette-derived
		ff_pick_tex = null

	# FUN_004123f4's camera — fixed orientation (right=+X, down=-Y,
	# back=+Z semantic rows) at cameraPos, fov from scaleY at the
	# 600x360 projection divisors. Far covers the camera-locked
	# backdrop quad (placed at 25000).
	var ct: Transform3D = ff["camera"]
	$Camera3D.global_transform = ct
	$Camera3D.fov = float(ff["fov_deg"])
	$Camera3D.far = 50000.0

	# FUN_00412530 — the per-frame rendered backdrop (LEVEL/POD
	# sampler + ZOOM dither + LUT + chunk sprite) on the camera-
	# locked quad, and the one-shot FLARE4/PICK sprite loads.
	_ff_backdrop_frame(ff)
	if ff_flare_px.is_empty() or ff_pick_tex == null:
		_ff_sprites_load()
	# §4C — this frame's ordered veil records for the serial chain.
	_ff_mask_update()

	# 0x4edc04 — palette-bright factor applied at upload. The original
	# picks the dim target per state (OBSERVED mode-2 dispatch):
	#   intro t>90   0x40fb08  BLACK  (fade-in)
	#   intro t<60   0x40fba0  WHITE  (atmosphere-entry white-out)
	#   play ramps   0x40fba0  WHITE  (post-intro ramp-in + dips)
	#   death        0x40fb08  BLACK  (541554 <= 0 -> 0x4106e3)
	#   exit         0x40fb08  BLACK  (timeline > 31 -> 0x4107b4)
	#   fade>1       saturating white (damage flash)
	var fade := float(ff["fade"])
	var fr: ColorRect = $FadeLayer/FadeRect
	var ff_intro := int(ff.get("intro_countdown", 0))
	if fade < 1.0:
		var to_white := false
		if ff_intro > 0:
			to_white = ff_intro < 60
		else:
			to_white = int(ff["health"]) > 0 and \
				float(ff["timeline"]) <= 31.0
		if to_white:
			fr.color = Color(1, 1, 1, 1.0 - fade)
		else:
			fr.color = Color(0, 0, 0, 1.0 - fade)
		fr.visible = true
	elif fade > 1.0:
		# Saturating multiply approximated as a white-out — the
		# original clamps every channel at 255 (fade=3 -> alpha ~.67).
		fr.color = Color(1, 1, 1, min(1.0, 1.0 - 1.0 / fade))
		fr.visible = true
	else:
		fr.visible = false

	# FUN_00417e20 — the mode-2 SC_STAT health gauge + digits (draws
	# only once the descent draw block starts running).
	_apply_ff_hud()

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
		# Kind-4 — the trail veil ribbon (world-space sections,
		# dst = lut[row][dst] at real depth).
		_ff_apply_trail(node, o)
		# Kind-5 — the launch FLARE4 veil quad (obj+0x108 gate).
		_ff_apply_flare(node, o)
		# Kind-1 — the +0x10c PICK marker (pickups).
		_ff_apply_marker(node, o, ff)
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

	# --ff-shot-pass trigger: an inbound missile inside the close
	# window (still below, lateral-near) arms the screenshot for the
	# next _process — the visible-pass acceptance capture.
	if ff_shot_pass and shot_frames_left == -1 and ff.has("player"):
		var pp: Vector3 = ff["player"]["pos_mdk"]
		for o in snaps:
			if not o["alive"] or int(o["type"]) != 1:
				continue
			var mp: Vector3 = o["pos_mdk"]
			var dz := mp.z - pp.z
			var lat := Vector2(mp.x - pp.x, mp.y - pp.y).length()
			var dmin := float(_shot_pass_lo)
			var dmax := float(_shot_pass_hi)
			if dz > dmin and dz < dmax and lat < 45.0:
				shot_frames_left = 1
				break

	_update_ff_debug(ff)


# --- Phase 19B.1 — mode-5 intermission presentation ------------------
# bridge.stream_frame() returns the last presented indexed frame
# expanded through the applied 768B palette (the DAC surface — fades
# and the terminal fill live in it). The texture is a persistent
# ImageTexture updated in place, same contract as the frontend.

func _apply_stream() -> void:
	var fr: Dictionary = bridge.stream_frame()
	if fr.is_empty():
		return
	var w := int(fr["w"])
	var h := int(fr["h"])
	st_img = Image.create_from_data(w, h, false, Image.FORMAT_RGBA8,
		fr["rgba"])
	if st_tex == null:
		st_tex = ImageTexture.create_from_image(st_img)
	else:
		st_tex.update(st_img)
	var r: TextureRect = $StreamLayer/StreamRect
	r.texture = st_tex
	r.visible = true
	$StreamLayer.visible = true


# --- Phase 19E — mode-6 briefing presentation ------------------------
# bridge.mode6_frame() returns the briefing's composited indexed frame
# (L%d_MAP + the typed BRIEF%d page) expanded through the applied DAC —
# fade-in, the settled palette, and fade-out all live in it. Same
# persistent-ImageTexture contract as the frontend/stream presenters.
var m6_img: Image = null
var m6_tex: ImageTexture = null

func _apply_mode6() -> void:
	var fr: Dictionary = bridge.mode6_frame()
	if fr.is_empty():
		return
	var w := int(fr["w"])
	var h := int(fr["h"])
	m6_img = Image.create_from_data(w, h, false, Image.FORMAT_RGBA8,
		fr["rgba"])
	if m6_tex == null:
		m6_tex = ImageTexture.create_from_image(m6_img)
	else:
		m6_tex.update(m6_img)
	var r: TextureRect = $BriefingLayer/BriefingRect
	r.texture = m6_tex
	r.visible = true
	$BriefingLayer.visible = true

func _mode6_hide() -> void:
	if $BriefingLayer.visible:
		$BriefingLayer.visible = false


# --- Freefall teletype (FALL_T1) -------------------------------------
# bridge.ff_teletype_frame() returns the FUN_0041cb44 service draw —
# the FONTSML/FONTBIG lines on a transparent strip, expanded through
# the resident SYS_PAL head. Overlay contract matches the briefing
# presenter (full-rect, alpha-composited over the 3D descent).
var tt_img: Image = null
var tt_tex: ImageTexture = null

func _apply_ff_teletype() -> void:
	var fr: Dictionary = bridge.ff_teletype_frame()
	if fr.is_empty():
		_tt_hide()
		return
	var w := int(fr["w"])
	var h := int(fr["h"])
	tt_img = Image.create_from_data(w, h, false, Image.FORMAT_RGBA8,
		fr["rgba"])
	if tt_tex == null:
		tt_tex = ImageTexture.create_from_image(tt_img)
	else:
		tt_tex.update(tt_img)
	var r: TextureRect = $TtLayer/TtRect
	r.texture = tt_tex
	r.visible = true
	$TtLayer.visible = true

func _tt_hide() -> void:
	if $TtLayer.visible:
		$TtLayer.visible = false


# --- Freefall health HUD (FUN_00417e20) --------------------------------
# bridge.ff_hud_frame() returns the SC_STAT gauge + 0x541554 digits
# as a transparent RGBA overlay (FALLP-expanded). Only drawn once the
# descent draw block runs — the intro's early return skips it.
var ff_hud_img: Image = null
var ff_hud_tex: ImageTexture = null

func _apply_ff_hud() -> void:
	var fr: Dictionary = bridge.ff_hud_frame()
	if fr.is_empty() or not bool(fr.get("show", false)):
		_ff_hud_hide()
		return
	var w := int(fr["w"])
	var h := int(fr["h"])
	ff_hud_img = Image.create_from_data(w, h, false,
		Image.FORMAT_RGBA8, fr["rgba"])
	if ff_hud_tex == null:
		ff_hud_tex = ImageTexture.create_from_image(ff_hud_img)
	else:
		ff_hud_tex.update(ff_hud_img)
	var r: TextureRect = $FfHudLayer/FfHudRect
	r.texture = ff_hud_tex
	r.visible = true
	$FfHudLayer.visible = true

func _ff_hud_hide() -> void:
	if $FfHudLayer.visible:
		$FfHudLayer.visible = false


# --- Phase 19D — mode-8 ending cinematic presentation -----------------
# One FLIC frame per paced ~33.3ms call into the bridge (the file's
# speed field is the limiter window — the same policy as mode 5).
# ending_frame() hands back the decoded surface pre-expanded through
# the effective palette (FUN_0047b384's white ramp included).
const END_STEP_MS := 1000.0 / 30.0
var end_pace_ms := 0.0
var end_pace_armed := false
var end_img: Image = null
var end_tex: ImageTexture = null
var end_seq := -1
var end_last_ramp := -1.0
var end_players := {}          # FINISH.BNI name -> AudioStreamPlayer
var end_mve_player: AudioStreamPlayer = null   # 19E soundtrack
var end_mve_live := false
var end_abort_at := -1     # --ending-abort N (QA): edge at seq>=N
var end_repeat := false    # --ending-repeat (QA): re-enter once
var end_repeat_done := false

func _ending_pace_run(delta_ms: float) -> int:
	if int(bridge.get_mode()) != 8:
		end_pace_armed = false
		return 0
	if not end_pace_armed:
		# First live tick — present frame 0 immediately (same
		# dead-window skip as the stream pacer).
		end_pace_armed = true
		end_pace_ms = END_STEP_MS
	end_pace_ms += delta_ms
	var n := int(end_pace_ms / END_STEP_MS)
	if n > STREAM_MAX_CATCHUP:
		n = STREAM_MAX_CATCHUP
		end_pace_ms = 0.0
	else:
		end_pace_ms -= n * END_STEP_MS
	# 19E — key edges feed the MVE-stage abort check (the FLIC stage
	# ignores input); _fe_input() consumes this frame's edges.
	var input := _fe_input()
	# QA seam: while the threshold holds, every paced tick carries a
	# raw key edge — a zero-step call can't silently eat the abort.
	if end_abort_at >= 0 and \
			int(bridge.ending_diag().get("stage", 0)) == 2 and \
			int(bridge.ending_diag().get("seq", 0)) >= end_abort_at:
		input["raw_edges"] = PackedInt32Array([1, 0, 0, 0])
	var ran := 0
	while ran < n and int(bridge.get_mode()) == 8:
		bridge.step_frame_input(END_STEP_MS, input)
		input = {}       # edges are consumed once
		ran += 1
	if int(bridge.get_mode()) != 8:
		end_abort_at = -1
	return ran

func _apply_ending() -> void:
	var fr: Dictionary = bridge.ending_frame()
	if fr.is_empty():
		return
	# Re-upload on a new decoded frame OR a ramp change (the 0xe9
	# hold stalls seq while the palette blend may still move).
	var seq := int(fr["seq"])
	var ramp := float(fr["ramp"])
	if seq == end_seq and ramp == end_last_ramp:
		return
	end_seq = seq
	end_last_ramp = ramp
	end_img = Image.create_from_data(int(fr["w"]), int(fr["h"]),
		false, Image.FORMAT_RGBA8, fr["rgba"])
	# FLIC 600x360 -> MVE 432x320 crosses the boundary: update()
	# requires identical size, so re-create on a shape change.
	if end_tex == null or \
			end_tex.get_width() != end_img.get_width() or \
			end_tex.get_height() != end_img.get_height():
		end_tex = ImageTexture.create_from_image(end_img)
	else:
		end_tex.update(end_img)
	var r: TextureRect = $EndingLayer/EndingRect
	# 19E — the MVE stage keeps the movie's authored aspect (the
	# original's FUN_00489a50 allocates a 4:3 surface); FLIC keeps
	# the original stretched presentation.
	var aspect := TextureRect.STRETCH_KEEP_ASPECT_CENTERED \
		if bool(fr.get("keep_aspect", false)) \
		else TextureRect.STRETCH_SCALE
	if r.stretch_mode != aspect:
		r.stretch_mode = aspect
	r.texture = end_tex
	r.visible = true
	$EndingLayer.visible = true

func _ending_drain_audio() -> void:
	for ev in bridge.ending_drain_audio():
		var nm := String(ev.get("name", ""))
		var op := String(ev.get("op", ""))
		if op == "play" and nm != "" and ev.get("stream") != null:
			var p: AudioStreamPlayer = end_players.get(nm)
			if p == null:
				p = AudioStreamPlayer.new()
				p.name = "EndSnd_" + nm
				$AudioRoot.add_child(p)
				end_players[nm] = p
			p.stream = ev["stream"]
			p.volume_db = float(ev.get("vol", 0.0))
			p.play()
		elif op == "stop" and nm != "":
			var q: AudioStreamPlayer = end_players.get(nm)
			if q != null:
				q.stop()
		elif op == "mve_audio" and ev.get("stream") != null:
			if end_mve_player == null:
				end_mve_player = AudioStreamPlayer.new()
				end_mve_player.name = "EndMve"
				$AudioRoot.add_child(end_mve_player)
			end_mve_player.stream = ev["stream"]
			end_mve_player.volume_db = float(ev.get("vol", 0.0))
			end_mve_player.play()
			end_mve_live = true
		elif op == "mve_stop":
			if end_mve_player != null:
				end_mve_player.stop()
			end_mve_live = false
	# The soundtrack's playback position is the movie clock —
	# reported live while playing so the bridge can hold video pts
	# sync against the hardware audio position.
	if end_mve_player != null:
		if end_mve_player.playing:
			bridge.ending_set_audio_clock(
				end_mve_player.get_playback_position())
		elif end_mve_live:
			# Natural end of the WAV — the bridge falls back to its
			# own wall clock for the remaining video tail.
			bridge.ending_set_audio_clock(-1.0)
			end_mve_live = false

func _ending_hide() -> void:
	if $EndingLayer.visible:
		$EndingLayer.visible = false
		$EndingLayer/EndingRect.visible = false
	for p in end_players.values():
		p.stop()
		p.queue_free()
	end_players.clear()
	if end_mve_player != null:
		end_mve_player.stop()
		end_mve_player.queue_free()
		end_mve_player = null
	end_mve_live = false
	end_pace_armed = false
	end_seq = -1
	end_last_ramp = -1.0


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

	# Veil path — the LUT/lookup textures the veil shaders read
	# must exist, and a live type-3 radar must bind a surfaced
	# mesh whose material carries the veil shader.
	_check(ff_idx_tex != null and ff_lut_tex != null and
		ff_pal_tex != null, "veil lookup textures bound")
	for o in bridge.get_freefall_object_snapshots():
		if int(o["type"]) != 3 or not o["alive"]:
			continue
		var rslot := int(o["pool_slot"])
		var rnode := $FreefallRoot.get_node_or_null(
			"FFObj_%d" % rslot)
		if rnode == null:
			continue
		var rbody: MeshInstance3D = rnode.get_node("Body")
		_check(rbody.mesh != null and
			rbody.mesh.get_surface_count() > 0,
			"radar twin presents a surfaced mesh")
		if rbody.mesh != null and \
				rbody.mesh.get_surface_count() > 0:
			var rm := rbody.mesh.surface_get_material(0)
			_check(rm != null and rm is ShaderMaterial and
				(rm as ShaderMaterial).shader == ff_veil_top_shader,
				"radar wedge rides the painter-last veil shader")
		break

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
	var saw_trail_mesh := false
	for i in 1400:
		var r: Dictionary = _step_n({}, 1)
		for o in bridge.get_freefall_object_snapshots():
			seen_types[int(o["type"])] = true
			if int(o["type"]) == 0:
				seen_anims[int(o["anim_handle"])] = true
			if bool(o.get("chute", false)):
				saw_chute = true
		if i % 20 == 0 and not saw_trail_mesh:
			_apply_freefall()
			for child in $FreefallRoot.get_children():
				var tm: MeshInstance3D = \
					child.get_node_or_null("Trail")
				if tm != null and tm.visible and \
						tm.mesh != null and \
						tm.mesh.get_surface_count() > 0:
					saw_trail_mesh = true
					# The overlap contract: the trail binds the
					# depth-tested veil shader (the top/painter-last
					# variant is the radar wedge's alone).
					var sm := tm.get_active_material(0) as ShaderMaterial
					_check(sm != null and \
							sm.shader == ff_veil_shader,
						"trail veil uses the depth-tested shader")
					_check(tm.top_level,
						"trail veil in world space (real depth)")
		if bool(r.get("done", false)):
			done = true
			route = int(r.get("handoff_route", -1))
			break
	_check(saw_trail_mesh,
		"kind-4 trail veil mesh presented during the run")
	_check(done, "freefall course completed")
	print("  types seen: %s  player anims: %s  chute: %s" %
		[seen_types.keys(), seen_anims.keys(), saw_chute])
	# 19C.3 — the mode-2 audio pass: the run's kFfEvSound events fed
	# the shared mixer; the completion frame's stopAll queued the
	# bank-teardown tail. Drain once (the scripted smoke drives
	# steps directly — _process never interleaves) and census.
	_drain_audio_fx()
	_check(int(audio_stats["starts"]) > 0,
		"audio: freefall produced voice starts")
	print("  audio names: %s" % str(audio_stats["names"].keys()))
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


func _run_smoke_stream(course: int) -> void:
	# Headless mode-5 presentation smoke (Phase 19B.1 + 19B.2A +
	# 19B.2B1). Drives the same step path _process uses; asserts the
	# host-side contract: indexed composition counters, palette
	# uploads, the natural kExitMode exit, the terminal fill byte,
	# the model-geometry submission census (lookup misses and invalid
	# geometry must stay zero), and the ribbon raster census.
	print("smoke(stream): course=%d" % course)
	_check(int(bridge.get_mode()) == 5, "mode == 5 (stream)")
	_check(bridge.stream_active(), "stream_active after load")

	var res := {}
	var frames := 0
	var exited := false
	# Phase 19B.1A — optional framebuffer evidence: --stream-shots DIR
	# captures early/mid/near-exit presented frames as PNGs. A sparse
	# checkpoint ring (every 20 presented frames + seq 2) keeps the
	# mid pick deterministic; the last presented frame is kept for
	# the near-exit shot (the kExitMode fill never overwrites it in
	# the ring — the exit frame is a uniform palette, not a scene).
	var shot_dir := stream_shot_dir
	var st_ckpt := {}          # seq -> {rgba, fb, pal, spr dict}
	var st_last := {}          # last kPresent frame dict
	var st_last_seq := -1
	while frames < 2200:
		res = bridge.step_frame_input(33.333, {"actions": 0})
		frames += 1
		# 19B.3B1 — the voice commands drain once per step (the same
		# cadence the live path's _process drain uses); on the exit
		# step this also delivers the teardown tail's stops.
		_drain_audio_fx()
		if not bool(res.get("ok", true)):
			_check(false, "stream step failed: %s" %
				bridge.get_last_error())
			return
		if bool(res.get("frame_ready", false)) and \
				not shot_dir.is_empty():
			var fr_now: Dictionary = bridge.stream_frame()
			if not fr_now.is_empty():
				var sq := int(res.get("seq", 0))
				var dnow: Dictionary = fr_now["diag"]
				var entry := {
					"seq": sq,
					"rgba": fr_now["rgba"],
					"fb": int(dnow["fb_hash"]),
					"pal": int(dnow["palette_hash"]),
					"spr": int(dnow["sprites"]),
					"drawn": int(dnow["sprite_drawn"]),
				}
				if not bool(res.get("exited", false)):
					st_last = entry
					st_last_seq = sq
					if sq <= 2 or sq % 20 == 0:
						st_ckpt[sq] = entry
		if bool(res.get("exited", false)):
			exited = true
			break
	_check(exited, "stream reaches natural exit")
	_apply_stream()
	var d: Dictionary = bridge.stream_diag()
	_check(int(d["terminal_fills"]) == 1, "one terminal fill")
	# OBSERVED exit fills: 0xff for courses 0..3 (alive, non-final),
	# 0x00 for course 4 (the final/dead path).
	var want_fill := 0x00 if course >= 4 else 0xff
	_check(int(d["terminal_fill"]) == want_fill,
		"terminal fill == 0x%02x" % want_fill)
	_check(int(d["presented"]) > 0, "frames presented")
	_check(int(d["backdrop_blits"]) > 0, "backdrop blits")
	_check(int(d["sprites"]) > 0, "sprite draw events")
	# 19B.1A closure gate — a sprite that reached the presenter with
	# an unbound tag or an unusable bound image is a true resource
	# miss; both counters must stay zero. Clipped/zero-size/keyed
	# outcomes are faithful FUN_00403a40 raster results, tracked
	# separately (never "misses").
	_check(int(d["sprite_miss_res"]) == 0,
		"no sprite resource misses (unbound tag)")
	_check(int(d["sprite_miss_meta"]) == 0,
		"no sprite metadata misses (bad bound image)")
	_check(int(d["sprite_drawn"]) + int(d["sprite_misses"]) +
		int(d["sprite_zero_size"]) + int(d["sprite_clipped"]) +
		int(d["sprite_transparent"]) == int(d["sprites"]),
		"sprite outcome census covers every command")
	_check(int(d["hud_blits"]) > 0, "HUD blits")
	_check(int(d["palette_sets"]) > 0, "palette uploads")
	# Real streams post no TELETYPE text (OBSERVED) — the count is
	# reported, not asserted.
	# 19B.2B1 — every kModelDraw resolves to the object's live model
	# (or the classRec0 fuse arm); misses and malformed geometry are
	# hard failures, the A..G census must cover every pushed tri.
	_check(int(d["model_commands"]) > 0,
		"model draw commands received")
	_check(int(d["model_lookup_miss"]) == 0,
		"no model resource misses")
	_check(int(d["model_invalid_geometry"]) == 0,
		"no invalid model geometry")
	_check(int(d["model_resolved"]) + int(d["model_class_rec0"]) +
		int(d["model_lookup_miss"]) == int(d["model_commands"]),
		"model resolution census covers every command")
	var mc: Array = d["model_mat_census"]
	var mc_total := 0
	for i in range(7):
		mc_total += int(mc[i])
	_check(mc_total + int(d["model_polys_backface"]) ==
		int(d["model_tris_walked"]),
		"model A..G census covers every submitted tri")
	# 19B.2A — every kRibbonTri consumed is a command; the census must
	# show rasterized tris and no unported material branch / LUT miss.
	_check(int(d["ribbon_commands"]) > 0,
		"ribbon tri commands received")
	_check(int(d["ribbon_rasterized"]) > 0,
		"ribbon tris rasterized")
	_check(int(d["ribbon_pixels"]) > 0,
		"ribbon indexed pixels written")
	_check(int(d["ribbon_unsupported"]) == 0,
		"no unsupported ribbon material branches")
	_check(int(d["ribbon_lut_misses"]) == 0,
		"no ribbon LUT misses (base palette bound)")
	var fr: Dictionary = bridge.stream_frame()
	_check(not fr.is_empty(),
		"stream_frame returns the terminal image")
	if not fr.is_empty():
		_check(int(fr["w"]) == 600 and int(fr["h"]) == 360,
			"presented frame is 600x360")
		_check(PackedByteArray(fr["rgba"]).size() == 600 * 360 * 4,
			"rgba payload == 600*360*4")
	# Exit routes (OBSERVED dispatcher tail, checked in order):
	# health<=0 -> mode 0 frontend; levelId<4 -> mode 6 loader;
	# else levelId=5 + mode 7. On the canonical seed courses 0..3
	# exit alive (hero latch) while course 4 drains the counter to
	# the death latch -> health 0 -> mode 0.
	var want_mode := 0 if course >= 4 else 6
	_check(int(bridge.get_mode()) == want_mode,
		"exit handoff -> mode %d" % want_mode)
	# u64 digests print as two u32 halves — GDScript %x renders
	# negative int64s signed.
	var fbh := int(d["fb_hash"])
	var plh := int(d["palette_hash"])
	print(("  stream%d: frames=%d pres=%d bg=%d spr=%d drawn=%d " +
		"miss=%d(res=%d,meta=%d) zsize=%d clip=%d key=%d " +
		"hud=%d+%dmiss tt=%d pal=%d mdl=%d rib=%d snd=%d " +
		"fill=0x%02x fb=%08x%08x pal=%08x%08x mode=%d") % [
		course, frames,
		int(d["presented"]), int(d["backdrop_blits"]),
		int(d["sprites"]), int(d["sprite_drawn"]),
		int(d["sprite_misses"]), int(d["sprite_miss_res"]),
		int(d["sprite_miss_meta"]),
		int(d["sprite_zero_size"]), int(d["sprite_clipped"]),
		int(d["sprite_transparent"]),
		int(d["hud_blits"]), int(d["hud_misses"]),
		int(d["teletype_draws"]), int(d["palette_sets"]),
		int(d["model_commands"]), int(d["ribbon_commands"]),
		int(d["sound_events"]), int(d["terminal_fill"]),
		(fbh >> 32) & 0xffffffff, fbh & 0xffffffff,
		(plh >> 32) & 0xffffffff, plh & 0xffffffff,
		int(bridge.get_mode())])
	# 19B.2B1 — model submission census + the A..G material arms.
	var mdig := int(d["model_fb_digest"])
	print(("    model: cmd=%d res=%d miss=%d rec0=%d elem=%d/%d " +
		"tri=%d back=%d oob=%d flush=%d px=%d " +
		"A..G=[%d,%d,%d,%d,%d,%d,%d] fbdig=%08x%08x") % [
		int(d["model_commands"]), int(d["model_resolved"]),
		int(d["model_lookup_miss"]), int(d["model_class_rec0"]),
		int(d["model_elements_walked"]), int(d["model_elements_masked"]),
		int(d["model_tris_walked"]), int(d["model_polys_backface"]),
		int(d["model_invalid_geometry"]), int(d["model_flushes"]),
		int(d["model_pixels"]),
		int(mc[0]), int(mc[1]), int(mc[2]), int(mc[3]),
		int(mc[4]), int(mc[5]), int(mc[6]),
		(mdig >> 32) & 0xffffffff, mdig & 0xffffffff])
	for e in d.get("model_branch", []):
		print("      mbranch %s n=%d" % [
			String(e["branch"]), int(e["count"])])
	# 19B.2B2 — the material-path split: persp/affine drawers vs the
	# three flat-0xff fallback classes, plus texel/keyed counters.
	# texMiss/texMeta/invalidRec are defect classes — always zero;
	# indexRec/lookupMiss are reported (proven-native null-slot
	# arms may be nonzero — BONES WHITE precedent).
	_check(int(d["model_tex_lookup_miss"]) == 0,
		"no texel fetch misses")
	_check(int(d["model_tex_invalid_meta"]) == 0,
		"no invalid texture metadata")
	_check(int(d["model_mat_invalid_rec"]) == 0,
		"no invalid material records")
	print(("    matpath: persp=%d affine=%d indexRec=%d " +
		"lookupMiss=%d invalidRec=%d texMiss=%d texMeta=%d " +
		"clipFan=%d degen=%d zero=%d matPx=%d keyedSkip=%d " +
		"rast=%d") % [
		int(d["model_mat_persp"]), int(d["model_mat_affine"]),
		int(d["model_mat_index_rec"]),
		int(d["model_mat_lookup_miss"]),
		int(d["model_mat_invalid_rec"]),
		int(d["model_tex_lookup_miss"]),
		int(d["model_tex_invalid_meta"]),
		int(d["model_mat_clip_fan"]),
		int(d["model_mat_degenerate"]), int(d["model_mat_zero"]),
		int(d["model_mat_pixels"]),
		int(d["model_tex_transparent"]),
		int(d["model_mat_rasterized"])])
	# 19B.2A ribbon raster census + material branch breakdown.
	print(("    ribbon: cmd=%d rast=%d zero=%d clip=%d drop=%d " +
		"unsup=%d lutmiss=%d px=%d") % [
		int(d["ribbon_commands"]), int(d["ribbon_rasterized"]),
		int(d["ribbon_zero_pixels"]), int(d["ribbon_clipped"]),
		int(d["ribbon_clip_dropped"]), int(d["ribbon_unsupported"]),
		int(d["ribbon_lut_misses"]), int(d["ribbon_pixels"])])
	for e in d.get("ribbon_branch", []):
		print("      branch %s n=%d" % [
			String(e["branch"]), int(e["count"])])
	# The deterministic miss census — every non-drawn sprite outcome,
	# bucketed by (class, tag, src dims); names join from the bind.
	for e in d.get("sprite_census", []):
		print(("    sprmiss %s tag=%d(%s) src=%dx%d " +
			"size=[%d..%d] n=%d") % [
			e["cls"], int(e["tag"]), String(e["name"]),
			int(e["src_w"]), int(e["src_h"]),
			int(e["size_min"]), int(e["size_max"]), int(e["count"])])
	# 19B.3B1 — the mode-5 audio census: the tail already drained on
	# the exit-step pass; this second drain only runs the
	# non-owning-mode sweep that frees the stopped player nodes.
	_drain_audio_fx()
	_stream_audio_census(str(course), d)
	_check(int(d["snd_loop_plays"]) == 1,
		"WIND loop play emitted exactly once")
	_check(int(d["snd_stops"]) == 1,
		"WIND stop emitted exactly once")
	_check(int(audio_stats["starts_by_name"].get("WIND", 0)) == 1,
		"WIND player started exactly once")
	_check(int(audio_stats["stops_by_name"].get("WIND", 0)) == 1,
		"WIND player stopped exactly once")
	_check(_audio_live_count() == 0,
		"no live players after teardown")
	if not shot_dir.is_empty():
		_stream_shot_dump(shot_dir, course, st_ckpt, st_last,
			st_last_seq, int(d["presented"]))
	print("smoke(stream): %d failure(s)" % failures)


func _run_smoke_campaign(course: int) -> void:
	# Phase 19B.3A — the real campaign handoff route for one course:
	# load_level (the traversal the campaign lands on) -> the
	# diagnostic END_LEVEL mailbox (the same store the script op
	# writes) -> the mode-3 dispatcher tail (FUN_004371bc teardown +
	# FUN_0042b270 entry) -> a natural StreamScene exit -> the
	# dispatcher exit routes -> the mode-6 loader pump -> the next
	# runtime. Layer ownership is checked at every edge, and the
	# mode-5 material-bank audit is the BONES.WHITE stale-bank
	# oracle (courses 2/3 carry the named slot).
	print("smoke(campaign): course=%d" % course)
	printerr("campaign%d: enter" % course)
	# levelId -> TRAVERSE dir — the OBSERVED 0x4999e8 table
	# ({7,6,3,4,8,5,2,1}; the loader advances levelId past the
	# played course on the mode-6 exit).
	var dirs := [7, 6, 3, 4, 8]
	var dir: int = dirs[course]
	var dti := "TRAVERSE/LEVEL%d/LEVEL%d.DTI" % [dir, dir]
	_check(bridge.load_level(dti), "load_level %s" % dti)
	if failures > 0:
		return
	_check(int(bridge.get_mode()) == 3, "mode == 3 (traversal)")
	var el: Dictionary = bridge.diagnostic_end_level()
	_check(bool(el.get("ok", false)), "END_LEVEL mailbox armed")
	printerr("campaign%d: armed, stepping mode 3 -> 5" % course)

	# The mode-3 dispatcher tail: the FUN_0040dde0 arm, the takeoff
	# sequence, then the >300 white-out latches 0x49a030 and the
	# tail consumes it — teardown + mode-5 entry in one step.
	var res := {}
	var frames := 0
	while frames < 800:
		res = bridge.step_frame_input(33.333, {"actions": 0})
		frames += 1
		if int(bridge.get_mode()) != 3:
			break
	printerr("campaign%d: mode 3 -> %d after %d frames" %
		[course, int(bridge.get_mode()), frames])
	_check(int(bridge.get_mode()) == 5,
		"mode-3 dispatcher tail -> mode 5")
	if int(bridge.get_mode()) != 5:
		printerr("campaign: never reached mode 5 (frames=%d mode=%d)" %
			[frames, int(bridge.get_mode())])
		_check(false, "mode-3 dispatcher tail -> mode 5")
		return
	_check(bridge.stream_active(), "stream_active after handoff")
	_apply_stream()
	_check($StreamLayer.visible, "StreamLayer visible in mode 5")
	_check(not $FrontendLayer/FrontendRect.visible,
		"frontend hidden under mode 5")

	# The mode-5 entry audit — the BONES.WHITE stale-bank oracle.
	# bank_b_bound=false means the traversal arena's .MAT table did
	# NOT survive the FUN_004371bc -> FUN_0042b270 edge (the oracle's
	# outcome-A arm: no stale bank B exists to resolve WHITE).
	var d0: Dictionary = bridge.stream_diag()
	_check(int(d0["route_from"]) == 3 and int(d0["route_to"]) == 5,
		"route edge 3->5 recorded")
	_check(int(d0["mode5_enters"]) == 1, "mode-5 entered once")
	_check(int(d0["bank_a_records"]) > 0,
		"bank A bound (STREAM.MTI)")
	_check(not bool(d0["bank_b_bound"]),
		"no prior-mode bank survives mode-5 entry")
	if course == 2 or course == 3:
		_check(int(d0["white_slot"]) >= 0,
			"BONES name table carries WHITE")
		_check(not bool(d0["white_resolved"]),
			"BONES.WHITE unresolved -> flat 0xff")
		print("  oracle: white_slot=%d resolved=%s bankB=%s bankA=%d" %
			[int(d0["white_slot"]), bool(d0["white_resolved"]),
			bool(d0["bank_b_bound"]), int(d0["bank_a_records"])])

	# Drive the StreamScene to its natural exit — the kExitMode
	# consume inside stepCore_ runs the dispatcher tail (teardown +
	# the tally-done progression step).
	var exited := false
	frames = 0
	while frames < 2200:
		res = bridge.step_frame_input(33.333, {"actions": 0})
		frames += 1
		# 19B.3B1 — the voice commands drain once per step; the
		# exit-step pass also delivers the teardown tail's stops.
		_drain_audio_fx()
		if not bool(res.get("ok", true)):
			_check(false, "stream step failed: %s" %
				bridge.get_last_error())
			return
		if bool(res.get("exited", false)):
			exited = true
			break
	printerr("campaign%d: stream exit after %d -> mode %d" %
		[course, frames, int(bridge.get_mode())])
	_check(exited, "stream reaches natural exit")
	if not exited:
		return
	_apply_stream()   # the terminal fill frame presents once
	var d: Dictionary = bridge.stream_diag()
	_check(int(d["stream_teardowns"]) == 1,
		"exactly one stream teardown")
	_check(int(d["stream_exit_frame"]) > 0,
		"terminal presented frame recorded")
	_check(not bridge.stream_active(),
		"stream inactive after teardown")
	# 19B.3B1 — the audio census for this entry: the tail drained on
	# the exit-step pass, so the WIND stop already landed; the extra
	# drain runs the non-owning-mode node sweep.
	_drain_audio_fx()
	_stream_audio_census(str(course), d)
	_check(int(d["snd_loop_plays"]) == 1,
		"WIND loop play emitted exactly once")
	_check(int(d["snd_stops"]) == 1,
		"WIND stop emitted exactly once")
	_check(int(audio_stats["starts_by_name"].get("WIND", 0)) == 1,
		"WIND player started exactly once")
	_check(int(audio_stats["stops_by_name"].get("WIND", 0)) == 1,
		"WIND player stopped exactly once")
	_check(_audio_live_count() == 0,
		"no live players after teardown")

	if course >= 4:
		# The final course's exit is health-gated by the CARRIED
		# session state: the standalone --stream convention seeds a
		# fresh 100/rng and drains to the death latch -> mode 0, but
		# the real campaign handoff carries the traversal runtime's
		# globals (health 150, the shared rand stream) — the hero
		# latch wins and the dispatcher writes 541498 = 5 -> mode 7
		# (the OBSERVED 0x4015ef store). The death->0 arm is covered
		# by the standalone --stream 4 smoke and the core suite.
		_check(int(bridge.get_mode()) == 7,
			"course-4 carried-health exit -> mode 7")
		var pumps7 := 0
		var pr7 := {}
		while pumps7 < 8 and int(bridge.get_mode()) == 7:
			pr7 = bridge.frontend_progression_step(
				{"stage_done": true})
			pumps7 += 1
			if not bool(pr7.get("ok", true)):
				_check(false, "mode-7 pump failed: %s" %
					bridge.get_last_error())
				return
		_check(int(bridge.get_mode()) == 3,
			"mode 7 -> mode 3 (LEVEL5 continuation)")
		_check(int(pr7.get("level_id", -1)) == 5,
			"mode-7 levelId == 5")
		var d4: Dictionary = bridge.stream_diag()
		_check(int(d4["route_from"]) == 7 and
			int(d4["route_to"]) == 3,
			"route edge 7->3 recorded")
		# Layer ownership on the traversal re-entry: the stream
		# layer must be down and the gameplay set live. One step
		# settles the fresh runtime's view set before the apply.
		if $StreamLayer.visible:
			$StreamLayer.visible = false
		_step_n({}, 1)
		_check(not $StreamLayer.visible,
			"StreamLayer hidden on mode-7 re-entry")
		_check(not bridge.get_player_snapshot().is_empty(),
			"traversal snapshot live on mode-7 re-entry")
	else:
		# Courses 0-3 exit alive -> mode 6 (the loader). The pump
		# runs the sub-state machine (2 -> 4 -> 1 -> 3 -> exit); the
		# exit installs the FALL3D_<levelId+1> freefall entry.
		_check(int(d["stream_exit_reason"]) == 2,
			"exit reason == hero latch")
		_check(int(d["stream_exit_health"]) > 0,
			"health carried positive")
		_check(int(bridge.get_mode()) == 6, "exit -> mode 6")
		_check(int(d["mode6_enters"]) == 1, "mode-6 entered once")
		var pumps := 0
		var pr := {}
		# The real briefing machine owns mode 6 now (19E): stage_done
		# rides its esc/skip arm — fade-in ~15f + page fill + exit
		# key + fade-out ~15f ≈ 31 pumps, not the placeholder's 1.
		while pumps < 60 and int(bridge.get_mode()) == 6:
			pr = bridge.frontend_progression_step(
				{"stage_done": true})
			pumps += 1
			if not bool(pr.get("ok", true)):
				_check(false, "mode-6 pump failed: %s" %
					bridge.get_last_error())
				return
		printerr("campaign%d: mode 6 -> %d in %d pumps" %
			[course, int(bridge.get_mode()), pumps])
		_check(int(bridge.get_mode()) == 2,
			"loader exit -> mode 2 (freefall)")
		_check(int(pr.get("level_id", -1)) == course + 1,
			"loader advanced levelId -> %d" % (course + 1))
		var d2: Dictionary = bridge.stream_diag()
		_check(int(d2["mode6_exits"]) == 1, "mode-6 exited once")
		_check(int(d2["route_from"]) == 6 and
			int(d2["route_to"]) == 2,
			"route edge 6->2 recorded")
		# Layer handoff: the freefall presenter owns the screen; the
		# terminal stream frame must not overlay it.
		_apply_freefall()
		_check($FreefallRoot.visible,
			"FreefallRoot visible in mode 2")
		_check(not $StreamLayer.visible,
			"StreamLayer hidden exactly once after exit")
		_check(not $FrontendLayer/FrontendRect.visible,
			"frontend hidden under mode 2")

		# Repeat entry/exit — continue the same campaign session:
		# the freefall handoff lands the next traversal, the
		# END_LEVEL mailbox arms again, and the second mode-5 entry
		# must rebuild with no stale state (counters accumulate).
		printerr("campaign%d: chaining freefall -> traversal" % course)
		var chained := _campaign_chain_once(course + 1)
		printerr("campaign%d: chain result %s" %
			[course, str(chained)])
		if chained:
			var d3: Dictionary = bridge.stream_diag()
			_check(int(d3["mode5_enters"]) == 2,
				"second mode-5 entry in-session")
			_check(int(d3["stream_teardowns"]) == 2,
				"second teardown counted")
			_check(int(d3["mode6_enters"]) == 2,
				"second mode-6 entry in-session")
			_check(not bool(d3["bank_b_bound"]),
				"no stale bank on re-entry")
			# 19B.3B1 — repeat entry: a fresh WIND loop starts on the
			# second entry's own census (the registration table and
			# counters reset per entry; the player-side count is
			# cumulative across both entries).
			_drain_audio_fx()
			_check(int(d3["snd_loop_plays"]) == 1,
				"re-entry WIND loop play emitted once")
			_check(int(d3["snd_stops"]) == 1,
				"re-entry WIND stop emitted once")
			_check(int(audio_stats["starts_by_name"].get(
				"WIND", 0)) == 2,
				"WIND player started once per entry")
			_check(int(audio_stats["stops_by_name"].get(
				"WIND", 0)) == 2,
				"WIND player stopped once per entry")
			_check(_audio_live_count() == 0,
				"no live players after re-entry teardown")
		else:
			print("  chain: freefall ended before traversal " +
				"(repeat-entry skipped — death or course bound)")

	var de: Dictionary = bridge.stream_diag()
	print("  campaign%d: frames=%d mode=%d teardowns=%d " %
		[course, frames, int(bridge.get_mode()),
		int(de["stream_teardowns"])])
	print("smoke(campaign): %d failure(s)" % failures)


func _campaign_chain_once(next_course: int) -> bool:
	# Continue the live campaign session one more hop: freefall ->
	# traversal -> END_LEVEL -> mode 5 -> natural exit -> mode 6.
	# Returns true when the second mode-5 entry+exit completed.
	# next_course >= 4 death-routes out of the chain (mode 0), so
	# only chains landing back on the loader count here.
	if int(bridge.get_mode()) != 2 or next_course > 3:
		return false
	var res := {}
	for i in 1600:
		res = _step_n({}, 1)
		if bool(res.get("done", false)):
			break
	if int(bridge.get_mode()) != 3:
		return false   # the freefall death route ends the campaign
	var el: Dictionary = bridge.diagnostic_end_level()
	if not bool(el.get("ok", false)):
		return false
	for i in 800:
		_step_n({}, 1)
		if int(bridge.get_mode()) != 3:
			break
	if int(bridge.get_mode()) != 5 or not bridge.stream_active():
		return false
	for i in 2200:
		res = bridge.step_frame_input(33.333, {"actions": 0})
		# 19B.3B1 — same per-step voice drain as the first entry.
		_drain_audio_fx()
		if bool(res.get("exited", false)):
			break
	return int(bridge.get_mode()) == 6 or int(bridge.get_mode()) == 7


func _stream_shot_dump(shot_dir: String, course: int, ckpt: Dictionary,
		last: Dictionary, last_seq: int, pres_total: int) -> void:
	# Pick early / mid / near-exit frames from the checkpoint ring and
	# save them under the ignored diagnostic dir. Mid = the stored
	# checkpoint nearest the half-presented seq. PNGs carry decoded
	# proprietary art — out/ is .gitignore'd, nothing is committed.
	DirAccess.make_dir_recursive_absolute(shot_dir)
	var mid_target := int(pres_total / 2)
	var mid_seq := -1
	for sq in ckpt.keys():
		if mid_seq < 0 or absi(sq - mid_target) < absi(mid_seq - mid_target):
			mid_seq = sq
	var early_seq := -1
	for sq in ckpt.keys():
		if sq >= 60 and (early_seq < 0 or sq < early_seq):
			early_seq = sq
	var picks := {"early": early_seq, "mid": mid_seq, "exit": last_seq}
	for label in ["early", "mid", "exit"]:
		var sq: int = picks[label]
		if sq < 0:
			continue
		var e: Dictionary = last if sq == last_seq else ckpt[sq]
		var img := Image.create_from_data(600, 360, false,
			Image.FORMAT_RGBA8, e["rgba"])
		var path := shot_dir.path_join(
			"stream%d_%s_f%04d.png" % [course, label, sq])
		var err := img.save_png(path)
		print(("    shot stream%d %s seq=%d -> %s " +
			"fb=%08x%08x pal=%08x%08x spr=%d drawn=%d " +
			"rc=%d") % [
			course, label, sq, path,
			(int(e["fb"]) >> 32) & 0xffffffff, int(e["fb"]) & 0xffffffff,
			(int(e["pal"]) >> 32) & 0xffffffff, int(e["pal"]) & 0xffffffff,
			int(e["spr"]), int(e["drawn"]), err])


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
		if fe_active:
			# The frontend owns every key while it presents — Esc is
			# the shell's cancel, not the app's quit shortcut.
			_fe_key_edge(event)
			return
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
	# --ff-dump-dir DIR + --ff-dump-every N: consecutive-frame game-
	# output capture (viewport texture only — no OS window pixels).
	if not ff_dump_dir.is_empty():
		if _ff_dump_count % ff_dump_every == 0:
			var vt := get_viewport().get_texture()
			if vt != null:
				var img := vt.get_image()
				if img != null and not img.is_empty():
					img.save_png(ff_dump_dir +
						"/f%06d.png" % _ff_dump_count)
		_ff_dump_count += 1
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
				" size=", img.get_width(), "x", img.get_height(),
				" ending=", bridge.ending_diag())
			get_tree().quit(0 if err == OK else 1)
			return
	if frames_left > 0:
		frames_left -= 1
		if frames_left == 0:
			# 19C.1 — the startup-proof line carries the mode/stream
			# state so a bounded wall-clock run proves the paced
			# step cadence (sim steps = presented stream frames).
			var fm := int(bridge.get_mode())
			# stream_diag is a persistent census — safe to read post-exit.
			var fpres := int(bridge.stream_diag().get("presented", 0))
			print("frames: startup proof complete",
				" mode=", fm, " stream_presented=", fpres,
				" ending=", bridge.ending_diag())
			get_tree().quit(0)
			return
	var mode := int(bridge.get_mode())
	# Standalone mode-5 flag — under the frontend route the scene is
	# stepped inside _frontend_frame instead (one step per frame).
	var stream_standalone := mode == 5 and not fe_active
	if fe_active and (mode == 0 or (mode >= 5 and mode <= 8)):
		# The frontend route owns the frame: mode 0 runs the shell
		# loop, modes 5-8 run the progression pump until a presented
		# runtime lands. No gameplay presenter runs underneath.
		_frontend_frame(delta, mode)
		mode = int(bridge.get_mode())
		if mode == 0 or (mode >= 5 and mode <= 8):
			return
		_frontend_hide()   # a runtime mode just landed
	elif fe_active:
		# Frontend booted but a gameplay mode is presenting.
		if $FrontendLayer/FrontendRect.visible:
			_frontend_hide()
		else:
			pass
	if (shot_path.is_empty() or combat_demo or chute_demo \
			or step_shot) and (mode == 2 or mode == 3):
		# Screenshot mode keeps the exact frame-0 spawn pose — UNLESS
		# a scripted-input demo flag is set (combat/chute demos step
		# the runtime deliberately; plain --screenshot still freezes).
		#
		# Keyboard goes through "keys" — held physical codes in the
		# original's internal domain. buildRawInput_ turns them into
		# keyLevel + keyEdge, then the configured binding table
		# (KeyJump/KeySniper/KeyFire/items/zoom/...) resolves the
		# semantics — the same path the keyboard menu rebinds. The
		# QA "actions" mask is left empty here: its hard-coded
		# WASD/Space aliases conflicted with the real bindings (Space
		# is KeySniper in the defaults, not jump).
		var input := {
			"keys": _gameplay_keys(),
			"mouse_dx": mouse_dx,
			"mouse_dy": mouse_dy,
			"mouse_dz": mouse_dz,
			"mouse_buttons": _mouse_button_bits(),
		}
		mouse_dx = 0
		mouse_dy = 0
		mouse_dz = 0
		if ff_steer and mode == 2:
			# --ff-steer: corkscrew weave through REAL key events —
			# parse_input_event sets the polled device state, so the
			# production _ff_input() -> direction booleans -> core
			# fold path carries the steering (not a dict shortcut).
			demo_frame += 1
			_ff_steer_inject(demo_frame)
		input.merge(_ff_input(), true)
		if ff_trace:
			var _k: PackedInt32Array = input["keys"]
			if not _k.is_empty():
				print("ff-trace keys=%s ff=%s" % [_k, input])
		if combat_demo and mode == 3:
			# MMB edge at frame 4 scopes in; LMB holds from frame 10
			# — the cadence decay runs while scoped, then the shot
			# fires and the bullet-cam window/impact path render.
			demo_frame += 1
			if demo_frame == 4:
				input["mouse_buttons"] = 4
			elif demo_frame >= 10:
				input["mouse_buttons"] = 1
		elif chute_demo and mode == 3:
			# LAlt (internal 0x38) press at frame 15 — after the spawn
			# settle, so the jump edge lands on a grounded frame —
			# held through the descent: airCharge seeds on the way
			# down, the sustain event selects K_CHUTE then K_CHUTEC —
			# the original's chute contract on the real input path.
			demo_frame += 1
			if demo_frame >= 15:
				var k: PackedInt32Array = input["keys"]
				if not k.has(0x38):
					k.append(0x38)
				input["keys"] = k
		# Paced sim stepping: one ~33.3ms step per completed slice of
		# accumulated wall time (see game_pace_* above — the original
		# contract is one step per ~30Hz dispatcher tick, NOT one per
		# rendered frame). Input state is sampled once per render
		# frame and held across the substeps — the same relationship
		# a period 30Hz loop had to its input merge.
		game_pace_ms += delta * 1000.0
		if not game_pace_armed:
			# First gameplay frame — present it immediately rather
			# than dead-waiting the slice that just landed.
			game_pace_armed = true
			game_pace_ms += STREAM_STEP_MS
		var n := int(game_pace_ms / STREAM_STEP_MS)
		if n > STREAM_MAX_CATCHUP:
			n = STREAM_MAX_CATCHUP
			game_pace_ms = 0.0   # drop the backlog — no death spiral
		else:
			game_pace_ms -= n * STREAM_STEP_MS
		var first_step := true
		while n > 0:
			var step_mode := int(bridge.get_mode())
			if step_mode != 2 and step_mode != 3:
				break   # mode flipped mid-frame — other presenters own it
			bridge.step_frame_input(STREAM_STEP_MS, input)
			n -= 1
			if first_step:
				# Device deltas are per-sample-period — applying them
				# to every substep would double-count aim. Held keys
				# and buttons are level state and correctly persist.
				input["mouse_dx"] = 0.0
				input["mouse_dy"] = 0.0
				input["mouse_dz"] = 0.0
				first_step = false
			if step_mode == 2:
				if _ff_steps == 0:
					_ff_wall0 = Time.get_ticks_msec()
				_ff_steps += 1
				if ff_trace:
					_ff_trace_tick()
		# The freefall->traversal handoff can flip the mode inside the
		# step — re-read so the apply path follows the live runtime.
		# (Traversal sessions are always mode != 2; only a session
		# that STARTED in mode 2 logs the transition.)
		mode = int(bridge.get_mode())
		if mode != 2 and mode != 3:
			# Non-gameplay mode took over (death->0, handoff->5..8);
			# a later gameplay load re-arms with a present-first tick.
			game_pace_armed = false
		if (freefall or fe_run or frontend) and mode != 2 and \
				not ff_handoff_seen:
			var fs: Dictionary = bridge.get_freefall_snapshot()
			# Live runs gate on _ff_steps; the smoke route steps
			# through _step_n instead — its done/dead phase is the
			# equivalent proof the mode-2 frame ran to the edge.
			if _ff_steps > 0 or int(fs.get("phase", -1)) >= 2:
				ff_handoff_seen = true
				print("mdk-godot: freefall handoff -> mode %d " %
					mode +
					"steps=%d wall=%dms phase=%s hp=%s died=%s" %
					[_ff_steps,
					 Time.get_ticks_msec() - _ff_wall0,
					 str(fs.get("phase", -1)),
					 str(fs.get("health", -1)),
					 str(fs.get("died", "?"))])
	elif shot_path.is_empty() and stream_standalone:
		# Standalone mode 5: paced ~33.3ms steps (19C.1), not one per
		# display refresh. The scene takes actions only (the roll/
		# pitch steer axes); mouse deltas are unbound here.
		if _stream_pace_run(delta * 1000.0) > 0:
			var m2 := int(bridge.get_mode())
			if m2 != mode and pace_trace:
				print("pace: handoff mode %d -> %d at steps=%d wall=%dms" %
					[mode, m2, pace_steps,
					Time.get_ticks_msec() - max(pace_wall0, 0)])
			mode = m2
	if mode == 2:
		# Mode-2 freefall — the FUN_004109d8 model walk + the
		# FUN_004123f4 fixed-orientation camera + 0x4edc04 fade.
		_fe_stop_songs()   # 19C.4 — the mode-2 bank load releases them
		_apply_freefall()
		# FUN_0041cb44 — the FALL_T1 teletype strip (course 0).
		_apply_ff_teletype()
		# 19C.3 — the mode-2 sound commands drain on the same
		# once-per-rendered-frame cadence as traversal/mode-5.
		_drain_audio_fx()
		return
	if ff_backdrop != null and ff_backdrop.visible:
		# The mode-2 backdrop quad is a camera child — gate it off
		# the moment any other mode's presenter takes over.
		ff_backdrop.visible = false
	_tt_hide()   # the FALL_T1 strip dies with the freefall mode
	if mode == 5:
		# Standalone StreamScene — the step above may have run the
		# exit handoff; the terminal fill frame still uploads. Under
		# the frontend route _frontend_frame already presented.
		if stream_standalone:
			_apply_stream()
		# 19B.3B1 — the mode-5 voice commands drain on the same
		# once-per-rendered-frame cadence as traversal.
		_drain_audio_fx()
		return
	if mode == 8:
		# Standalone mode 8 — the ending cinematic: paced FLIC pump,
		# FINISH.BNI mark sounds, the white ramp; the MVE stage then
		# runs to natural EOF/abort and exits to the frontend.
		_fe_stop_songs()   # the traversal bank release is silent
		if _ending_pace_run(delta * 1000.0) > 0:
			_apply_ending()
		_ending_drain_audio()
		if int(bridge.get_mode()) != 8:
			_ending_hide()
			# --ending-repeat (QA): re-enter the whole cinematic
			# once — exercises teardown/re-entry hygiene.
			if end_repeat and not end_repeat_done:
				end_repeat_done = true
				if not bridge.load_ending():
					printerr("repeat load_ending: ",
						bridge.get_last_error())
		return
	if stream_standalone:
		# The mode-5 exit flipped the mode inside the step — upload
		# the terminal-fill frame once, then idle (modes 6/7/8 have
		# no standalone presenter; the frontend route owns them when
		# a campaign drives the progression). The teardown tail's
		# queued stops still need the drain — otherwise the WIND
		# loop's player would keep sounding through the next mode.
		_apply_stream()
		_drain_audio_fx()
	if mode == 0:
		# Frontend route (post-death handoff or unload) — the mode-0
		# shell is a documented seam; freeze the last frame.
		return
	if mode != 3:
		# Post-intermission progression modes (6/7/8) present nothing
		# on the standalone route.
		return
	# Mode 3 (traversal — reached directly or via the handoff).
	_fe_stop_songs()   # 19C.4 — the traversal bank load's release
	if $StreamLayer.visible:
		$StreamLayer.visible = false
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
	_drain_audio_fx()
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
		# The audio event batch is drain-once — smoke paths never run
		# _process, so the drain rides the shared step helper to keep
		# rt.audioFx bounded and exercise the presenter.
		_drain_audio_fx()
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
	_reset_audio()
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
	_reset_audio()
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
			# vert_count is the EXPANDED per-tri output (tris*3 —
			# shared verts duplicate across material groups);
			# tri_count still equals the snapshot's model count.
			_check(int(g["tri_count"]) == int(o["tri_count"]) and
				int(g["vert_count"]) == int(o["tri_count"]) * 3,
				"geometry counts == snapshot counts")
			_check(int(g["geom_key"]) == int(o["geom_key"]),
				"geom_key == snapshot key")
			var gm: ArrayMesh = g["mesh"]
			_check(gm != null and gm.get_surface_count() ==
				PackedInt32Array(g["surface_elems"]).size(),
				"mesh surfaces == surface_elems")
			print("first_model=%s elems=%d verts=%d tris=%d (src %d/%d)" % [
				String(g["model"]), int(g["elem_count"]),
				int(g["vert_count"]), int(g["tri_count"]),
				int(o["vert_count"]), int(o["tri_count"])])
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
	# CHMO_2 has no MTO render block — its connector render geometry
	# comes from the O.SNI region-C fallback (726ed3e), so the corridor
	# itself is the displayed arena while its script-spawned XCORDOOR
	# door presents from the corridor's own object list. Walking +y
	# crosses the proven y=1237 portal into HMO_3 (arena 11 -> 2).
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
			# The corridor now renders its own O.SNI geometry
			# (726ed3e): when it is current it is the primary
			# displayed arena — the old geometry-less-connector
			# assumption (partner carried the view) no longer holds.
			_check(int(dspc["primary"]) == 11,
				"corridor cur -> corridor geometry displayed")
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
		# active partner. A corridor door that self-migrated to follow
		# the player is legitimately re-enumerated on its destination —
		# the SAME object transfers between arena lists (FUN_004574d0:
		# unlink+relink same record, +0x302 dest, connDest flips back to
		# the old home), so its id is stable and it stays a connector.
		# The real alias to reject is a seen door id worn by a
		# DIFFERENT object — which only shows as a non-connector.
		for od in bridge.get_object_snapshots():
			_check(not (door_ids_seen.has(int(od["id"])) and
				int(od["arena"]) != 11 and not bool(od["connector"])),
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

	# ---- Phase 17C.2: traversal audio playback ---------------------
	# Everything above stepped through _step_n, which drains the core
	# event batch into the voice pool + presenter. The landed jump,
	# scope enter/exit, scoped fire, and shockwave detonation all
	# emitted real events; verify they became audible playbacks —
	# BEFORE the level reload below clears the counters.
	var ast: Dictionary = bridge.get_audio_stats()
	print("smoke(audio): banks=%d resolved=%d missing=%d cache=%d " %
		[int(ast["banks"]), int(ast["resolved"]),
		int(ast["missing"]), int(ast["cache"])] +
		"active=%d exhausted=%d starts=%d params=%d stops=%d" %
		[int(ast["active"]), int(ast["pool_exhausted"]),
		int(audio_stats["starts"]), int(audio_stats["params"]),
		int(audio_stats["stops"])])
	print("smoke(audio): names=%s" % str(audio_stats["names"]))
	# Bank set: LEVEL<n>S + LEVEL<n>O + TRAVERSE + MDKSOUND — the
	# O bank joined with the zone-ambient port (flags&3 records).
	_check(int(ast["banks"]) == 4,
		"audio: level+traverse+global banks bound")
	_check(int(ast["resolved"]) > 0, "audio: records resolved+decoded")
	_check(int(audio_stats["starts"]) > 0,
		"audio: one-shot events became playback")
	# The scope-enter path's restart (SNIPERON) and the scoped fire
	# one-shot (SNIPERSHOT) are both guaranteed by the checks above.
	_check(audio_stats["names"].has("SNIPERON"),
		"audio: scope-enter SNIPERON emitted")
	_check(audio_stats["names"].has("SNIPERSHOT"),
		"audio: scoped fire SNIPERSHOT emitted")
	_check(int(ast["pool_exhausted"]) == 0,
		"audio: 63-pool never exhausted on this path")
	_check(int(ast["active"]) <= 63, "audio: pool bound respected")
	# Zone ambience — the arena CMI pair armed its flags&3 looped
	# record on this route (HMO_1 -> "H1", HMO_3 -> "H3" in the
	# LEVEL3 bank set) and the fader pushed per-frame volumes.
	_check(audio_stats["names"].has("H1") or
		audio_stats["names"].has("H3"),
		"audio: zone-ambient record armed")
	# Players actually instantiated under AudioRoot (bounded).
	var live_players := 0
	for p in audio_players:
		if p != null:
			live_players += 1
	_check(live_players > 0 and live_players <= AUDIO_VOICES,
		"audio: presenter players bounded (%d)" % live_players)

	# ---- Phase 14B: traversal death -> LASTGAME -> frontend ----
	# The 0x540dac fade (lrint(dt*100) per stepped frame) crosses
	# 255 after ~86 frames; the checks above ran ~80, so a bounded
	# wait drains the tail through the real route: LASTGAME.SAV
	# header-only commit -> teardown -> mode 0.
	var lastgame_path := smoke_save_dir.path_join("LASTGAME.SAV")
	var routed := false
	for i in 60:
		if int(bridge.get_mode()) == 0:
			routed = true
			break
		_step_n({}, 1)
	_check(routed, "death route: fade released to mode 0")
	_check(FileAccess.file_exists(lastgame_path),
		"death route: LASTGAME.SAV committed")
	if bridge.frontend_booted():
		# The returning frontend entry gates Continue on the file.
		# Its entry transition (TransitionArmed fx) arms on the first
		# drained frame and eats one edge as a skip-ack — drain it
		# before nav/confirm reach the shell.
		_fe_smoke_step({}, 1)
		# INTRO1A runs a 300-tick (10s) timeline — ~300 steps at 33ms.
		var tguard := 0
		while fe_transition_ms >= 0.0 and tguard < 320:
			_fe_smoke_step({}, 1)
			tguard += 1
		var fs0 := _fe_smoke_step({}, 2)
		_check(int(fs0.get("saves_exist", -1)) == 1,
			"death route: Continue enabled by LASTGAME")
		# Land on selection 0 (Continue) before confirming.
		for i in 8:
			if int(_fe_smoke_step({}, 1).get("selection", -1)) == 0:
				break
			if int(_fe_press("prev").get("selection", -1)) == 0:
				break
		_fe_press("confirm")   # selection 0 == Continue
		_check(int(bridge.get_mode()) == 3,
			"death route: Continue -> fresh traversal (mode 3)")
		_step_n({}, 1)   # first live frame binds the HUD
		var hps: Dictionary = bridge.get_hud_snapshot()
		_check(int(hps.get("health", -1)) == 100,
			"death route: Continue restores health 100")
		# Reentrant route: a second death re-arms the fade, rewrites
		# the checkpoint, and lands the clean-quit delete (0x401174).
		# Only meaningful if the continue landed a live runtime.
		if int(bridge.get_mode()) == 3:
			_step_n({}, 4)
			bridge.diagnostic_damage(500)
		var routed2 := false
		for i in 140:
			if int(bridge.get_mode()) == 0:
				routed2 = true
				break
			_step_n({}, 1)
		_check(routed2, "death route: second fade -> mode 0")
		_check(FileAccess.file_exists(lastgame_path),
			"death route: LASTGAME.SAV re-committed")
		# The second mode-0 entry re-arms the INTRO1A window — drain
		# it before navigating.
		_fe_smoke_step({}, 1)
		var tguard2 := 0
		while fe_transition_ms >= 0.0 and tguard2 < 320:
			_fe_smoke_step({}, 1)
			tguard2 += 1
		# Root -> Quit (bottom of the saves-present menu): the
		# dispatch's quit edge deletes the checkpoint.
		var qsel := -1
		for i in 8:
			var qsnap := _fe_smoke_step({}, 1)
			qsel = int(qsnap.get("selection", -1))
			if qsel == 4:
				break
			qsnap = _fe_press("next")
			qsel = int(qsnap.get("selection", -1))
			if qsel == 4:
				break
		_check(qsel == 4, "death route: Quit item reachable")
		_fe_press("confirm")
		_check(not FileAccess.file_exists(lastgame_path),
			"death route: clean Quit deletes LASTGAME.SAV")

	# ---- Phase 17B.2: level transition ----
	# shutdown() drops the HUD/bezel cache; a reload must rebuild
	# every surface from the fresh runtime — nothing stale survives.
	# The runtime may be torn down if the death-route QA ran; the
	# reload re-enters traversal regardless.
	var hpre: Dictionary = bridge.get_hud_snapshot()
	var tex_pre = hpre.get("tex")
	_check(bridge.load_level("TRAVERSE/LEVEL3/LEVEL3.DTI"),
		"transition: LEVEL3 reload")
	_reset_audio()
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
	# Audio seam — the reload reset the bridge-side pool: no voice
	# from the OLD session survives. The fresh runtime's zone-
	# ambient pair legitimately re-arms inside the first steps (the
	# original's resolver re-establishes it on the new current
	# arena) — anything beyond the two zone slots would be stale.
	var ast2: Dictionary = bridge.get_audio_stats()
	_check(int(ast2["active"]) <= 2,
		"audio: pool cleared across level reload")
	live_players = 0
	for p in audio_players:
		if p != null:
			live_players += 1
	_check(live_players <= 2,
		"audio: presenter players cleared across reload")

	print("smoke: %d failure(s)" % failures)


func _smoke_inject_key(physical: int, pressed: bool) -> void:
	# Real InputEventKey through Input.parse_input_event — the same
	# event the OS delivers for a physical key. Both keycode and
	# physical_keycode are set (hardware events carry both): the
	# keycode side feeds _ff_input's is_key_pressed aliases, the
	# physical side feeds _gameplay_keys. flush_buffered_events
	# makes the held state visible to the polled Input API inside
	# this synchronous smoke.
	var ev := InputEventKey.new()
	ev.physical_keycode = physical
	ev.keycode = physical
	ev.pressed = pressed
	Input.parse_input_event(ev)
	Input.flush_buffered_events()


func _smoke_live_step() -> Dictionary:
	# The exact dict _process() builds for modes 2/3 — the regression
	# that was missing: a real key event must reach the fold.
	var inp := {
		"keys": _gameplay_keys(),
		"mouse_dx": 0, "mouse_dy": 0, "mouse_dz": 0,
		"mouse_buttons": _mouse_button_bits(),
	}
	inp.merge(_ff_input(), true)
	return bridge.step_frame_input(1000.0 / 30.0, inp)


func _run_smoke_input() -> void:
	# Live-input regression — the six-defect repair: before the fix,
	# _process never sent "keys", so every configured binding
	# (Space=KeySniper, LAlt=KeyJump, LCtrl=KeyFire, arrows, items,
	# zoom, weapons, turbo) was dead in ordinary play. These checks
	# drive REAL InputEvents through the same poll the live frame
	# uses, and observe the fold's echo fields on the step result.
	var cdir := ProjectSettings.globalize_path("user://saves_input")
	DirAccess.make_dir_recursive_absolute(cdir)
	_check(bridge.frontend_boot(cdir),
		"input: frontend_boot (live binding table)")
	_reset_audio()
	bridge.step_frame_input(0.0, {})

	# --- A. Space -> KeySniper edge -> scope ------------------------
	_smoke_inject_key(KEY_SPACE, true)
	_check(_gameplay_keys().has(57),
		"input: held space maps to internal 57")
	var r: Dictionary = _smoke_live_step()
	_check(bool(r["input"]["sniper_pulse"]),
		"input: space edge -> sniperPulse")
	r = _smoke_live_step()
	_check(not bool(r["input"]["sniper_pulse"]),
		"input: held space -> single pulse, no repeat")
	var scoped := false
	for i in 8:
		_smoke_live_step()
		if bool(bridge.get_hud_snapshot().get("sniper_view", false)):
			scoped = true
			break
	_check(scoped, "input: sniper_view armed after space pulse")
	_smoke_inject_key(KEY_SPACE, false)

	# --- B. Rebind: KeySniper (global 7) -> 'E' (internal 18) -------
	_check(bridge.qa_set_key_global(7, 18),
		"input: rebind snipe slot -> E")
	_smoke_live_step()   # syncBindings_ mirrors the new table
	_smoke_inject_key(KEY_SPACE, true)
	r = _smoke_live_step()
	_check(not bool(r["input"]["sniper_pulse"]),
		"input: space dead after rebind")
	_smoke_inject_key(KEY_SPACE, false)
	_smoke_inject_key(KEY_E, true)
	r = _smoke_live_step()
	_check(bool(r["input"]["sniper_pulse"]),
		"input: E edge -> sniperPulse (rebind honored)")
	_smoke_inject_key(KEY_E, false)
	# The pulse toggled scope off — verify the view followed.
	scoped = true
	for i in 8:
		_smoke_live_step()
		if not bool(bridge.get_hud_snapshot().get("sniper_view",
				false)):
			scoped = false
			break
	_check(not scoped, "input: sniper_view cleared after E pulse")
	_check(bridge.qa_set_key_global(7, 57),
		"input: restore factory snipe")
	_smoke_live_step()

	# --- C. LAlt -> KeyJump level -> airborne -----------------------
	_smoke_inject_key(KEY_ALT, true)
	r = _smoke_live_step()
	_check(bool(r["input"]["jump"]), "input: LAlt -> jump level")
	var airborne := false
	for i in 30:
		r = _smoke_live_step()
		if not bool(r["grounded"]):
			airborne = true
			break
	_check(airborne, "input: held LAlt -> Kurt airborne")
	_smoke_inject_key(KEY_ALT, false)
	for i in 3:
		_smoke_live_step()

	# --- D. LCtrl -> fire level -------------------------------------
	_smoke_inject_key(KEY_CTRL, true)
	r = _smoke_live_step()
	_check(bool(r["input"]["fire"]), "input: LCtrl -> fire level")
	_smoke_inject_key(KEY_CTRL, false)
	_smoke_live_step()

	# --- E. Arrows -> turn/move axes (KeyLeft=105 / KeyUp=103) ------
	_smoke_inject_key(KEY_LEFT, true)
	r = _smoke_live_step()
	_check(float(r["input"]["turn_axis"]) < 0.0,
		"input: left arrow -> turnAxis < 0")
	_smoke_inject_key(KEY_LEFT, false)
	_smoke_inject_key(KEY_UP, true)
	r = _smoke_live_step()
	_check(float(r["input"]["move_digital"]) < 0.0,
		"input: up arrow -> moveDigital < 0 (fwd)")
	_smoke_inject_key(KEY_UP, false)
	_smoke_live_step()

	# --- F. MMB -> sniper via the mouse-button mask (bit2 -> 4) ----
	var mev := InputEventMouseButton.new()
	mev.button_index = MOUSE_BUTTON_MIDDLE
	mev.pressed = true
	Input.parse_input_event(mev)
	Input.flush_buffered_events()
	r = _smoke_live_step()
	_check(bool(r["input"]["sniper_pulse"]),
		"input: MMB edge -> sniperPulse (mouse mask)")
	var mrel := InputEventMouseButton.new()
	mrel.button_index = MOUSE_BUTTON_MIDDLE
	mrel.pressed = false
	Input.parse_input_event(mrel)
	Input.flush_buffered_events()
	_smoke_live_step()

	# --- G. Mode-2 freefall: bound arrows steer via "keys" ----------
	# (Space-held ending is irrelevant — mode 2 consumes directions.)
	_check(bridge.load_freefall(0, 1, 12648430),
		"input: freefall load")
	_smoke_inject_key(KEY_LEFT, true)
	r = _smoke_live_step()
	_check(bool(r["input"]["left"]),
		"input: bound left arrow -> freefall left")
	_smoke_inject_key(KEY_LEFT, false)
	_smoke_inject_key(KEY_A, true)
	r = _smoke_live_step()
	_check(bool(r["input"]["left"]),
		"input: 'A' alias -> freefall left (dict)")
	_smoke_inject_key(KEY_A, false)
	_smoke_live_step()


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
	# Phase 17C.2 — the scope/fire/impact/teardown steps above all
	# drained the audio batch through _step_n. The level-agnostic
	# invariants: banks bound, at least the SNIPERON restart and
	# SNIPERSHOT one-shot decoded + played, pool bounded.
	var astc: Dictionary = bridge.get_audio_stats()
	print("  combat(%s) audio: resolved=%d missing=%d active=%d" %
		[tag, int(astc["resolved"]), int(astc["missing"]),
		int(astc["active"])] +
		" starts=%d names=%s" %
		[int(audio_stats["starts"]), str(audio_stats["names"])])
	# LEVEL<n>S + LEVEL<n>O (zone ambience) + TRAVERSE + MDKSOUND.
	_check(int(astc["banks"]) == 4,
		"combat(%s): audio banks bound" % tag)
	_check(audio_stats["names"].has("SNIPERSHOT"),
		"combat(%s): SNIPERSHOT decoded+played" % tag)
	_check(int(astc["active"]) <= AUDIO_VOICES,
		"combat(%s): voice pool bounded" % tag)


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


# ---------------------------------------------------------------------------
# Phase 18B.2A — frontend presentation layer
#
# Godot is presentation + routing ONLY. FrontendShell owns the menu
# state machine, overlays, request queue, and input repeat/debounce;
# the bridge maps these dictionaries onto FrontendMenuInput verbatim.
# ---------------------------------------------------------------------------

# The four-key semantic edge set _fe_key_edge produces, plus the
# internal-domain raw bitmap (rawKeyEdge — the keyboard child's
# capture path). Held fields are polled per frame in _fe_input.
func _fe_key_edge(event: InputEventKey) -> void:
	# Semantic edges — the same fields frontendInputFromSdl sets for
	# the SDL app. Positional (physical) keycodes match DIK layout.
	match event.physical_keycode:
		KEY_ENTER, KEY_KP_ENTER:
			fe_edges["confirm"] = true
		KEY_ESCAPE:
			fe_edges["cancel"] = true
		KEY_RIGHT:
			fe_edges["attract"] = true   # DIK_RIGHT edge advances attract
			fe_edges["right_edge"] = true
		KEY_LEFT:
			fe_edges["left_edge"] = true
		KEY_PAGEUP:
			fe_edges["page_up"] = true
		KEY_PAGEDOWN:
			fe_edges["page_down"] = true
		KEY_HOME:
			fe_edges["home"] = true
			fe_edges["name_home"] = true
		KEY_END:
			fe_edges["end"] = true
			fe_edges["name_end"] = true
		KEY_BACKSPACE:
			fe_edges["name_bs"] = true
		KEY_DELETE:
			fe_edges["name_del"] = true
		KEY_Y:
			fe_edges["key_y"] = true
		KEY_N:
			fe_edges["key_n"] = true
		KEY_F1:
			fe_edges["f1"] = true
		KEY_F2:
			fe_edges["f2"] = true
		KEY_F3:
			fe_edges["f3"] = true
		KEY_F10:
			fe_edges["f10"] = true
		KEY_F11:
			fe_edges["f11"] = true
		KEY_F12:
			fe_edges["f12"] = true
		KEY_PAUSE:
			fe_edges["pause"] = true
		KEY_P:
			fe_edges["pause_alt"] = true
		KEY_PRINT:
			fe_edges["utility"] = true
	# Typed characters queue — the shell consumes one byte/frame.
	if event.unicode > 0 and event.unicode < 128:
		fe_typed.append(event.unicode)
	# Raw internal-domain edge (keyboard-capture path — the same
	# FUN_0046b688 translate the SDL app's dikFromSdlScancode +
	# internalKeyFromDik produce).
	var code := _fe_internal_code(event.physical_keycode)
	if code >= 0:
		fe_raw.append(code)


# Godot physical keycode -> the original's internal key code, i.e.
# internalKeyFromDik(dikFromSdlScancode(sc)) — extended DIKs already
# folded through the OBSERVED 0x49bbf0 table. -1 = unmappable.
func _fe_internal_code(k: int) -> int:
	match k:
		KEY_Q: return 0x10
		KEY_W: return 0x11
		KEY_E: return 0x12
		KEY_R: return 0x13
		KEY_T: return 0x14
		KEY_Y: return 0x15
		KEY_U: return 0x16
		KEY_I: return 0x17
		KEY_O: return 0x18
		KEY_P: return 0x19
		KEY_A: return 0x1e
		KEY_S: return 0x1f
		KEY_D: return 0x20
		KEY_F: return 0x21
		KEY_G: return 0x22
		KEY_H: return 0x23
		KEY_J: return 0x24
		KEY_K: return 0x25
		KEY_L: return 0x26
		KEY_Z: return 0x2c
		KEY_X: return 0x2d
		KEY_C: return 0x2e
		KEY_V: return 0x2f
		KEY_B: return 0x30
		KEY_N: return 0x31
		KEY_M: return 0x32
		KEY_1: return 0x02
		KEY_2: return 0x03
		KEY_3: return 0x04
		KEY_4: return 0x05
		KEY_5: return 0x06
		KEY_6: return 0x07
		KEY_7: return 0x08
		KEY_8: return 0x09
		KEY_9: return 0x0a
		KEY_0: return 0x0b
		KEY_ENTER: return 0x1c
		KEY_KP_ENTER: return 0x60        # 0x9c -> extended 0x60
		KEY_ESCAPE: return 0x01
		KEY_BACKSPACE: return 0x0e
		KEY_TAB: return 0x0f
		KEY_SPACE: return 0x39
		KEY_MINUS: return 0x0c
		KEY_EQUAL: return 0x0d
		KEY_BRACKETLEFT: return 0x1a
		KEY_BRACKETRIGHT: return 0x1b
		KEY_BACKSLASH: return 0x2b
		KEY_SEMICOLON: return 0x27
		KEY_APOSTROPHE: return 0x28
		KEY_QUOTELEFT: return 0x29
		KEY_COMMA: return 0x33
		KEY_PERIOD: return 0x34
		KEY_SLASH: return 0x35
		KEY_CAPSLOCK: return 0x3a
		KEY_F1: return 0x3b
		KEY_F2: return 0x3c
		KEY_F3: return 0x3d
		KEY_F4: return 0x3e
		KEY_F5: return 0x3f
		KEY_F6: return 0x40
		KEY_F7: return 0x41
		KEY_F8: return 0x42
		KEY_F9: return 0x43
		KEY_F10: return 0x44
		KEY_F11: return 0x57
		KEY_F12: return 0x58
		KEY_PRINT: return 0x64           # SYSRQ -> 100
		KEY_SCROLLLOCK: return 0x46
		KEY_PAUSE: return 0x7f           # 0xc5 -> extended 0x7f
		KEY_INSERT: return 0x6e          # 0xd2 -> 110
		KEY_HOME: return 0x66            # 0xc7 -> 102
		KEY_PAGEUP: return 0x68          # 0xc9 -> 104
		KEY_DELETE: return 0x6f          # 0xd3 -> 111
		KEY_END: return 0x6b             # 0xcf -> 107
		KEY_PAGEDOWN: return 0x6d        # 0xd1 -> 109
		KEY_RIGHT: return 0x6a           # 0xcd -> 106
		KEY_LEFT: return 0x69            # 0xcb -> 105
		KEY_DOWN: return 0x6c            # 0xd0 -> 108
		KEY_UP: return 0x67              # 0xc8 -> 103
		KEY_NUMLOCK: return 0x45
		KEY_KP_DIVIDE: return 0x63       # 0xb5 -> 99
		KEY_KP_MULTIPLY: return 0x37
		KEY_KP_SUBTRACT: return 0x4a
		KEY_KP_ADD: return 0x4e
		KEY_KP_1: return 0x4f
		KEY_KP_2: return 0x50
		KEY_KP_3: return 0x51
		KEY_KP_4: return 0x4b
		KEY_KP_5: return 0x4c
		KEY_KP_6: return 0x4d
		KEY_KP_7: return 0x47
		KEY_KP_8: return 0x48
		KEY_KP_9: return 0x49
		KEY_KP_0: return 0x52
		KEY_KP_PERIOD: return 0x53
		KEY_CTRL: return 0x1d
		KEY_SHIFT: return 0x2a
		KEY_ALT: return 0x38
		KEY_META: return 0x7f            # LWIN -> 0x7f
		_: return -1


# Build this frame's FrontendMenuInput dictionary — held fields are
# polled, edge fields come from _fe_key_edge, and the raw bitmap is
# OR'd into four dwords (the FUN_0046b688 internal domain).
func _fe_input() -> Dictionary:
	var d := {
		"prev": Input.is_key_pressed(KEY_UP),
		"next": Input.is_key_pressed(KEY_DOWN),
		"left": Input.is_key_pressed(KEY_LEFT),
		"right": Input.is_key_pressed(KEY_RIGHT),
		"mouse_dx": mouse_dx,
		"mouse_dy": mouse_dy,
		# DIMOUSESTATE.lZ — wheel ticks scaled to the ±120 detent
		# domain (the SDL path multiplies by WHEEL_DELTA the same way).
		"mouse_dz": mouse_dz * 120,
		"mouse_buttons": _mouse_button_bits(),
	}
	for k in fe_edges.keys():
		d[k] = true
	fe_edges.clear()
	if not fe_typed.is_empty():
		d["typed"] = fe_typed[0]
		fe_typed.remove_at(0)
	var raw := PackedInt32Array()
	raw.resize(4)
	for code in fe_raw:
		if code >= 0 and code < 128:
			raw[code >> 5] = raw[code >> 5] | (1 << (code & 31))
	d["raw_edges"] = raw
	fe_raw.clear()
	mouse_dx = 0
	mouse_dy = 0
	mouse_dz = 0
	return d


func _frontend_show() -> void:
	# The frontend owns the full frame: every gameplay presentation
	# layer is hidden — no world under the menu (phase §18).
	$FrontendLayer/FrontendRect.visible = true
	$FrontendLayer.visible = true
	_hide_gameplay_layers()


func _frontend_hide() -> void:
	$FrontendLayer/FrontendRect.visible = false
	$FrontendLayer.visible = false
	_show_gameplay_layers()


func _hide_gameplay_layers() -> void:
	$ArenaRoot.visible = false
	$FreefallRoot.visible = false
	$DynamicObjectRoot.visible = false
	$PlayerRoot.visible = false
	$ShotRoot.visible = false
	$FxRoot.visible = false
	$KurtLayer.visible = false
	$BezelLayer.visible = false
	$ScopeLayer.visible = false
	$ShotCamLayer.visible = false
	$HudLayer.visible = false
	$StreamLayer.visible = false
	$EndingLayer.visible = false
	$BriefingLayer.visible = false


func _show_gameplay_layers() -> void:
	$ArenaRoot.visible = true
	$FreefallRoot.visible = true
	$DynamicObjectRoot.visible = true
	$PlayerRoot.visible = true
	$ShotRoot.visible = true
	$FxRoot.visible = true
	$KurtLayer.visible = true
	$BezelLayer.visible = true
	$ScopeLayer.visible = true
	$ShotCamLayer.visible = true
	$HudLayer.visible = true


# FrontendFx ids (mdk::FrontendFx order).
const FE_FX_PAUSE_SOUNDS := 0
const FE_FX_RESUME_SOUNDS := 1
const FE_FX_RESUME_SOUNDS_ALT := 2
const FE_FX_MENU_SONG := 3
const FE_FX_LOAD_RESOURCES := 4
const FE_FX_TRANSITION_ARMED := 5
const FE_FX_ABORT_RESOURCES := 6
const FE_FX_THUMB_GRAB := 7

func _frontend_fx(f: int) -> void:
	fe_fx_counts[f] = int(fe_fx_counts.get(f, 0)) + 1
	if f == FE_FX_TRANSITION_ARMED:
		# Returning-entry transition armed — run the INTRO1A still
		# under the six-phase palette timeline (FUN_0041e554); the
		# ack is FUN_0041ebf4's clear point.
		fe_transition_ms = 0.0
		fe_transition_total_ms = bridge.frontend_transition_seconds() * 1000.0
	elif f == FE_FX_THUMB_GRAB:
		# FUN_00427e8c's arm-time capture — the bridge samples the
		# live indexed frame into the staged THMB record.
		bridge.frontend_capture_thumbnail()
	elif f == FE_FX_MENU_SONG:
		# FUN_0041d720 — the mode-0 entry restarts the MAINSONG
		# ambient bed (DAT_00541492 == 0 in the front-end).
		_fe_play_record("MAINSONG", true)
	elif f == FE_FX_PAUSE_SOUNDS:
		# FUN_00402510 — pause every live voice.
		for p in audio_players:
			if p != null:
				p.stream_paused = true
		for p in fe_audio_players.values():
			if p != null:
				p.stream_paused = true
	elif f == FE_FX_RESUME_SOUNDS or f == FE_FX_RESUME_SOUNDS_ALT:
		for p in audio_players:
			if p != null:
				p.stream_paused = false
		for p in fe_audio_players.values():
			if p != null:
				p.stream_paused = false


# --- Phase 19C.4 — frontend music host ------------------------------
# MAINSONG (OPTIONS.BNI — the FUN_0041d720 ambient bed) and OPTSONG
# (MDKSOUND.SNI music class) ride dedicated players; OPTBUTT is the
# menu-activate blip. The sound menu's semantic events arrive via
# bridge.frontend_drain_audio_events(); volumes mirror the live
# DAT_00541308/0c globals through bridge.frontend_volumes().
var fe_audio_players := {}   # record name -> AudioStreamPlayer
var fe_audio_vol := {}       # record name -> 0..0x7fff base vol

const FE_SND_AMBIENT_STOP := 0    # FUN_0041d774 — stop+release
const FE_SND_SONG_START := 1      # FUN_00402388(OPTSONG, 0) ensure
const FE_SND_BUTTON := 2          # FUN_00402388(OPTBUTT, 1) restart
const FE_SND_VOL_APPLIED := 3     # FUN_004024c4 — push to live voices
const FE_SND_SONG_STOP := 4       # FUN_0040210c(OPTSONG)
const FE_SND_AMBIENT_START := 5   # FUN_0041d720 — MAINSONG restart

func _fe_play_record(name: String, restart: bool) -> void:
	var def: Dictionary = bridge.frontend_song_stream(name)
	if def.is_empty():
		return
	var p: AudioStreamPlayer = fe_audio_players.get(name, null)
	if p == null:
		p = AudioStreamPlayer.new()
		p.name = "FeSnd_" + name
		add_child(p)
		fe_audio_players[name] = p
		fe_audio_vol[name] = int(def.get("vol", 0x7fff))
	elif not restart and p.playing:
		return   # FUN_00402388(h, 0) — play-if-not-playing
	p.stream = def["stream"]
	p.pitch_scale = 1.0
	# OPTBUTT is the menu blip — the SFX slider domain; the songs
	# take the music slider. (Host reading of the OBSERVED
	# FUN_004024c4 push — both slider globals exist.)
	var vols: Dictionary = bridge.frontend_volumes()
	var pct: int = int(vols["sound_fx"]) if name == "OPTBUTT" \
			else int(vols["sound_music"])
	p.volume_db = float(bridge.audio_vol_db(
		int(fe_audio_vol[name]), pct))
	p.play()

func _fe_stop_record(name: String) -> void:
	var p: AudioStreamPlayer = fe_audio_players.get(name, null)
	if p != null and p.playing:
		p.stop()

func _fe_apply_volumes() -> void:
	# FUN_004024c4 — both slider globals push onto live voices.
	var vols: Dictionary = bridge.frontend_volumes()
	for name in fe_audio_players:
		var p: AudioStreamPlayer = fe_audio_players[name]
		if p != null and p.playing:
			var pct: int = int(vols["sound_fx"]) if name == "OPTBUTT" \
					else int(vols["sound_music"])
			p.volume_db = float(bridge.audio_vol_db(
				int(fe_audio_vol.get(name, 0x7fff)), pct))

func _fe_stop_songs() -> void:
	# The mode-2/3 bank load releases the frontend records — the
	# ambient bed/menu song die at the gameplay boundary.
	for p in fe_audio_players.values():
		if p != null and p.playing:
			p.stop()

func _fe_audio_event(e: int) -> void:
	match e:
		FE_SND_AMBIENT_STOP:
			_fe_stop_record("MAINSONG")
		FE_SND_SONG_START:
			_fe_play_record("OPTSONG", false)
		FE_SND_BUTTON:
			_fe_play_record("OPTBUTT", true)
		FE_SND_VOL_APPLIED:
			_fe_apply_volumes()
		FE_SND_SONG_STOP:
			_fe_stop_record("OPTSONG")
		FE_SND_AMBIENT_START:
			_fe_play_record("MAINSONG", true)


# True when the input dictionary carries any key edge — the OBSERVED
# transition skip flag 0x54b57c ("any key edge in codes 0..127").
# raw_edges already covers every mappable physical key; the named
# fields are edge-triggered entries (the smoke's injected dicts and
# any unmapped semantic edge). Held levels (prev/next/left/right)
# are NOT edges — a held key must not skip. Mouse buttons/motion are
# not keys and do not skip.
const FE_EDGE_KEYS := [
	"confirm", "attract", "cancel",
	"page_up", "page_down", "home", "end",
	"key_y", "key_n", "f1", "f2", "f3", "f10", "f11", "f12",
	"pause", "pause_alt", "utility",
	"left_edge", "right_edge",
	"name_bs", "name_del", "name_home", "name_end",
]

func _fe_any_edge(d: Dictionary) -> bool:
	for k in FE_EDGE_KEYS:
		if bool(d.get(k, false)):
			return true
	if int(d.get("typed", 0)) != 0:
		return true
	var raw = d.get("raw_edges", null)
	if raw != null:
		for v in raw:
			if int(v) != 0:
				return true
	return false


# Per-frame transition bookkeeping shared by the live loop and the
# smoke. While the window is active the frame routes to the
# transition machine, not the menu pump (0x49aa7c's dispatch) — so
# the input the shell sees that frame is empty. A key edge skips
# (0x54b57c); the window closes when the elapsed time reaches the
# record's timeline duration. Returns the input dict to feed
# frontend_update (empty while the transition owns the frame).
func _fe_transition_input(input: Dictionary, dt_ms: float) -> Dictionary:
	if fe_transition_ms < 0.0:
		return input
	if _fe_any_edge(input):
		bridge.frontend_transition_complete()
		fe_transition_acks += 1
		fe_transition_ms = -1.0
		return {}
	fe_transition_ms += dt_ms
	if fe_transition_ms >= fe_transition_total_ms:
		bridge.frontend_transition_complete()
		fe_transition_acks += 1
		fe_transition_ms = -1.0
	return {}


# FrontendRequest ids (mdk::FrontendRequest order).
const FE_REQ_QUIT := 1
const FE_REQ_NEW_GAME := 2
const FE_REQ_CONTINUE := 3
const FE_REQ_LOAD_SAVE := 4
const FE_REQ_WRITE_SAVE := 5
const FE_REQ_ABORT := 6
const FE_REQ_RESUME := 7
const FE_REQ_BRIGHTNESS := 8
const FE_REQ_CAPTURE := 9
const FE_REQ_LEGACY := 10

func _frontend_request(req: Dictionary) -> void:
	# The bridge already dispatched host-owned requests; this sees the
	# report + the presentation/app-owned leftovers (handled=false).
	var r := int(req.get("request", 0))
	fe_req_counts[r] = int(fe_req_counts.get(r, 0)) + 1
	if bool(req.get("handled", false)):
		return
	if r == FE_REQ_QUIT:
		# DAT_0054148e — the process quit is app-owned.
		fe_quit = true
	elif r == FE_REQ_BRIGHTNESS:
		# The brightness global already wrapped shell-side; the
		# upload is the next frame's palette — nothing to do here.
		pass
	elif r == FE_REQ_LEGACY or r == FE_REQ_CAPTURE:
		pass   # diagnostic only — the seams are not ported


func _frontend_present() -> void:
	if not $FrontendLayer/FrontendRect.visible:
		_frontend_show()
	# The armed-transition elapsed time drives the frame — the core
	# shows the INTRO1A still under the blended palette when
	# fe_transition_ms >= 0.
	var fr: Dictionary = bridge.frontend_frame(fe_transition_ms)
	if fr.is_empty():
		return
	var w := int(fr["w"])
	var h := int(fr["h"])
	fe_img = Image.create_from_data(w, h, false,
		Image.FORMAT_RGBA8, fr["rgba"])
	if fe_tex == null:
		fe_tex = ImageTexture.create_from_image(fe_img)
	else:
		fe_tex.update(fe_img)
	$FrontendLayer/FrontendRect.texture = fe_tex


# One frontend-owned frame: shell update, request dispatch, fx
# drain, transition ack, present, end_frame — the phase §2 loop.
func _frontend_frame(delta: float, mode: int) -> void:
	var input := _fe_input()
	# --fe-run: a single confirm on the real input path (fresh-entry
	# selection is New Game) — every later transition is production.
	# The entry transition eats the first edge as a skip-ack, so the
	# press waits for its window to close.
	if fe_run and not _fe_run_done:
		if fe_transition_ms < 0.0:
			_fe_run_done = true
			input["confirm"] = true
	if mode == 0:
		var snap: Dictionary = bridge.frontend_update(
			_fe_transition_input(input, delta * 1000.0))
		fe_sub = int(snap.get("sub_mode", 0))
		for req in bridge.frontend_dispatch_requests():
			_frontend_request(req)
		for f in bridge.frontend_drain_fx():
			_frontend_fx(int(f))
		for e in bridge.frontend_drain_audio_events():
			_fe_audio_event(int(e))
		_frontend_present()
		# Queued mixer commands (a teardown tail from freefall or
		# mode 5 that landed on the frontend route) still deliver —
		# the drain is mode-agnostic.
		_drain_audio_fx()
		bridge.frontend_end_frame(delta * 1000.0)
		if fe_quit:
			get_tree().quit(0)
		return
	# Modes 5..8 — the progression pump. No runtime/presentation of
	# their own: a bounded placeholder hold (or a confirm) marks the
	# stage-complete seam, then the bridge installs the next mode.
	#
	# Mode 5 is the exception once the pump installs the live
	# StreamScene (frontend_progression_step arms campaignStreamEnter_
	# on first contact): the scene is stepped + presented here on the
	# paced ~33.3ms cadence (19C.1), until its kExitMode drives the
	# handoff.
	if mode == 5 and bridge.stream_active():
		if _stream_pace_run(delta * 1000.0) > 0:
			_apply_stream()
		# The voice commands drain once per rendered frame — the
		# standalone mode-5 drain below is unreachable under
		# fe_active (this branch returns first), so the frontend
		# route needs its own.
		_drain_audio_fx()
		bridge.frontend_end_frame(delta * 1000.0)
		return
	# Mode 8 is runtime-owned — the bridge's stepEnding_ pumps the
	# cinematic through the paced gate; a load failure takes the
	# original's missing-file edge straight to the frontend.
	if mode == 8:
		if _ending_pace_run(delta * 1000.0) > 0:
			_apply_ending()
		_ending_drain_audio()
		bridge.frontend_end_frame(delta * 1000.0)
		if int(bridge.get_mode()) != 8:
			_ending_hide()
		return
	if mode == 6:
		# 19E — the briefing machine owns the stage edge for
		# loaderSub 3 (FUN_00429600's key-scan); the placeholder
		# hold doesn't apply while it runs. Input levels feed the
		# original's latches: Esc -> skip, bound-advance -> hurry
		# (60c/s + 2x fades), any-key -> the post-page exit gate.
		var bi := {
			"dt_ms": delta * 1000.0,
			"esc": Input.is_key_pressed(KEY_ESCAPE),
			"hurry": Input.is_key_pressed(KEY_ENTER) or \
				Input.is_key_pressed(KEY_SPACE) or \
				(int(input.get("mouse_buttons", 0)) & 7) != 0,
		}
		bi["any_key"] = bi["hurry"] or bi["esc"] or \
			bool(input.get("confirm", false)) or \
			bool(input.get("prev", false)) or \
			bool(input.get("next", false)) or \
			bool(input.get("left", false)) or \
			bool(input.get("right", false))
		if fe_run and not fe_brief_natural:
			# --fe-run: a held key is a legal original path (hurry
			# rate + the post-page exit gate) — keeps the scripted
			# route moving through the briefing.
			bi["any_key"] = true
			bi["hurry"] = true
		elif fe_run and fe_brief_natural:
			# Natural pace: the machine types at 15c/s with no
			# keys. After typing_done, hold 30 frames (visible
			# bf04-gate proof) then pulse one key to exit.
			var d6: Dictionary = bridge.mode6_diag()
			if bool(d6.get("typing_done", false)):
				_mode6_hold_n += 1
				if _mode6_hold_n >= 30:
					bi["any_key"] = true
		var res6: Dictionary = bridge.frontend_progression_step(bi)
		if not bool(res6.get("ok", true)):
			printerr("mode-6 briefing: ", bridge.get_last_error())
		if bridge.mode6_active():
			_apply_mode6()
		else:
			_mode6_hide()
		if int(bridge.get_mode()) != mode:
			fe_stage_hold = FE_STAGE_HOLD
		bridge.frontend_end_frame(delta * 1000.0)
		return
	fe_stage_hold -= 1
	var done := fe_stage_hold <= 0 or \
		bool(input.get("confirm", false))
	var res: Dictionary = bridge.frontend_progression_step(
		{"stage_done": done})
	if not bool(res.get("ok", true)):
		printerr("frontend progression: ", bridge.get_last_error())
		fe_stage_hold = FE_STAGE_HOLD
	var new_mode := int(bridge.get_mode())
	if new_mode != mode:
		fe_stage_hold = FE_STAGE_HOLD
	bridge.frontend_end_frame(delta * 1000.0)


# ---------------------------------------------------------------------------
# Frontend smoke — the phase §23 bounded scenarios. Synthetic input
# dictionaries drive the same bridge path _frontend_frame uses; no
# _process involvement, so the whole run is deterministic.
# ---------------------------------------------------------------------------

# Advance the frontend N frames through the same call sequence the
# live loop runs (update -> dispatch -> fx -> transition-ack ->
# frame -> end_frame). The first frame carries `input`.
func _fe_smoke_step(input: Dictionary, n: int = 1) -> Dictionary:
	var snap := {}
	for i in n:
		var frame_input: Dictionary = input if i == 0 else {}
		snap = bridge.frontend_update(
			_fe_transition_input(frame_input, 33.333))
		for req in bridge.frontend_dispatch_requests():
			_frontend_request(req)
		for f in bridge.frontend_drain_fx():
			_frontend_fx(int(f))
		# Match _frontend_frame's consumer order: the song/button
		# event queue drains every stepped frame, else Sound-screen
		# SongStart/OPTBUTT events pool up and never reach a player.
		for e in bridge.frontend_drain_audio_events():
			_fe_audio_event(int(e))
		bridge.frontend_frame(fe_transition_ms)
		bridge.frontend_end_frame(33.333)
	fe_sub = int(snap.get("sub_mode", 0)) if not snap.is_empty() else 0
	return snap


# One semantic key press = one held frame + one released frame. The
# shell's repeat/debounce deadlines only reset on release — a second
# held frame inside the 30-tick first-delay window is a hold, not a
# new press.
func _fe_press(field: String) -> Dictionary:
	_fe_smoke_step({field: true}, 1)
	return _fe_smoke_step({}, 1)


# ---- Phase 18B.2B closeout helpers — deterministic frame folds ----

# FNV-1a fold over the composed RGBA frame (600x360x4).
func _ba_hash(b: PackedByteArray) -> int:
	var h := -3750763034362895579   # 0xcbf29ce484222325
	for i in range(0, b.size(), 253):
		h = (h ^ int(b[i])) * 0x100000001b3
	return h


func _frame_hash(transition_ms: float = -1.0) -> int:
	var fr: Dictionary = bridge.frontend_frame(transition_ms)
	if fr.is_empty():
		return 0
	return _ba_hash(fr["rgba"])


func _rect_hash(rgba: PackedByteArray, x0: int, y0: int,
		w: int, h: int) -> int:
	var hh := -3750763034362895579
	for y in range(y0, y0 + h):
		for x in range(x0, x0 + w):
			var o := (y * 600 + x) * 4
			hh = (hh ^ int(rgba[o])) * 0x100000001b3
			hh = (hh ^ int(rgba[o + 1])) * 0x100000001b3
			hh = (hh ^ int(rgba[o + 2])) * 0x100000001b3
			hh = (hh ^ int(rgba[o + 3])) * 0x100000001b3
	return hh


# Per-row changed-pixel counts between two composed frames, bounded
# to x < x_max (keeps the detail pane out of the bracket/row diff).
func _row_diff_counts(a: PackedByteArray, b: PackedByteArray,
		x_max: int) -> Array:
	var counts := []
	for r in 13:
		counts.append(0)
	for r in 13:
		var y0 := 0x67 + r * 0x10
		for y in range(y0 - 2, y0 + 14):
			if y < 0 or y >= 360:
				continue
			for x in range(0, x_max):
				var o := (y * 600 + x) * 4
				if a[o] != b[o] or a[o + 1] != b[o + 1] or \
						a[o + 2] != b[o + 2] or a[o + 3] != b[o + 3]:
					counts[r] += 1
					break
	return counts


# Leftmost pixel differing from bg inside [x_min, x_max) of a band —
# used to read the row text origin off the composed frame.
func _band_left_ink(rgba: PackedByteArray, y0: int, y1: int,
		x_min: int, x_max: int, bg: PackedByteArray) -> int:
	for x in range(x_min, x_max):
		for y in range(y0, y1):
			var o := (y * 600 + x) * 4
			if rgba[o] != bg[0] or rgba[o + 1] != bg[1] or \
					rgba[o + 2] != bg[2]:
				return x
	return -1


func _px3(rgba: PackedByteArray, x: int, y: int) -> PackedByteArray:
	var o := (y * 600 + x) * 4
	return PackedByteArray([rgba[o], rgba[o + 1], rgba[o + 2]])


# .FTI interior directory (fti_directory.h): image = file[4:]; u32
# count @img+0, then N x {name[8], img-relative u32 offset}. Records
# carry no stored size — the payload runs to the next offset.
func _fti_record(fti: PackedByteArray, rec_name: String) -> PackedByteArray:
	var n := fti.decode_u32(4)
	for i in n:
		var ro := 8 + i * 12
		# Name compares match the original's zero-padded 8-byte query:
		# ascii decode stops at the first NUL.
		if fti.slice(ro, ro + 8).get_string_from_ascii() == rec_name:
			return fti.slice(4 + fti.decode_u32(ro + 8))
	return PackedByteArray()


# The resident system palette head (64 RGB entries) — the merge's
# source for preview pens < 64 (OBSERVED palette merge).
func _sys_pal_head() -> PackedByteArray:
	var fti := FileAccess.get_file_as_bytes(
		data_root_path.path_join("MISC/MDKFONT.FTI"))
	var rec := _fti_record(fti, "SYS_PAL")
	return rec.slice(0, 192)


func _run_smoke_continue() -> void:
	# Two-process relaunch QA — run 2 of the save->quit->relaunch->
	# load pair. Run 1 (--smoke --save-dir DIR) wiped DIR, wrote
	# LASTGAME.SAV + named slots, and exited; this run boots on the
	# same root WITHOUT the wipe, so the shell must observe the
	# prior process's saves and route a real Continue request.
	print("smoke(continue): relaunch-load")
	_check(bridge.frontend_booted(), "R: frontend booted")
	_check(not smoke_save_dir.is_empty() and
		FileAccess.file_exists(smoke_save_dir.path_join(
			"LASTGAME.SAV")),
		"R: LASTGAME.SAV survived from the prior process")
	var snap := _fe_smoke_step({}, 2)
	_check(int(snap.get("mode", -1)) == 0, "R: mode == frontend")
	_check(bool(snap.get("saves_exist", false)),
		"R: relaunch sees saves_exist")
	_check(int(snap.get("selection", -1)) == 0,
		"R: Continue is the default selection")
	_check(bridge.frontend_lastgame_exists(),
		"R: LASTGAME probe true post-relaunch")
	# The prior process's named slots enumerate through the real
	# list path (sub_mode 1), then Esc back to root.
	snap = _fe_press("next")
	snap = _fe_press("next")
	_check(int(snap.get("selection", -1)) == 2,
		"R: Saved Game selectable post-relaunch")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 1,
		"R: save list opens post-relaunch")
	var sl: Dictionary = snap.get("save_list", {})
	_check(int(sl.get("count", 0)) >= 1,
		"R: save enumeration survived relaunch")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"R: Esc exits the relaunched save list")
	# Esc's root re-entry re-derives selection (0 with saves) —
	# walk to Continue deterministically either way.
	for i in 6:
		if int(snap.get("selection", -1)) == 0:
			break
		snap = _fe_press("prev")
	_check(int(snap.get("selection", -1)) == 0,
		"R: Continue re-selected")
	snap = _fe_press("confirm")
	var mode := int(bridge.get_mode())
	_check(mode == 0 or mode == 3 or mode == 6,
		"R: Continue request routed (mode %d)" % mode)
	# The original-data root stays read-only: the relaunch wrote
	# nothing into <data>/SAVES.
	_check(not FileAccess.file_exists(
		data_root_path.path_join("SAVES/REL_SMOKE.SAV")),
		"R: no writes into the data root")
	print("smoke(continue): %d failure(s)" % failures)
	get_tree().quit(1 if failures else 0)


func _run_smoke_frontend() -> void:
	print("smoke: frontend")
	_check(bridge.frontend_booted(), "frontend booted")

	# --- A. boot, no LASTGAME (the temp save-dir is fresh) ---------
	var snap := _fe_smoke_step({}, 2)
	_check(int(snap.get("mode", -1)) == 0, "A: mode == frontend")
	_check(int(snap.get("sub_mode", -1)) == 0, "A: sub == root")
	_check(not bool(snap.get("saves_exist", true)),
		"A: no LASTGAME -> saves_exist false")
	_check(int(snap.get("selection", -1)) == 1,
		"A: selection defaults to New Game")

	# --- C. root navigation ----------------------------------------
	# No saves: Continue (0) is disabled — prev from 1 wraps to the
	# bottom and next from the bottom wraps back to 1, both skipping
	# the disabled row (OBSERVED controller logic).
	snap = _fe_press("prev")
	_check(int(snap.get("selection", -1)) == 4,
		"C: prev from New Game wraps to Quit (no saves)")
	snap = _fe_press("next")
	_check(int(snap.get("selection", -1)) == 1,
		"C: next wraps back to New Game")
	snap = _fe_press("next")
	_check(int(snap.get("selection", -1)) == 2,
		"C: next advances to Saved Game")
	snap = _fe_press("next")
	snap = _fe_press("next")
	_check(int(snap.get("selection", -1)) == 4,
		"C: next x2 lands on Quit")
	snap = _fe_press("prev")                   # back off Quit -> 3

	# --- L. Abort console (frontend root Esc arm) ------------------
	# Esc at the menu arms the abort console AND self-cancels the
	# same frame — the OBSERVED quirk: the just-armed overlay
	# dispatches the still-live Esc edge as 'No'. Net effect: the
	# Pause->Resume->MenuSong fx sequence and a fresh root re-entry.
	# The visible console is exercised in scenario O via the in-game
	# F10 arm, which does not self-cancel.
	var pause0: int = int(fe_fx_counts.get(FE_FX_PAUSE_SOUNDS, 0))
	var song0: int = int(fe_fx_counts.get(FE_FX_MENU_SONG, 0))
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"L: Esc abort arm self-cancels same frame (OBSERVED)")
	_check(int(fe_fx_counts.get(FE_FX_PAUSE_SOUNDS, 0)) > pause0 and
		int(fe_fx_counts.get(FE_FX_MENU_SONG, 0)) > song0,
		"L: arm+cancel fx sequence ran (pause+song)")
	_check(int(snap.get("selection", -1)) == 1,
		"L: fresh re-entry re-derives selection")

	# --- F/G. save list (needs saves — write a header-only one) ----
	var wok: bool = bridge.frontend_write_save(
		{"name": "SMOKE1", "header_only": true})
	_check(wok, "F: header-only save write to temp dir")
	var wok2: bool = bridge.frontend_write_save(
		{"name": "SMOKE2", "header_only": true})
	_check(wok2, "F: second header-only save write")
	# F3 carries the same sub==0 && mode!=0 gate as F1 — an OBSERVED
	# no-op at the menu. The menu route is the Saved Game item.
	snap = _fe_press("f3")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"F: F3 at the menu is a no-op (in-game gate)")
	snap = _fe_press("next")                   # 1 -> 2
	_check(int(snap.get("selection", -1)) == 2,
		"F: Saved Game item selected")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 1,
		"F: confirm -> save list")
	var sl: Dictionary = snap.get("save_list", {})
	_check(int(sl.get("count", 0)) >= 2, "F: list sees the writes")
	var stems: Array = sl.get("stems", [])
	_check(stems.has("SMOKE1"), "G: stem enumeration")
	var insp: Dictionary = bridge.frontend_inspect_slot("SMOKE1")
	_check(bool(insp.get("found", false)), "G: slot inspect found")
	_check(bool(insp.get("valid", false)), "G: header-only slot valid")
	_check(not bool(insp.get("full_save", true)),
		"G: header-only classification")
	snap = _fe_press("next")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"F: Esc exits save list")

	# --- A2. music host playback evidence --------------------------
	# fx emission alone proves the event fired — this block checks
	# the PLAYED side: stream resolution, live AudioStreamPlayer,
	# advancing playback position, volume push, and the stop/restart
	# teardown around the Sound screen.
	var song_def: Dictionary = bridge.frontend_song_stream("MAINSONG")
	_check(not song_def.is_empty() and song_def.get("stream") != null,
		"A2: MAINSONG stream resolved (OPTIONS.BNI decode)")
	var mp: AudioStreamPlayer = fe_audio_players.get("MAINSONG", null)
	_check(mp != null and mp.playing,
		"A2: MAINSONG player live after ambient-start")
	if mp != null:
		var pos0: float = mp.get_playback_position()
		var advanced := false
		# Bounded real-time mix window — the Dummy driver can take a
		# few frames to start reporting a moving position.
		for _i in range(10):
			OS.delay_msec(60)
			if mp.get_playback_position() > pos0:
				advanced = true
				break
		_check(advanced,
			"A2: MAINSONG playback position advancing")
	_check(int(bridge.frontend_volumes().get("sound_music", -1)) >= 0,
		"A2: live volume globals readable")

	# --- A3. Sound screen: OPTSONG swap + OPTBUTT + volume push ----
	# Root -> Options (sel 3) -> Sound row (1). Entry stops the
	# ambient bed and starts OPTSONG (FUN_0042322c).
	snap = _fe_press("next")
	snap = _fe_press("next")                   # 1 -> 3 (Options)
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 11,
		"A3: options sub for sound entry")
	snap = _fe_press("next")                   # 8 -> 0
	snap = _fe_press("next")                   # 0 -> 1 (Sound row)
	snap = _fe_press("confirm")
	_check(String(snap.get("screen", "")) == "sound",
		"A3: Sound screen entered")
	var opt_def: Dictionary = bridge.frontend_song_stream("OPTSONG")
	_check(not opt_def.is_empty() and opt_def.get("stream") != null,
		"A3: OPTSONG stream resolved (MDKSOUND.SNI decode)")
	var op: AudioStreamPlayer = fe_audio_players.get("OPTSONG", null)
	_check(op != null and op.playing,
		"A3: OPTSONG player live in sound screen")
	if mp != null:
		_check(not mp.playing,
			"A3: ambient MAINSONG stopped on sound entry")
	# Entry selection is row 0 (SoundFX); next -> row 1 (SoundMusic),
	# then one LEFT query fires OPTBUTT + lowers the music slider
	# + pushes VolumesApplied onto the live OPTSONG voice.
	var db0 := 0.0
	if op != null:
		db0 = op.volume_db
	var mus0 := int(bridge.frontend_volumes().get("sound_music", -1))
	snap = _fe_press("next")                   # row 0 -> 1
	var bp: AudioStreamPlayer = fe_audio_players.get("OPTBUTT", null)
	snap = _fe_press("left")
	bp = fe_audio_players.get("OPTBUTT", null)
	_check(bp != null and bp.stream != null,
		"A3: OPTBUTT player ran on the nav query")
	if op != null:
		_check(op.playing,
			"A3: OPTSONG still live through volume push")
		_check(op.volume_db < db0 or mus0 <= 0,
			"A3: LEFT query pushed a lower music volume onto OPTSONG")
	var mus1 := int(bridge.frontend_volumes().get("sound_music", -1))
	_check(mus1 <= mus0 and (mus1 < mus0 or mus0 <= 0),
		"A3: music slider global moved")
	# Esc -> SongStop + AmbientSongStart: OPTSONG dies, MAINSONG
	# restarts, then Esc leaves options to the root.
	snap = _fe_press("cancel")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"A3: Esc twice returns to root")
	if op != null:
		_check(not op.playing,
			"A3: OPTSONG stopped on sound-screen exit")
	if mp != null:
		_check(mp.playing,
			"A3: MAINSONG restarted on sound-screen exit")
	# The root controller stays alive under the options screen —
	# it resumes at the Options row, not the fresh-entry New Game
	# the following sections expect. Walk back deterministically.
	for i in 8:
		if int(snap.get("selection", -1)) == 1:
			break
		snap = _fe_press("prev")
	_check(int(snap.get("selection", -1)) == 1,
		"A3: root selection restored to New Game")

	# --- H. save-name entry (F2 gate is traversal-only; the arm
	#        through the shell's autosave path is the testable one) -
	# Drive the request path the write seam exposes instead: the
	# shell's F2 arm only fires in mode 3, so verify the writer +
	# the confirm-phase surface through a fresh autosave arm via
	# the host seam (write already covered above).
	var det: Dictionary = bridge.frontend_inspect_slot("SMOKE2")
	_check(bool(det.get("found", false)), "H: second slot detail")
	var fr: Dictionary = bridge.frontend_frame()
	_check(int(fr.get("w", 0)) == 600 and int(fr.get("h", 0)) == 360,
		"H: composed frame is 600x360")

	# --- I/J/K. Options navigation + help + skill cycle ------------
	# The save-list Esc exit ran a fresh re-entry -> selection is 1.
	snap = _fe_press("next")
	snap = _fe_press("next")                   # 1 -> 3
	_check(int(snap.get("selection", -1)) == 3,
		"I: options item selectable")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 11,
		"I: confirm -> options sub")
	# Options entry selection is row 8 (Quit/Back); all 9 rows are
	# navigable (devHidden_ is the -mapok flag, canonical 0). Row 0
	# is Help — next wraps 8 -> 0, confirm arms the sub-10 overlay.
	snap = _fe_press("next")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 10,
		"K: options Help row -> help sub")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 11,
		"K: Esc closes help -> options")
	var skill0 := int(snap.get("settings", {}).get("skill", -1))
	# Skill is options row 6 — six next presses from row 0, then the
	# activate query cycles it up in place (wraps 2 -> 0). The flow's
	# skill global syncs on the return-to-root handoff, so the check
	# reads the snapshot after Esc leaves the subtree.
	for i in 6:
		snap = _fe_press("next")
	snap = _fe_press("confirm")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"I: Esc exits options")
	var skill1 := int(snap.get("settings", {}).get("skill", -1))
	_check(skill0 >= 0 and skill1 == (skill0 + 1) % 3,
		"J: skill cycled through the contract")

	# --- M. transition arm/ack/skip --------------------------------
	# enterFrontend(true) is the returning-entry path: it arms
	# TransitionArmed + Esc suppression; the INTRO1A timeline is 10s
	# (300 ticks of 1/30) and any key edge skips it (0x54b57c,
	# OBSERVED). The ack is FUN_0041ebf4's clear point.
	bridge.frontend_enter(true)
	var fx_seen := false
	for i in 4:
		bridge.frontend_update({})
		for f in bridge.frontend_drain_fx():
			_frontend_fx(int(f))
		bridge.frontend_end_frame(33.333)
	var snap2: Dictionary = bridge.frontend_snapshot()
	fx_seen = int(fe_fx_counts.get(FE_FX_TRANSITION_ARMED, 0)) > 0
	_check(fx_seen, "M: returning entry armed TransitionArmed")
	_check(bool(snap2.get("suppress_esc_abort", false)),
		"M: Esc suppression held during transition")
	_check(fe_transition_ms >= 0.0,
		"M: transition presentation window armed")
	# The armed frame is the INTRO1A record under the blended palette,
	# not the menu — count the non-black pixels a few ticks in.
	_fe_smoke_step({}, 3)
	var tfr: Dictionary = bridge.frontend_frame(fe_transition_ms)
	_check(int(tfr.get("w", 0)) == 600, "M: transition frame composed")
	# A key edge skips (Y is inert at the root menu, so nothing else
	# dispatches off this frame).
	snap2 = _fe_smoke_step({"key_y": true}, 1)
	_check(not bool(snap2.get("suppress_esc_abort", true)),
		"M: key edge ack released Esc suppression")
	_check(fe_transition_ms < 0.0, "M: transition window closed")

	# --- D. New Game request -> progression ------------------------
	# Select index 1 (New Game) then confirm.
	for i in 6:
		snap = _fe_press("prev")
		if int(snap.get("selection", -1)) == 1:
			break
	var sel := int(snap.get("selection", -1))
	if sel != 1:
		for i in 6:
			snap = _fe_press("next")
			if int(snap.get("selection", -1)) == 1:
				break
	_check(int(snap.get("selection", -1)) == 1,
		"D: New Game selectable")
	var req_seen := false
	snap = _fe_press("confirm")
	req_seen = int(fe_req_counts.get(FE_REQ_NEW_GAME, 0)) > 0
	_check(req_seen, "D: StartNewGame request drained")
	# The campaign pump: mode 6 briefing -> mode 2 freefall.
	var mode := int(bridge.get_mode())
	_check(mode == 6, "D: campaign entry is mode 6")
	var guard := 0
	while mode >= 5 and mode <= 8 and guard < 40:
		var res: Dictionary = bridge.frontend_progression_step(
			{"stage_done": true})
		_check(bool(res.get("ok", true)) or guard < 3,
			"D: progression step ok")
		mode = int(bridge.get_mode())
		guard += 1
	_check(mode == 2, "D: progression reached freefall (mode 2)")
	_frontend_hide()
	frontend = true   # still frontend-launched; runtime now live

	# --- N. frontend -> gameplay hide ------------------------------
	_check(not $FrontendLayer/FrontendRect.visible,
		"N: frontend layer hidden on gameplay entry")

	# --- O. gameplay -> frontend return (abort YES) -----------------
	# Step a few freefall frames, then arm the console. The in-game
	# arm is F10 — an Esc-armed abort self-cancels the same frame on
	# the still-live edge (the OBSERVED quirk checked at L).
	for i in 4:
		bridge.step_frame_input(33.333, {})
	snap = _fe_press("f10")
	_check(int(snap.get("sub_mode", -1)) == 9,
		"O: F10 in gameplay arms abort")
	_check(int(snap.get("abort_selection", -1)) == 0,
		"O: abort selection starts on Yes")
	snap = _fe_press("key_y")
	# Abort YES -> AbortToFrontend request -> teardown + fresh entry.
	snap = _fe_smoke_step({}, 2)
	mode = int(bridge.get_mode())
	_check(mode == 0, "O: abort YES returned to frontend")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"O: fresh frontend entry at root")
	_frontend_show()
	_check($FrontendLayer/FrontendRect.visible,
		"O: frontend layer restored")

	# --- E. Continue (LASTGAME now exists — write it) --------------
	var lok: bool = bridge.frontend_write_save(
		{"name": "LASTGAME", "header_only": true})
	_check(lok, "E: LASTGAME write to temp dir")
	bridge.frontend_enter(false)   # re-probe saves_exist
	snap = _fe_smoke_step({}, 2)
	_check(bool(snap.get("saves_exist", false)),
		"E: LASTGAME -> saves_exist true")
	_check(int(snap.get("selection", -1)) == 0,
		"E: Continue is the default selection")
	_check(bridge.frontend_lastgame_exists(),
		"E: host LASTGAME probe true")
	snap = _fe_press("confirm")
	# Continue request drained — the header-only mode field decides
	# the route; a 0-mode field falls back to frontend (mode 0).
	mode = int(bridge.get_mode())
	_check(mode == 0 or mode == 3 or mode == 6,
		"E: continue request routed")

	# Temp-save safety: every write this smoke did landed under the
	# user:// temp root, never the original tree.
	var save_abs := ProjectSettings.globalize_path("user://saves_smoke")
	_check(FileAccess.file_exists(save_abs.path_join("LASTGAME.SAV")),
		"temp: LASTGAME under user:// saves_smoke root")
	_check(not FileAccess.file_exists(
		data_root_path.path_join("SAVES/SMOKE1.SAV")),
		"temp: no writes into the data root")

	# ============================================================
	# Phase 18B.2B closeout — runtime validation of the landed
	# presentation seams. Every check composes through the real
	# bridge path (no visual approximation).
	# ============================================================

	# --- V. attract: shell-owned idle entry -> real MDKS slides
	#        -> corpus walk -> wrap to menu (FUN_0041dc90/0041ef74) --
	# Normalize to a mode-0 root first — E's continue may have routed
	# to mode 3/6; frontend_enter(false) is FUN_0041d85c(0) fresh.
	bridge.frontend_enter(false)
	snap = _fe_smoke_step({}, 2)
	_check(int(bridge.get_mode()) == 0,
		"V: frontend_enter returns to mode 0")
	_check(int(snap.get("attract_state", -1)) == 0,
		"V: fresh entry attract state 0")
	var menu_hash := _frame_hash()
	# The idle timer is shell-owned (DAT_0049aaa4 += frameStep) — the
	# smoke input carries no attract field; 5s of empty frames fires
	# state 0 -> 1 (OBSERVED 0x495a20 menu delay).
	var vi := 0
	while int(snap.get("attract_state", -1)) == 0 and vi < 200:
		snap = _fe_smoke_step({}, 1)
		vi += 1
	_check(int(snap.get("attract_state", -1)) == 1,
		"V: idle ~5s -> attract state 1 (shell timer, no GDScript timer)")
	_check(bool(snap.get("attract_slide_active", false)),
		"V: slide 1 live after idle entry")
	_check(bool(snap.get("menu_strings_hidden", false)),
		"V: menu strings hidden at state 1")
	var dg1 := _frame_hash()
	_check(dg1 != menu_hash, "V: state-1 frame is the slide, not menu")
	# Corpus walk — each attract edge (DIK_RIGHT, 0x54b554) advances
	# one slide; the probe-fail past the end wraps state > 1 to 0.
	var slide_digests := {}
	var wg := 0
	var st := int(snap["attract_state"])
	while st > 0 and wg < 24:
		var dg := _frame_hash()
		_check(dg != menu_hash,
			"V: state %d composes a slide, not the menu" % st)
		_check(not slide_digests.has(dg),
			"V: state %d slide digest distinct" % st)
		slide_digests[dg] = st
		if st >= 2:
			_check(not bool(snap.get("menu_strings_hidden", true)),
				"V: strings overlay at state %d" % st)
		snap = _fe_smoke_step({"attract": true}, 1)
		st = int(snap.get("attract_state", -1))
		wg += 1
	_check(st == 0, "V: corpus end wraps attract to state 0")
	_check(_frame_hash() == menu_hash,
		"V: post-wrap frame is the plain menu (no slide residue)")
	# Input mid-attract drives the menu underneath — the OBSERVED
	# FUN_0041dc90 structure has no key-driven attract exit (0x49aa98
	# is only written by the advance/wrap and the entry reset). The
	# state-1 idle-reset quirk (0x4479c000 = 999.0f) auto-advances on
	# the same frame the input lands.
	snap = _fe_smoke_step({"attract": true}, 1)
	_check(int(snap.get("attract_state", -1)) == 1,
		"V: attract edge re-enters state 1")
	var sel_before := int(snap.get("selection", -1))
	snap = _fe_smoke_step({"next": true}, 1)
	_check(int(snap.get("selection", -1)) != sel_before,
		"V: input reaches the menu under attract (no exit)")
	_check(int(snap.get("attract_state", -1)) == 2,
		"V: state-1 999.0f idle quirk auto-advances on input")
	snap = _fe_smoke_step({}, 1)
	# Probe the whole corpus — the 600x360 gate per slide index.
	var slide_n := 0
	while slide_n < 24:
		var pr: Dictionary = bridge.frontend_slide_probe(slide_n + 1)
		if not bool(pr.get("exists", false)):
			break
		_check(int(pr.get("width", 0)) == 600 and
			int(pr.get("height", 0)) == 360,
			"V: MDKS_%03d passes the 600x360 gate" % (slide_n + 1))
		slide_n += 1
	_check(slide_n >= 2, "V: real MDKS corpus probed (%d)" % slide_n)
	_check(slide_digests.size() == slide_n,
		"V: every probed slide produced a distinct composed frame")
	print("  V: slides probed=%d distinct=%d" % [
		slide_n, slide_digests.size()])
	# Walk out: advance past the corpus end again -> wrap to menu.
	wg = 0
	while int(snap.get("attract_state", -1)) > 0 and wg < 24:
		snap = _fe_smoke_step({"attract": true}, 1)
		wg += 1
	_check(int(snap.get("attract_state", -1)) == 0,
		"V: second walk wraps to menu")

	# --- W. save-list geometry: OBSERVED rows/bracket/title -------
	while int(snap.get("selection", -1)) != 2:
		snap = _fe_press("next")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 1, "W: save list opens")
	# Park the selection on row 0 for a deterministic diff.
	var wsel := 0
	while int(snap.get("save_list", {}).get("selection", -1)) != 0 \
			and wsel < 20:
		snap = _fe_press("prev")
		wsel += 1
	var lf0: Dictionary = bridge.frontend_frame()   # row 0 selected
	snap = _fe_press("next")
	_check(int(snap.get("save_list", {}).get("selection", -1)) == 1,
		"W: next selects row 1")
	var lf1: Dictionary = bridge.frontend_frame()   # row 1 selected
	# The diff is the FUN_00414b28 bracket leaving row 0 and landing on
	# row 1 — text is identical, so exactly the two adjacent 0x10
	# bands change (the detail pane is excluded via x_max).
	var rc := _row_diff_counts(lf0["rgba"], lf1["rgba"], 330)
	var changed_rows := []
	for r in 13:
		if rc[r] > 0:
			changed_rows.append(r)
	_check(changed_rows == [0, 1],
		"W: selection move diffs exactly rows 0+1 (step 0x10)")
	# On the sel-1 frame row 0 is unselected — pure text ink, first
	# column at the OBSERVED x=0x62 modulo glyph side-bearing.
	var bg := _px3(lf1["rgba"], 590, 355)
	var row0_left := _band_left_ink(
		lf1["rgba"], 0x67 - 14, 0x67 + 4, 4, 330, bg)
	_check(row0_left >= 94 and row0_left <= 112,
		"W: row text starts at x~0x62 (got %d)" % row0_left)
	# Title SVOPT1 centered at y=0x1f — centered ink, never at row x.
	var title_ink := _band_left_ink(
		lf1["rgba"], 0x1f, 0x1f + 30, 4, 596, bg)
	_check(title_ink > 60 and title_ink < 400,
		"W: SVOPT1 title band carries centered ink")
	# Bracket blink cadence: the shared accumulator steps inside the
	# flagged draw (fild/fadd/fistp in FUN_00414b28), bit 3 selects
	# the phase — the bracket region alternates between exactly two
	# phase frames at ~8 draws per phase.
	snap = _fe_press("prev")   # selection back to row 0
	var blink_hashes := []
	for i in 20:
		var bfr: Dictionary = bridge.frontend_frame()
		# row-0 bracket box: x [96, end+2], y [89, 107]
		blink_hashes.append(_rect_hash(bfr["rgba"], 90, 85, 160, 27))
		_fe_smoke_step({}, 1)
	var blink_set := {}
	var toggles := 0
	for i in 20:
		blink_set[blink_hashes[i]] = true
		if i > 0 and blink_hashes[i] != blink_hashes[i - 1]:
			toggles += 1
	_check(blink_set.size() == 2,
		"W: bracket phase alternates between exactly 2 frames")
	_check(toggles >= 2 and toggles <= 6,
		"W: blink toggles on the ~8-draw accumulator cadence")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"W: Esc exits the save list")

	# --- Y. THMB golden: traversal F2 arm -> capture -> write ->
	#        byte-exact inspect -> list display ---------------------
	_check(bridge.load_level("TRAVERSE/LEVEL3/LEVEL3.DTI"),
		"Y: traversal level loads")
	_reset_audio()
	_check(bridge.load_arena("HMO_1"), "Y: spawn arena bound")
	_step_n({}, 2)
	var hud0: Dictionary = bridge.get_hud_snapshot()
	var hud_fb: PackedByteArray = hud0.get("fb", PackedByteArray())
	_check(hud_fb.size() == 216000, "Y: indexed HUD frame present")
	var pal0: PackedByteArray = bridge.get_active_palette()
	_check(pal0.size() == 768, "Y: staged palette present")
	var grab0 := int(fe_fx_counts.get(FE_FX_THUMB_GRAB, 0))
	snap = _fe_smoke_step({"f2": true}, 1)
	_check(int(snap.get("sub_mode", -1)) == 8,
		"Y: F2 arms the save-name dialog in traversal")
	_check(int(fe_fx_counts.get(FE_FX_THUMB_GRAB, 0)) == grab0 + 1,
		"Y: SaveNameThumbnailGrab fired once at arm")
	# Independent recomputation of FUN_00427e8c's sample formula over
	# the same indexed frame the bridge captured.
	var exp := PackedByteArray()
	exp.resize(3648)
	for i in 768:
		exp[i] = pal0[i]
	for r in 45:
		for c in 64:
			exp[768 + r * 64 + c] = hud_fb[r * 8 * 600 + 44 + c * 8]
	for ch in "TST1":
		_fe_smoke_step({"typed": ch.unicode_at(0)}, 1)
	snap = _fe_smoke_step({"confirm": true}, 1)
	_check(int(snap.get("sub_mode", -1)) == 0,
		"Y: name commit closed the dialog")
	var sav_abs2 := save_abs.path_join("TST1.SAV")
	_check(FileAccess.file_exists(sav_abs2),
		"Y: TST1.SAV written under the temp root")
	# The stream (file+8) ciphers from offset 10 with the clear seed at
	# stream 8..9: key = seed&0xff, byte ^= key, key += seed>>8.
	var file_bytes := FileAccess.get_file_as_bytes(sav_abs2)
	var fseed := int(file_bytes[16]) | (int(file_bytes[17]) << 8)
	var fk := fseed & 0xff
	var fdl := (fseed >> 8) & 0xff
	var plain := PackedByteArray()
	plain.resize(file_bytes.size() - 8)
	for i in plain.size():
		if i < 10:
			plain[i] = file_bytes[8 + i]
		else:
			plain[i] = int(file_bytes[8 + i]) ^ fk
			fk = (fk + fdl) & 0xff
	# THMB packet at stream offset 10: 8B head + 3648B record.
	_check(plain.slice(10, 14).get_string_from_ascii() == "THMB",
		"Y: THMB packet tag in the written stream")
	var file_thmb := plain.slice(18, 18 + 3648)
	_check(file_thmb == exp,
		"Y: captured THMB == bytes inside the written packet")
	var ins: Dictionary = bridge.frontend_inspect_slot("TST1")
	_check(bool(ins.get("found", false)) and
		bool(ins.get("full_save", false)),
		"Y: TST1 inspects as a full save")
	_check(int(ins.get("thumbnail_size", 0)) == 3648,
		"Y: inspection carries the 3648B THMB record")
	var th: PackedByteArray = ins.get("thumbnail", PackedByteArray())
	_check(th == exp,
		"Y: inspected thumbnail == captured bytes")
	var nz_px := 0
	for i in range(768, 3648):
		if int(th[i]) != 0:
			nz_px += 1
	_check(nz_px > 0, "Y: THMB pixel payload nonempty (%d)" % nz_px)
	# Abort YES -> fresh frontend; select TST1; the detail pane must
	# show the stored THMB at (0x1a2,0x67) expanded through the
	# merged palette (pens >= 64 -> the record's own entries).
	_fe_smoke_step({"f10": true}, 1)
	snap = _fe_smoke_step({"key_y": true}, 1)
	snap = _fe_smoke_step({}, 2)
	_check(int(bridge.get_mode()) == 0,
		"Y: abort YES returned to frontend mode")
	while int(snap.get("selection", -1)) != 2:
		snap = _fe_press("next")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 1,
		"Y: save list reopens for TST1")
	var gsel := 0
	while String(snap.get("save_list", {}).get(
			"selected", {}).get("name", "")) != "TST1" and gsel < 16:
		snap = _fe_press("next")
		gsel += 1
	var sel_name := String(snap.get("save_list", {}).get(
		"selected", {}).get("name", ""))
	_check(sel_name == "TST1", "Y: TST1 row selectable")
	var thfr: Dictionary = bridge.frontend_frame()
	var trgba: PackedByteArray = thfr["rgba"]
	# Byte-exact presentation compare over the full pen space: the
	# OBSERVED merge sends pens < 64 through SYS_PAL's head and pens
	# >= 64 through the record's own palette entries.
	var spal := _sys_pal_head()
	_check(spal.size() == 192, "Y: SYS_PAL head read for the merge")
	var all_alpha := true
	var ge64 := 0
	var pal_ok := true
	for r in 45:
		for c in 64:
			var o := ((0x67 + r) * 600 + (0x1a2 + c)) * 4
			if int(trgba[o + 3]) != 255:
				all_alpha = false
			var v := int(th[768 + r * 64 + c])
			var epal := th if v >= 64 else spal
			if v >= 64:
				ge64 += 1
			if int(trgba[o]) != int(epal[v * 3]) or \
					int(trgba[o + 1]) != int(epal[v * 3 + 1]) or \
					int(trgba[o + 2]) != int(epal[v * 3 + 2]):
				pal_ok = false
	_check(all_alpha, "Y: THMB rect (0x1a2,0x67) fully drawn")
	print("  Y: TST1 pen-space ge64=%d (HMO_1 spawn is dark)" % ge64)
	_check(pal_ok,
		"Y: presented pixels == saved THMB bytes via merged palette")
	var thmb_rect := _rect_hash(trgba, 0x1a2, 0x67, 64, 45)
	# Re-entering the list on a header-only row must swap the detail
	# — the stale-detail cache was cleared on exit.
	while String(snap.get("save_list", {}).get(
			"selected", {}).get("name", "")) != "SMOKE1" and gsel < 32:
		snap = _fe_press("prev")
		gsel += 1
	var sfr: Dictionary = bridge.frontend_frame()
	_check(_rect_hash(sfr["rgba"], 0x1a2, 0x67, 64, 45) != thmb_rect,
		"Y: header-only row drops the THMB detail (no stale cache)")
	# The header-only detail is LOAD_<level>.LBB centered on x=0x1c2:
	# corpus LBBs are 200x200 -> occupies (350,103)..(550,303).
	var lbb_det := _rect_hash(sfr["rgba"], 350, 103, 200, 200)
	var bg_zone := _rect_hash(sfr["rgba"], 560, 320, 30, 30)
	_check(lbb_det != bg_zone,
		"Y: LBB detail region carries decoded imagery")
	snap = _fe_press("cancel")

	# --- Z. transition: INTRO1A timeline, skip, swallow ------------
	_check(bridge.frontend_transition_seconds() == 10.0,
		"Z: INTRO1A decoded — 10s timeline live")
	var acks0 := fe_transition_acks
	bridge.frontend_enter(true)      # returning entry arms the window
	snap = _fe_smoke_step({}, 1)     # drains TransitionArmed -> ms 0
	_check(fe_transition_ms >= 0.0, "Z: transition window armed")
	_check(bool(snap.get("suppress_esc_abort", false)),
		"Z: Esc suppression held")
	# Phase digests (FUN_0041e554): fade-in 0-1s, hold 1-4s, crossfade
	# 4-6s, hold 6-9s, fade-out 9-10s. Holds are static; phases differ.
	var d_in := _frame_hash(500.0)
	var d_a1 := _frame_hash(2000.0)
	var d_a2 := _frame_hash(3500.0)
	var d_x := _frame_hash(5000.0)
	var d_b1 := _frame_hash(7000.0)
	var d_b2 := _frame_hash(8500.0)
	var d_out := _frame_hash(9500.0)
	_check(d_in != d_a1, "Z: fade-in mid != hold A")
	_check(d_a1 == d_a2, "Z: hold A static across the window")
	_check(d_x != d_a1 and d_x != d_b1,
		"Z: crossfade mid distinct from both holds")
	_check(d_b1 == d_b2, "Z: hold B static across the window")
	_check(d_out != d_b1 and d_out != d_a1,
		"Z: fade-out mid distinct from the holds")
	_check(d_b1 != menu_hash or d_a1 != menu_hash,
		"Z: transition still is INTRO1A, not the menu")
	# Input contract: a held-level field is NOT an edge — it is
	# swallowed while the transition owns the frame and neither skips
	# nor reaches the menu pump (0x49aa7c routes the frame elsewhere).
	sel_before = int(bridge.frontend_snapshot().get("selection", -1))
	snap = _fe_smoke_step({"next": true}, 1)
	_check(bool(snap.get("suppress_esc_abort", false)),
		"Z: held-level input does not skip the transition")
	_check(int(snap.get("selection", -1)) == sel_before,
		"Z: swallowed input never reached the menu pump")
	# A key edge skips AND is consumed — cancel would otherwise arm
	# the abort console on this same frame.
	snap = _fe_smoke_step({"cancel": true}, 1)
	_check(fe_transition_ms < 0.0 and
		not bool(snap.get("suppress_esc_abort", true)),
		"Z: key edge completed the transition")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"Z: skip edge swallowed — no abort arm")
	_check(fe_transition_acks == acks0 + 1,
		"Z: frontend_transition_complete ran exactly once")
	# Full timeline run: re-arm and let the clock finish it.
	bridge.frontend_enter(true)
	_fe_smoke_step({}, 1)
	_check(fe_transition_ms >= 0.0, "Z: second window armed")
	var tguard := 0
	while fe_transition_ms >= 0.0 and tguard < 320:
		snap = _fe_smoke_step({}, 1)
		tguard += 1
	_check(fe_transition_ms < 0.0, "Z: timeline ran to completion")
	_check(tguard >= 280 and tguard <= 320,
		"Z: ~10s playback (%d 33ms steps)" % tguard)
	_check(fe_transition_acks == acks0 + 2,
		"Z: natural completion acked exactly once more")
	_check(not bool(snap.get("suppress_esc_abort", true)),
		"Z: suppression cleared at completion")
	# Post-clear the elapsed-ms argument is inert: suppression is
	# gone, so ms=5000 can no longer route to the transition still —
	# the result must match NONE of the captured phase digests. (An
	# exact menu-hash compare would be flaky: the root selection
	# bracket advances its blink phase inside each compose.)
	var zlate := _frame_hash(5000.0)
	_check(zlate != d_in and zlate != d_a1 and zlate != d_x and
		zlate != d_b1 and zlate != d_out,
		"Z: post-transition compose ignores the ms arg")

	# --- X. bounded-resource cycles -------------------------------
	# Repeated root->attract->list->root rounds must reproduce
	# identical digests (decoded resources reuse, no growth). The
	# attract edge runs at the root (it is inert inside sub-modes);
	# confirm under attract dispatches through the menu per
	# FUN_0041dc90 (no key exits attract — state clears on the
	# list-exit re-entry).
	var cyc_slides := []
	var cyc_lists := []
	var cyc_details := []
	for i in 3:
		bridge.frontend_enter(false)
		_fe_smoke_step({}, 2)
		snap = _fe_smoke_step({"attract": true}, 1)
		_check(int(snap.get("attract_state", -1)) == 1,
			"X: cycle %d attract state 1" % i)
		cyc_slides.append(_frame_hash())
		while int(snap.get("selection", -1)) != 2:
			snap = _fe_press("next")
		snap = _fe_press("confirm")
		_check(int(snap.get("sub_mode", -1)) == 1,
			"X: cycle %d list opened" % i)
		var lf: Dictionary = bridge.frontend_frame()
		# The selected row's bracket carries the blink phase (a
		# 2-state alternator), so the whole-frame digest is bound to
		# a 2-value set; the decoded detail region is phase-free and
		# must be byte-identical every cycle.
		cyc_lists.append(_ba_hash(lf["rgba"]))
		cyc_details.append(_rect_hash(
			lf["rgba"], 350, 103, 200, 200))
		snap = _fe_press("cancel")
	_check(cyc_slides[0] == cyc_slides[1] and
		cyc_slides[1] == cyc_slides[2],
		"X: attract slide compose identical across cycles")
	var cyc_list_set := {}
	for h in cyc_lists:
		cyc_list_set[h] = true
	_check(cyc_list_set.size() <= 2,
		"X: save-list compose stable modulo the blink phase")
	_check(cyc_details[0] == cyc_details[1] and
		cyc_details[1] == cyc_details[2],
		"X: LBB detail imagery identical across cycles")
	# Presentation texture reuse — the single ImageTexture is updated
	# in place, never re-allocated per frame.
	_frontend_present()
	var tex0 = fe_tex
	_frontend_present()
	_check(fe_tex == tex0, "X: fe_tex object reused across presents")

	# fx coverage diagnostics — all bounded.
	print("smoke: fe_fx=", fe_fx_counts, " fe_req=", fe_req_counts)
	print("smoke(frontend): %d failure(s)" % failures)


func _smoke_frontend_frame_check() -> bool:
	var fr: Dictionary = bridge.frontend_frame()
	return int(fr.get("w", 0)) == 600 and int(fr.get("h", 0)) == 360


# ---------------------------------------------------------------------------
# Phase 18B.2A §24 — real-save corpus read-only check. Boot points the
# frontend store at the installed SAVES dir; the scenario only ever calls
# read seams (enumerate / inspect / slide probe / frame compose) and
# fingerprints the directory before and after to prove nothing was
# written.
# ---------------------------------------------------------------------------
func _run_smoke_real_saves() -> void:
	print("smoke: real saves (read-only)")
	var saves_dir := data_root_path.path_join("SAVES")
	var before := {}
	var da := DirAccess.open(saves_dir)
	_check(da != null, "R: real SAVES dir opens")
	if da != null:
		for f in da.get_files():
			before[f] = FileAccess.get_modified_time(
				saves_dir.path_join(f))

	_check(bridge.frontend_booted(), "R: frontend booted")
	var snap := _fe_smoke_step({}, 2)
	_check(int(snap.get("mode", -1)) == 0, "R: mode == frontend")

	# Host enumeration over the real dir — raw "*.SAV" names.
	var names: Array = bridge.frontend_enumerate_saves()
	_check(names.size() >= 2, "R: enumeration sees the corpus")
	_check(names.has("1.SAV") and names.has("2.SAV"),
		"R: 1.SAV + 2.SAV enumerated")

	# The full save: head + THMB (768-byte palette + 64x45 indexed).
	var full: Dictionary = bridge.frontend_inspect_slot("1")
	_check(bool(full.get("found", false)), "R: full slot found")
	_check(bool(full.get("valid", false)), "R: full slot valid")
	_check(bool(full.get("full_save", false)),
		"R: full-save classification")
	_check(int(full.get("thumbnail_size", 0)) == 3648,
		"R: full save carries the THMB record")
	print("R: 1.SAV level=", full.get("level_id"),
		" mode=", full.get("mode_field"),
		" health=", full.get("health"))

	# The header-only save: metadata only — LOAD_<level>.LBB is an
	# unknown format; no imagery is invented for it.
	var hdr: Dictionary = bridge.frontend_inspect_slot("2")
	_check(bool(hdr.get("found", false)), "R: header slot found")
	_check(bool(hdr.get("valid", false)), "R: header slot valid")
	_check(not bool(hdr.get("full_save", true)),
		"R: header-only classification")
	print("R: 2.SAV level=", hdr.get("level_id"),
		" mode=", hdr.get("mode_field"))

	# The real corpus carries no LASTGAME.SAV — the probe is false
	# and the default selection is New Game (1), matching the
	# no-saves contract verified in scenario A.
	_check(not bool(snap.get("saves_exist", true)),
		"R: no LASTGAME -> saves_exist false")
	_check(int(snap.get("selection", -1)) == 1,
		"R: New Game is the default selection")

	# The save list sees the real stems through the menu route.
	snap = _fe_press("next")                   # 1 -> 2
	_check(int(snap.get("selection", -1)) == 2,
		"R: Saved Game item selected")
	snap = _fe_press("confirm")
	_check(int(snap.get("sub_mode", -1)) == 1,
		"R: save list opens over the real corpus")
	var sl: Dictionary = snap.get("save_list", {})
	var stems: Array = sl.get("stems", [])
	_check(stems.has("1") and stems.has("2"),
		"R: shell list carries stems 1 + 2")

	# --- R2. real-save detail imagery through the live path -------
	# Row 0 = "1" (full): the stored THMB record blits at
	# (0x1a2,0x67); pens >= 64 expand through the record's own
	# palette entries (the OBSERVED 64..255 merge).
	var rsel := 0
	while int(snap.get("save_list", {}).get("selection", -1)) != 0 \
			and rsel < 8:
		snap = _fe_press("prev")
		rsel += 1
	var sel_sum: Dictionary = snap.get("save_list", {}).get(
		"selected", {})
	_check(String(sel_sum.get("name", "")) == "1",
		"R2: row 0 is the full save")
	var r1fr: Dictionary = bridge.frontend_frame()
	var r1rgba: PackedByteArray = r1fr["rgba"]
	var th1: PackedByteArray = full.get("thumbnail", PackedByteArray())
	_check(th1.size() == 3648, "R2: 1.SAV THMB bytes available")
	# Full pen-space compare: <64 -> SYS_PAL head, >=64 -> the
	# record's own palette entries (the OBSERVED merge).
	var spal1 := _sys_pal_head()
	var all_a := true
	var ge64b := 0
	var pal_ok1 := true
	for r in 45:
		for c in 64:
			var o := ((0x67 + r) * 600 + (0x1a2 + c)) * 4
			if int(r1rgba[o + 3]) != 255:
				all_a = false
			var v := int(th1[768 + r * 64 + c])
			var epal := th1 if v >= 64 else spal1
			if v >= 64:
				ge64b += 1
			if int(r1rgba[o]) != int(epal[v * 3]) or \
					int(r1rgba[o + 1]) != int(epal[v * 3 + 1]) or \
					int(r1rgba[o + 2]) != int(epal[v * 3 + 2]):
				pal_ok1 = false
	_check(all_a, "R2: 1.SAV THMB rect fully drawn")
	_check(ge64b > 0,
		"R2: preview pens reach the merged band (%d)" % ge64b)
	_check(pal_ok1,
		"R2: presented pixels == stored THMB bytes")
	# Row 1 = "2" (header-only): levelId 1 -> LOAD_6.LBB (the
	# 0x4999e8 table), centered at (0x1c2 - w/2, 0x67) = (350,103).
	_check(int(hdr.get("level_id", -1)) == 1,
		"R2: 2.SAV levelId==1 -> LOAD_6")
	snap = _fe_press("next")
	_check(int(snap.get("save_list", {}).get("selection", -1)) == 1,
		"R2: row 1 selected")
	var lbb_b := FileAccess.get_file_as_bytes(
		data_root_path.path_join("MISC/LOAD_6.LBB"))
	_check(lbb_b.size() == 40772, "R2: LOAD_6.LBB is 40772B")
	var lw := int(lbb_b[768]) | (int(lbb_b[769]) << 8)
	var lh := int(lbb_b[770]) | (int(lbb_b[771]) << 8)
	_check(lw == 200 and lh == 200, "R2: LBB dims 200x200")
	var lx0 := 0x1c2 - lw / 2
	var r2rgba: PackedByteArray = bridge.frontend_frame()["rgba"]
	var lbb_matched := 0
	var lbb_ge64 := 0
	for r in lh:
		for c in lw:
			var v := int(lbb_b[772 + r * lw + c])
			var epal := lbb_b if v >= 64 else spal1
			if v >= 64:
				lbb_ge64 += 1
			var o := ((0x67 + r) * 600 + (lx0 + c)) * 4
			if int(r2rgba[o]) == int(epal[v * 3]) and \
					int(r2rgba[o + 1]) == int(epal[v * 3 + 1]) and \
					int(r2rgba[o + 2]) == int(epal[v * 3 + 2]):
				lbb_matched += 1
	_check(lbb_ge64 > 1000,
		"R2: LBB image has pens in the merged band (%d)" % lbb_ge64)
	_check(lbb_matched == lh * lw,
		"R2: LBB pixels presented byte-exact through the merge")
	snap = _fe_press("cancel")
	_check(int(snap.get("sub_mode", -1)) == 0,
		"R: Esc exits the list")

	# Attract-slide probe — read-only MISC\MDKS_001.GIF gate check.
	# Full decode/compose coverage lives in the main frontend smoke
	# (the V scenario walks the whole corpus).
	var probe: Dictionary = bridge.frontend_slide_probe(1)
	_check(bool(probe.get("exists", false)),
		"R: MDKS_001 slide probe (600x360 gate)")

	# The composed frame still forms over the real data.
	var fr: Dictionary = bridge.frontend_frame()
	_check(int(fr.get("w", 0)) == 600 and int(fr.get("h", 0)) == 360,
		"R: composed frame is 600x360")

	# Fingerprint after — identical names and mtimes prove the whole
	# scenario never wrote.
	var after := {}
	var da2 := DirAccess.open(saves_dir)
	if da2 != null:
		for f in da2.get_files():
			after[f] = FileAccess.get_modified_time(
				saves_dir.path_join(f))
	_check(after == before, "R: SAVES dir untouched (read-only)")
	print("smoke(real-saves): %d failure(s)" % failures)
