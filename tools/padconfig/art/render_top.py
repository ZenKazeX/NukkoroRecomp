# Blender (background): the controller from above for the Settings program (tools/padconfig/art), in layers.
#   blender -b --factory-startup --python render_top.py -- <model dir> <out dir> [samples] [all|full|layers|sticks]
# <model dir>: the GameCube controller model (source/Gamecube_controller_mesh.zip, textures/*.png); nothing in it is
# changed. Renders top_full.png (art/top.png), top_body.png (art/top_body.png: without the two stick caps) and
# top_cap_main.png / top_cap_c.png (each cap alone; cropped to their box they are art/top_cap_*.png, the boxes are
# kStickCaps in art/hotspots.inc), all 1000 x 659 with a transparent background, the same camera and light. Blender 4.5,
# Cycles on the CPU: about 90 s. The camera is the earlier art/top.png's (found by matching its silhouette), so the
# button masks still fit.
import bpy, sys, os, math, tempfile, zipfile
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
model, out = argv[0], argv[1]
samples = int(argv[2]) if len(argv) > 2 else 128
only = argv[3] if len(argv) > 3 else "all"
os.makedirs(out, exist_ok=True)
textures = os.path.join(model, "textures")

bpy.ops.wm.read_factory_settings(use_empty=True)
unpacked = tempfile.mkdtemp()
with zipfile.ZipFile(os.path.join(model, "source", "Gamecube_controller_mesh.zip")) as z:
    z.extractall(unpacked)
bpy.ops.wm.obj_import(filepath=os.path.join(unpacked, "GameCube_Controller_OBJ_High.obj"))
mesh = [o for o in bpy.context.scene.objects if o.type == 'MESH'][0]

# The model's textures (the OBJ's MTL file is not shipped): base colour, normal map, roughness.
mat = bpy.data.materials.new("Controller")
mat.use_nodes = True
nt = mat.node_tree
bsdf = nt.nodes["Principled BSDF"]
def tex(name, non_color):
    n = nt.nodes.new("ShaderNodeTexImage")
    n.image = bpy.data.images.load(os.path.join(textures, "Gamecube_controller_mesh_GameCube_Mat_" + name + ".png"))
    if non_color:
        n.image.colorspace_settings.name = "Non-Color"
    return n
base = tex("Base", False)
# No brand logo on the pictures: the logo printed on the texture (pixels 661..857 x 1368..1469 of the 2048 x 2048 base
# colour, on plain purple) is painted over with that purple, in a copy.
img = base.image
w, h = img.size
px = list(img.pixels[:])
purple = (90 / 255.0, 70 / 255.0, 129 / 255.0, 1.0)
for y in range(1368, 1470):
    row = (h - 1 - y) * w  # Blender's rows start at the bottom
    for x in range(661, 858):
        px[(row + x) * 4:(row + x) * 4 + 4] = purple
img.pixels[:] = px
# Saved beside the unpacked model (a temporary folder) and used from there: the renderer reads the file.
img.filepath_raw = os.path.join(unpacked, "base_without_logo.png")
img.file_format = 'PNG'
img.save()
img.reload()
nt.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
nt.links.new(tex("Roug", True).outputs["Color"], bsdf.inputs["Roughness"])
nm = nt.nodes.new("ShaderNodeNormalMap")
nt.links.new(tex("Norm", True).outputs["Color"], nm.inputs["Color"])
nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
mesh.data.materials.clear()
mesh.data.materials.append(mat)
for p in mesh.data.polygons:
    p.use_smooth = True

# Split into its loose parts and name the two stick caps by where they are.
bpy.context.view_layer.objects.active = mesh
mesh.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.separate(type='LOOSE')
bpy.ops.object.mode_set(mode='OBJECT')
def centre(o):
    cs = [o.matrix_world @ v.co for v in o.data.vertices]
    return sum(cs, Vector()) / len(cs)
