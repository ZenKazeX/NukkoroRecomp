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

# A little shine on the sticks (their own copy of the plastic): a stronger highlight and a thin clear coat, so the light
# runs over a cap as it tilts.
SHINE = {"Specular IOR Level": 0.6, "Coat Weight": 0.25, "Coat Roughness": 0.15}
def shiny(material):
    for name, value in SHINE.items():
        material.node_tree.nodes["Principled BSDF"].inputs[name].default_value = value
    return material
stick_mat = shiny(mat.copy())
stick_mat.name = "Stick"
for cap in caps.values():
    cap.data.materials.clear()
    cap.data.materials.append(stick_mat)

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
# "sticks": each cap alone, turned on a bone whose head is
# the stick's root: the centre of the dome under the cap (the part that fills the gate; a sphere round the pivot, so it
# rolls in place as on a real controller and the gate never opens; found by fitting a sphere to it), as a GameCube
# stick moves: inside an octagonal gate with its
# corners (the notches) at the 4 directions and the 4 diagonals, up to MOST_TILT degrees at a notch (the sticks' parts
# allow about +-30). Rendered for the middle and for RINGS distances (1/RINGS .. 1 of the way to the gate) in each of
# DIRECTIONS directions (every 22.5 degrees from the right, anticlockwise: the notches and the gate's edges between them),
# only in the area round the stick: sticks/<main|c>_0.png (the middle) and sticks/<main|c>_<ring>_<direction>.png. The
# rest of the controller is a shadow catcher: not drawn, but it hides what tilts under the shell, and the cap's shadow
# on it is in the frame (drawn over the body without caps, the stick casts its shadow as it moves).
# Each frame in two layers: <key>_<ring>_<direction>.png the dome and the shadows, cap_<...>.png the cap alone; and
# sticks/tops.txt. "sticks-test": a few of those frames only. "sticks-tops": no render; writes sticks/tops.txt, where the top of the cap
# (its middle, at its highest point) is in the picture's pixels in each frame, for art/hotspots.inc (kStickTops).
CUT_Z = {"main": 21.3, "c": 19.55}  # just above each skirt (it ends at 21.15 and 19.38; the stems go on to 22.5 and 20.8)
STEM_R = {"main": 1.4, "c": 1.68}  # the stems' radii
DIRECTIONS, RINGS, MOST_TILT = 16, 4, 25.0

def dome_centre(cap):
    # The centre of the sphere the cap's dome lies on: a least-squares fit to the outer surface of its lowest quarter.
    import numpy as np
    pts = np.array([list(cap.matrix_world @ v.co) for v in cap.data.vertices])
    lo, hi = pts[:, 2].min(), pts[:, 2].max()
    mid = pts.mean(axis=0)
    r = np.hypot(pts[:, 0] - mid[0], pts[:, 1] - mid[1])
    sel = (pts[:, 2] < lo + 0.25 * (hi - lo)) & (r > 0.5 * r.max())
    A = np.c_[2 * pts[sel], np.ones(int(sel.sum()))]
    sol = np.linalg.lstsq(A, (pts[sel] ** 2).sum(axis=1), rcond=None)[0]
    centre = Vector(sol[:3])
    return centre, math.sqrt(sol[3] + centre.length_squared), lo

# The rest of each dome, which the model leaves out: a real stick's dome reaches on under the shell, so a tilted stick
# still fills its gate. The colour of each cap's plastic (sRGB 0..255) and how far down the dome goes (degrees from
# its top).
DOME_COLOUR = {"main": (178, 189, 198), "c": (236, 174, 12)}
DOME_REACH = 72.0