caps = {}
for o in bpy.context.scene.objects:
    if o.type != 'MESH':
        continue
    c = centre(o)
    size = max(o.dimensions)
    if abs(c.x + 15.0) < 1.0 and abs(c.y) < 1.0 and 7.0 < size < 9.0:
        caps["main"] = o
    elif abs(c.x - 7.57) < 1.0 and abs(c.y + 11.25) < 1.0 and 7.0 < size < 9.0:
        caps["c"] = o
assert set(caps) == {"main", "c"}, caps

scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.samples = samples
scene.cycles.use_denoising = True
scene.cycles.device = 'CPU'
scene.render.threads_mode = 'FIXED'
scene.render.threads = 4
scene.render.film_transparent = True
scene.render.resolution_x, scene.render.resolution_y = 1000, 659
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = 'Standard'
scene.view_settings.look = 'None'

# The camera of top.png, found by matching its silhouette (fit_camera.py): straight down from 195.5 units over
# (0.08, -4.19), 19.35 pixels per unit at height 22.
cam_data = bpy.data.cameras.new("Top")
cam_data.sensor_fit = 'HORIZONTAL'
cam_data.sensor_width = 36.0
cam_data.lens = 19.35 * (195.5 - 22.0) / 1000.0 * 36.0
cam = bpy.data.objects.new("Top", cam_data)
scene.collection.objects.link(cam)
cam.location = (0.08, -4.19, 195.5)
cam.rotation_euler = (0.0, 0.0, 0.0)
scene.camera = cam

# Light: a soft studio key from the upper left and front, a fill, and a grey world.
# Chosen by comparing renders with the earlier art/top.png: the world's strength, the key, fill, rim and overhead
# energies, how cool the world is; the plastic's specular level, the lights' colour and the exposure.
LIGHT = [0.05, 8000.0, 500.0, 600.0, 26000.0, 1.2]
TINT = (0.86, 0.9, 1.0)
scene.view_settings.exposure = 0.35
bsdf.inputs["Specular IOR Level"].default_value = 0.25
world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.8 - 0.1 * LIGHT[5], 0.8 - 0.05 * LIGHT[5], 0.82 + 0.15 * LIGHT[5], 1.0)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = LIGHT[0]
def area(name, loc, energy, size):
    d = bpy.data.lights.new(name, 'AREA')
    d.energy = energy
    d.size = size
    d.color = TINT
    o = bpy.data.objects.new(name, d)
    scene.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = (Vector((0, 0, 0)) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    return o
area("Key", (-30.0, -25.0, 60.0), LIGHT[1], 30.0)
area("Fill", (35.0, -10.0, 45.0), LIGHT[2], 40.0)
area("Rim", (0.0, 40.0, 40.0), LIGHT[3], 30.0)
if LIGHT[4] > 0:
    area("Overhead", (0.0, -4.0, 90.0), LIGHT[4], 70.0)  # a big softbox above: the faces towards the camera

def render(name, hide=(), holdout=()):
    for o in scene.objects:
        if o.type == 'MESH':
            o.hide_render = o in hide
            o.is_holdout = o in holdout
    scene.render.filepath = os.path.join(out, name)
    bpy.ops.render.render(write_still=True)
    print("WROTE", scene.render.filepath)

others = [o for o in scene.objects if o.type == 'MESH' and o not in caps.values()]
# ---- the sticks as a virtual controller: a bone at each stick's root, the cap tilted on it ----------------------------
# "sticks": each cap alone (the rest held out, so the shell hides what tilts under it), turned on a bone whose head is
# the stick's root, PIVOT_DEPTH units under the cap, as a GameCube stick moves: inside an octagonal gate with its
# corners (the notches) at the 4 directions and the 4 diagonals, up to MOST_TILT degrees at a notch (the sticks' parts
# allow about +-30). Rendered for the middle and for RINGS distances (1/RINGS .. 1 of the way to the gate) in each of
# DIRECTIONS directions (every 22.5 degrees from the right, anticlockwise: the notches and the gate's edges between them),
# only in the area round the stick: sticks/<main|c>_0.png (the middle) and sticks/<main|c>_<ring>_<direction>.png.
DIRECTIONS, RINGS, MOST_TILT, PIVOT_DEPTH = 16, 4, 25.0, 4.0

def gate(angle):
    # How far the octagonal gate lets the stick go in a direction, as a fraction of the way to a notch.
    sector = math.radians(22.5)
    off = (angle % math.radians(45.0)) - sector
    return math.cos(sector) / math.cos(off)
# Where each stick is in the picture, as fractions of its width and height from the top left (the area rendered).
STICK_AREA = {"main": (0.066, 0.158, 0.356, 0.598), "c": (0.496, 0.481, 0.788, 0.924)}

def stick_frames():
    from mathutils import Matrix, Quaternion
    arm_data = bpy.data.armatures.new("Sticks")
    rig = bpy.data.objects.new("Sticks", arm_data)
    scene.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode='EDIT')
    for key, cap in caps.items():
        lo = min((cap.matrix_world @ v.co).z for v in cap.data.vertices)
        c = centre(cap)
        bone = arm_data.edit_bones.new(key)
        bone.head = (c.x, c.y, lo - PIVOT_DEPTH)  # the stick's root
        bone.tail = (c.x, c.y, lo)
    bpy.ops.object.mode_set(mode='OBJECT')
    for key, cap in caps.items():
        world = cap.matrix_world.copy()
        cap.parent = rig
        cap.parent_type = 'BONE'
        cap.parent_bone = key
        cap.matrix_world = world  # stays where it is; follows the bone from now on
    rig.hide_render = True
    os.makedirs(os.path.join(out, "sticks"), exist_ok=True)
    scene.render.use_border = True
    scene.render.use_crop_to_border = True
    for key, cap in caps.items():
        x0, y0, x1, y1 = STICK_AREA[key]
        scene.render.border_min_x, scene.render.border_max_x = x0, x1
        scene.render.border_min_y, scene.render.border_max_y = 1.0 - y1, 1.0 - y0
        for o in scene.objects:
            if o.type == 'MESH':
                o.hide_render = o in caps.values() and o is not cap
                o.is_holdout = o in others
        pose = rig.pose.bones[key]
        pose.rotation_mode = 'QUATERNION'
        rest = rig.data.bones[key].matrix_local.to_3x3()
        poses = [("%s_0.png" % key, 0.0, 0.0)]
        for ring in range(1, RINGS + 1):
            for direction in range(DIRECTIONS):
                angle = math.radians(direction * 360.0 / DIRECTIONS)
                poses.append(("%s_%d_%d.png" % (key, ring, direction), angle, ring / RINGS * gate(angle)))
        for name, angle, length in poses:
            if length > 0.0:
                # Pushed that way the stick's top leans that way: a turn about the axis across it.
                dx, dy = math.cos(angle), math.sin(angle)
                turn = Matrix.Rotation(math.radians(MOST_TILT * length), 3, Vector((-dy, dx, 0.0)))
                pose.rotation_quaternion = (rest.inverted() @ turn @ rest).to_quaternion()
            else:
                pose.rotation_quaternion = Quaternion()
            bpy.context.view_layer.update()
            scene.render.filepath = os.path.join(out, "sticks", name)
            bpy.ops.render.render(write_still=True)
        pose.rotation_quaternion = Quaternion()
    print("WROTE sticks")
if only in ("all", "full"):
    render("top_full.png")
if only == "sticks":
    stick_frames()
if only in ("all", "layers"):
    render("top_body.png", hide=list(caps.values()))
    render("top_cap_main.png", hide=[caps["c"]], holdout=others)
    render("top_cap_c.png", hide=[caps["main"]], holdout=others)