def complete_dome(key, root, radius, cap_bottom):
    import bmesh
    me = bpy.data.meshes.new("dome_" + key)
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=96, v_segments=64, radius=radius - 0.03)
    lowest = root.z + (radius - 0.03) * math.cos(math.radians(DOME_REACH)) - root.z
    keep_top = cap_bottom + 0.8 - root.z  # tucked up under the model's own dome (just inside it): no seam between them
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if v.co.z > keep_top or v.co.z < lowest], context="VERTS")
    bm.to_mesh(me)
    bm.free()
    dome = bpy.data.objects.new("dome_" + key, me)
    scene.collection.objects.link(dome)
    dome.location = root
    for p in me.polygons:
        p.use_smooth = True
    mat = bpy.data.materials.new("Dome_" + key)
    mat.use_nodes = True
    b = mat.node_tree.nodes["Principled BSDF"]
    srgb = [c / 255.0 for c in DOME_COLOUR[key]]
    linear = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in srgb]
    b.inputs["Base Color"].default_value = (*linear, 1.0)
    b.inputs["Roughness"].default_value = 0.45
    me.materials.append(shiny(mat))
    bpy.context.view_layer.update()  # its matrix_world, before it is parented
    return dome

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
    domes_wanted = {}
    bpy.ops.object.mode_set(mode='EDIT')
    for key, cap in caps.items():
        root, radius, bottom = dome_centre(cap)
        domes_wanted[key] = (root.copy(), radius, bottom)
        bone = arm_data.edit_bones.new(key)
        bone.head = root  # the stick's root
        bone.tail = root + Vector((0.0, 0.0, 4.0))
        print("ROOT", key, tuple(round(v, 2) for v in root))
    bpy.ops.object.mode_set(mode='OBJECT')
    domes = {}
    for key, cap in caps.items():
        dome = complete_dome(key, *domes_wanted[key])
        domes[key] = dome
        for part in (cap, dome):
            world = part.matrix_world.copy()
            part.parent = rig
            part.parent_type = 'BONE'
            part.parent_bone = key
            part.matrix_world = world  # stays where it is; follows the bone from now on
    rig.hide_render = True
    os.makedirs(os.path.join(out, "sticks"), exist_ok=True)
    # The top of each cap, in the cap's own coordinates: its middle, at its highest point.
    top_local, tops = {}, []
    for key, cap in caps.items():
        # The middle of the top disc (the vertices within 0.3 units of the highest), measured in the world at rest (the
        # import turned the mesh's own axes): the mesh's average would lean to whichever side has more vertices.
        world = [cap.matrix_world @ v.co for v in cap.data.vertices]
        hi = max(c.z for c in world)
        disc = [c for c in world if c.z > hi - 0.3]
        top = Vector(((min(c.x for c in disc) + max(c.x for c in disc)) / 2.0,
                      (min(c.y for c in disc) + max(c.y for c in disc)) / 2.0, hi))
        top_local[key] = cap.matrix_world.inverted() @ top
        print("TOP", key, tuple(round(v, 2) for v in top), len(disc), "vertices")
    # Each cap in two parts, cut across its stem just above the flared skirt it stands on (model units above the root at
    # rest): the skirt, wider than the top and meeting the dome, goes into the dome layer, which is drawn where it is;
    # the stem and the top into the cap layer, which is moved onto the stick's top. Seen from above, the top covers the cut.
    # A material each that shows one part (the other transparent): object coordinates, so the cut turns with the cap.
    part_mats = {}
    for key, cap in caps.items():
        inverse = cap.matrix_world.inverted()
        point = inverse @ Vector((domes_wanted[key][0].x, domes_wanted[key][0].y, CUT_Z[key]))
        normal = (inverse.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized()
        part_mats[key] = {}
        for part in ("skirt", "top"):
            m = stick_mat.copy()
            m.name = "Stick %s %s" % (key, part)
            nodes, links = m.node_tree.nodes, m.node_tree.links
            out_node = nodes["Material Output"]
            surface = out_node.inputs["Surface"].links[0].from_socket
            coords = nodes.new("ShaderNodeTexCoord")
            dot = nodes.new("ShaderNodeVectorMath")
            dot.operation = "DOT_PRODUCT"
            dot.inputs[1].default_value = normal
            links.new(coords.outputs["Object"], dot.inputs[0])
            above = nodes.new("ShaderNodeMath")
            above.operation = "GREATER_THAN"
            links.new(dot.outputs["Value"], above.inputs[0])
            above.inputs[1].default_value = normal.dot(point)
            mix = nodes.new("ShaderNodeMixShader")
            # Cut away: transparent in the dome layer (the top above the skirt); a holdout in the cap layer, so nothing behind
            # the skirt (the shadow inside it) shows through.
            clear = nodes.new("ShaderNodeBsdfTransparent" if part == "skirt" else "ShaderNodeHoldout")
            links.new(above.outputs["Value"], mix.inputs["Fac"])
            # Fac 1 (above the cut) takes the second shader.
            links.new(surface if part == "skirt" else clear.outputs[0], mix.inputs[1])
            links.new(clear.outputs[0] if part == "skirt" else surface, mix.inputs[2])
            links.new(mix.outputs[0], out_node.inputs["Surface"])
            part_mats[key][part] = m
    # The cut stem is open: in the dome layer a lid closes it, so where the cap layer is moved a little off its frame the
    # stick's own colour shows round the stem, not the dark inside.
    lids = {}
    for key in caps:
        root = domes_wanted[key][0]
        bpy.ops.mesh.primitive_circle_add(vertices=48, fill_type="NGON", radius=STEM_R[key] + 0.02,
                                          location=(root.x, root.y, CUT_Z[key]))
        lid = bpy.context.active_object
        lid.data.materials.append(domes[key].data.materials[0])  # the dome's: the plastic's plain colour
        lid.visible_shadow = False
        world = lid.matrix_world.copy()
        lid.parent, lid.parent_type, lid.parent_bone = rig, "BONE", key
        lid.matrix_world = world
        lids[key] = lid
    scene.render.use_border = True
    scene.render.use_crop_to_border = True
    for key, cap in caps.items():
        x0, y0, x1, y1 = STICK_AREA[key]
        scene.render.border_min_x, scene.render.border_max_x = x0, x1
        scene.render.border_min_y, scene.render.border_max_y = 1.0 - y1, 1.0 - y0
        for o in scene.objects:
            if o.type == 'MESH':
                o.hide_render = (o in caps.values() and o is not cap) or (o in domes.values() and o is not domes[key])
                o.is_holdout = False
                o.is_shadow_catcher = o in others and o not in domes.values()
        pose = rig.pose.bones[key]
        pose.rotation_mode = 'QUATERNION'
        rest = rig.data.bones[key].matrix_local.to_3x3()
        poses = [("%s_0.png" % key, 0.0, 0.0)]
        for ring in range(1, RINGS + 1):
            for direction in range(DIRECTIONS):
                angle = math.radians(direction * 360.0 / DIRECTIONS)
                poses.append(("%s_%d_%d.png" % (key, ring, direction), angle, ring / RINGS * gate(angle)))
        if only == "sticks-test":
            poses = [p for p in poses if p[0] in ("%s_0.png" % key, "%s_4_0.png" % key, "%s_4_2.png" % key, "%s_4_6.png" % key, "%s_2_12.png" % key)]
        for name, angle, length in poses:
            if length > 0.0:
                # Pushed that way the stick's top leans that way: a turn about the axis across it.
                dx, dy = math.cos(angle), math.sin(angle)
                turn = Matrix.Rotation(math.radians(MOST_TILT * length), 3, Vector((-dy, dx, 0.0)))
                pose.rotation_quaternion = (rest.inverted() @ turn @ rest).to_quaternion()
            else:
                pose.rotation_quaternion = Quaternion()
            bpy.context.view_layer.update()
            from bpy_extras.object_utils import world_to_camera_view
            p = world_to_camera_view(scene, scene.camera, cap.matrix_world @ top_local[key])
            tops.append("%s %s %.3f %.3f" % (key, name[:-4], p.x * scene.render.resolution_x,
                                             (1.0 - p.y) * scene.render.resolution_y))
            if only == "sticks-tops":
                continue
            # Two layers, so the program can move the cap onto the stick's top while the dome below stays where it is:
            # the dome and the cap's skirt with their shadows, then the cap's stem and top with theirs (the dome a
            # holdout: where it is in front, the dome layer shows through).
            for layer in ("dome", "cap"):
                cap.data.materials[0] = part_mats[key]["top" if layer == "cap" else "skirt"]
                for k, lid in lids.items():
                    lid.hide_render = k != key or layer == "cap"
                domes[key].is_holdout = layer == "cap"
                domes[key].visible_shadow = layer == "dome"
                scene.render.filepath = os.path.join(out, "sticks", name if layer == "dome" else "cap_" + name)
                bpy.ops.render.render(write_still=True)
            domes[key].is_holdout = False
            domes[key].visible_shadow = True
            cap.data.materials[0] = stick_mat
            cap.visible_camera = domes[key].visible_camera = True
        pose.rotation_quaternion = Quaternion()
    with open(os.path.join(out, "sticks", "tops.txt"), "w") as fh:
        fh.write("\n".join(tops) + "\n")
    print("WROTE sticks")
if only in ("all", "full"):
    render("top_full.png")
if only in ("sticks", "sticks-test", "sticks-tops"):
    stick_frames()
if only in ("all", "layers"):
    render("top_body.png", hide=list(caps.values()))
    render("top_cap_main.png", hide=[caps["c"]], holdout=others)
    render("top_cap_c.png", hide=[caps["main"]], holdout=others)
