#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pipe_fbx2iqm.py  --  deterministic FBX -> IQM pipeline for E:/UZDXREMA (UZDoom/GZDoom VR fork)

Two run modes:

  BUILD    "<blender.exe>" -b --factory-startup --python pipe_fbx2iqm.py -- --config <cfg.json>
  VERIFY   python pipe_fbx2iqm.py --verify <file.iqm> [--config <cfg.json>]

The verify half imports nothing but the standard library, so any .iqm can be re-checked
without Blender, and every number printed by the build comes out of the written file's
raw bytes -- never out of Blender's dimensions/matrix caches and never out of the IQM's
own bbox field.

--------------------------------------------------------------------------------------
WHY IT IS BUILT THIS WAY  (each rule traces to a measurement, cited in the report)
--------------------------------------------------------------------------------------
iqm_export.py writes, with no unit conversion of its own:

    vertex  = Matrix.Scale(usescale) @ mesh_obj.matrix_world  @ v.co        (line 862/866/919)
    joint   =                          arm_obj.matrix_world   @ bone.matrix_local
              joint.translation *= usescale                                (line 726-731)
    frame   = (parent_pose^-1 @ pose) or (arm.matrix_world @ pose) for roots
              frame.translation *= usescale                                (line 622-642)

so the pipeline:

 1. NEVER calls transform_apply.  It left-multiplies *every* participating object's
    matrix_world by ONE common matrix X = Scale(g) @ R_orient.  Mesh and armature stay
    in the same frame, so the bind stays intact, and no fcurve is invalidated (applying
    a scale to an armature would silently rescale its pose-location channels).
 2. picks g so the armature's world scale lands on exactly 1.0.  jointData() rounds each
    joint scale to round(s*0x10000)/0x10000; at a world scale of 0.01 that writes
    0.0099945068, a 0.055% skeleton-vs-mesh shrink.  At 1.0 the rounding is exact.
 3. calls view_layer.update() after every transform/parent change and then RE-READS
    matrix_world.  Assigning .parent (or .matrix_world) leaves the cached evaluated
    matrix stale until the next depsgraph evaluation, and the exporter is often the
    first thing that forces one -- which is how a script can print 0.217 at every
    checkpoint and write 21.70 into the file.
 4. PREDICTS the export multiplier before exporting (it is X's scale times usescale,
    and usescale is derived from a measurement taken in the same frame X produces),
    prints the prediction, and then compares it to the raw bytes.
 5. uses usescale as the size lever.  It multiplies vertices, joint translations and
    frame translations by the same k and leaves joint rotation/scale alone: that is
    exactly a uniform rig rescale.  (usescale is NOT inert, contrary to one of the
    investigations.)
 6. selects meshes AND the armature explicitly.  Nothing selected -> a valid 124-byte
    header with zero meshes and {'FINISHED'}.  A mesh with no armature modifier selected
    alone -> findArmature() returns None -> 0 joints, no BLENDINDEXES, {'FINISHED'}.
    Two armatures selected -> findArmature() takes the first and binds the other hand's
    vertex groups by NAME to the wrong skeleton, {'FINISHED'}.
 7. enables the addon AFTER read_factory_settings (the reset unregisters it).
 8. produces a mirrored variant by reflecting the WRITTEN FILE, not by mirroring in
    Blender.  Skinning is v' = sum w_i * W_i * B_i^-1 * v.  With M = diag(-1,1,1) and
    M@M = I, conjugating every joint's LOCAL matrix (L -> M L M, i.e. translate.x *= -1,
    quat.y *= -1, quat.z *= -1, scale unchanged -- all proper rotations, all exactly
    representable) and reflecting every vertex position gives exactly M @ v' for every
    skinned vertex, at every frame.  Frame channels reflect the same way because
    value = offset + u16*scale and (-offset) + u16*(-scale) == -value exactly.
    No negative joint scale is produced; an IQM joint rotation is a quaternion and
    cannot carry a reflection.

--------------------------------------------------------------------------------------
ENGINE FACTS THE VERIFIER ENFORCES  (E:/UZDXREMA/src)
--------------------------------------------------------------------------------------
 common/models/models_iqm.cpp:349-465  I_FatalError on any unsupported vertex format.
      POSITION float3 | TEXCOORD float2 | NORMAL float3 |
      BLENDINDEXES ubyte4 or int4 | BLENDWEIGHTS ubyte4 or float4.
      Every other array TYPE (e.g. TANGENT) is silently skipped -- harmless.
 common/models/models_iqm.cpp:164      I_FatalError "Joint child comes before parent".
 common/models/models_iqm.cpp:168      I_FatalError "Joint parent index out of bounds".
 common/models/models_iqm.cpp:82       num_text == 0 -> LoadGeometry returns false.
 common/models/models_iqm.cpp:120      Adjacency is read unconditionally: must be in bounds.
 common/models/models_iqm.cpp:259-265  num_frames == 0 is handled (bind pose synthesised).
 common/models/models_iqm.cpp:349-360  LoadPosition writes v.x,v.z,v.y in file order, and
      CalculateBones* conjugates every bone matrix by a swapYZ (line 753/817-828/888),
      so the file's +Y axis is the engine's UP axis.  orientation:"fbx_native" rotates
      -90deg about X to undo the FBX importer's Y-up->Z-up conversion and put the model
      back in a Y-up file frame.
 r_data/models.cpp:1273 + models_iqm.cpp:520  surfaceskinids is MD3_MAX_SURFACES (32)
      entries and is indexed by mesh number: >32 surfaces is an out-of-bounds read.
 models_iqm.cpp:537                    a surface whose skin does not resolve is silently
      NOT DRAWN, so MODELDEF must declare a SurfaceSkin for every surface.
 hwrenderer/data/hw_vrmodes.cpp:635    vr_vunits_per_meter default 34.0 -> the default
      map_units_per_metre used to derive MODELDEF Scale.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys

try:
    import bpy  # type: ignore
    import mathutils  # type: ignore
    import addon_utils  # type: ignore
    HAVE_BPY = True
except Exception:
    HAVE_BPY = False


# =====================================================================================
# report plumbing
# =====================================================================================

class Report:
    def __init__(self):
        self.rows = []

    def add(self, sev, name, msg):
        self.rows.append((sev, name, msg))
        print("  [%-4s] %-28s %s" % (sev, name, msg))
        sys.stdout.flush()

    def ok(self, n, m):    self.add("PASS", n, m)
    def warn(self, n, m):  self.add("WARN", n, m)
    def fail(self, n, m):  self.add("FAIL", n, m)
    def info(self, n, m):  self.add("INFO", n, m)

    @property
    def failures(self):
        return [r for r in self.rows if r[0] == "FAIL"]


def die(msg, code=2):
    print("\n!!! PIPELINE FAILED: %s\n" % msg)
    sys.stdout.flush()
    sys.exit(code)


def f32(x):
    """Round a python float through float32, the way Blender's FloatProperty will."""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


# =====================================================================================
# tiny 4x4 math (verifier side -- pure stdlib, no mathutils)
# =====================================================================================

def m_ident():
    return [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


def m_mul(a, b):
    o = [0.0] * 16
    for r in range(4):
        for c in range(4):
            o[r * 4 + c] = (a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                            a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c])
    return o


def m_from_trs(t, q, s):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w)],
         [2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)]]
    m = m_ident()
    for i in range(3):
        for j in range(3):
            m[i * 4 + j] = r[i][j] * s[j]
        m[i * 4 + 3] = t[i]
    return m


def m_point(m, p):
    return (m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
            m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11])


# =====================================================================================
# raw IQM reader  (stdlib only -- this is the ONLY thing allowed to report a measurement)
# =====================================================================================

IQM_TYPES = {0: "POSITION", 1: "TEXCOORD", 2: "NORMAL", 3: "TANGENT",
             4: "BLENDINDEXES", 5: "BLENDWEIGHTS", 6: "COLOR"}
IQM_FMTS = {0: "byte", 1: "ubyte", 2: "short", 3: "ushort", 4: "int",
            5: "uint", 6: "half", 7: "float", 8: "double"}
FMT_SIZE = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 2, 7: 4, 8: 8}

# exactly what models_iqm.cpp accepts, keyed by array type
ENGINE_OK = {
    "POSITION":     [("float", 3)],
    "TEXCOORD":     [("float", 2)],
    "NORMAL":       [("float", 3)],
    "BLENDINDEXES": [("ubyte", 4), ("int", 4)],
    "BLENDWEIGHTS": [("ubyte", 4), ("float", 4)],
}

HDR_FIELDS = ("version filesize flags num_text ofs_text num_meshes ofs_meshes "
              "num_vertexarrays num_vertexes ofs_vertexarrays num_triangles ofs_triangles "
              "ofs_adjacency num_joints ofs_joints num_poses ofs_poses num_anims ofs_anims "
              "num_frames num_framechannels ofs_frames ofs_bounds num_comment ofs_comment "
              "num_extensions ofs_extensions").split()


def iqm_read(path):
    with open(path, "rb") as fh:
        buf = fh.read()
    d = {"path": path, "disk_size": len(buf), "raw": buf,
         "sha256": hashlib.sha256(buf).hexdigest()}
    d["magic"] = buf[:16]
    vals = struct.unpack_from("<27I", buf, 16)
    h = dict(zip(HDR_FIELDS, vals))
    d["hdr"] = h
    text = buf[h["ofs_text"]:h["ofs_text"] + h["num_text"]] if h["num_text"] else b""

    def name(off):
        if not text:
            return ""
        end = text.find(b"\0", off)
        return text[off:end if end >= 0 else len(text)].decode("utf-8", "replace")

    d["meshes"] = []
    for i in range(h["num_meshes"]):
        n, m, fv, nv, ft, nt = struct.unpack_from("<6I", buf, h["ofs_meshes"] + i * 24)
        d["meshes"].append({"name": name(n), "material": name(m), "first_vertex": fv,
                            "num_vertexes": nv, "first_triangle": ft, "num_triangles": nt})

    d["vas"] = []
    for i in range(h["num_vertexarrays"]):
        t, fl, fmt, size, off = struct.unpack_from("<5I", buf, h["ofs_vertexarrays"] + i * 20)
        d["vas"].append({"type": IQM_TYPES.get(t, "TYPE%d" % t), "type_id": t,
                         "flags": fl, "format": IQM_FMTS.get(fmt, "FMT%d" % fmt),
                         "format_id": fmt, "size": size, "offset": off})

    d["joints"] = []
    for i in range(h["num_joints"]):
        o = h["ofs_joints"] + i * 48
        n, = struct.unpack_from("<I", buf, o)
        p, = struct.unpack_from("<i", buf, o + 4)
        t = struct.unpack_from("<3f", buf, o + 8)
        q = struct.unpack_from("<4f", buf, o + 20)
        s = struct.unpack_from("<3f", buf, o + 36)
        d["joints"].append({"name": name(n), "parent": p, "t": t, "q": q, "s": s})

    d["poses"] = []
    for i in range(h["num_poses"]):
        o = h["ofs_poses"] + i * 88
        p, = struct.unpack_from("<i", buf, o)
        mask, = struct.unpack_from("<I", buf, o + 4)
        co = struct.unpack_from("<10f", buf, o + 8)
        cs = struct.unpack_from("<10f", buf, o + 48)
        d["poses"].append({"parent": p, "mask": mask, "offset": list(co), "scale": list(cs)})

    d["anims"] = []
    for i in range(h["num_anims"]):
        o = h["ofs_anims"] + i * 20
        n, ff, nf = struct.unpack_from("<3I", buf, o)
        fps, = struct.unpack_from("<f", buf, o + 12)
        fl, = struct.unpack_from("<I", buf, o + 16)
        d["anims"].append({"name": name(n), "first": ff, "num": nf, "fps": fps, "flags": fl})

    # vertex arrays
    d["pos"] = d["nrm"] = d["bi"] = d["bw"] = None
    nv = h["num_vertexes"]
    for va in d["vas"]:
        if va["type"] == "POSITION" and va["format"] == "float" and va["size"] == 3:
            d["pos"] = list(struct.unpack_from("<%df" % (nv * 3), buf, va["offset"]))
        elif va["type"] == "NORMAL" and va["format"] == "float" and va["size"] == 3:
            d["nrm"] = list(struct.unpack_from("<%df" % (nv * 3), buf, va["offset"]))
        elif va["type"] == "BLENDINDEXES" and va["format"] == "ubyte" and va["size"] == 4:
            d["bi"] = list(struct.unpack_from("<%dB" % (nv * 4), buf, va["offset"]))
        elif va["type"] == "BLENDWEIGHTS" and va["format"] == "ubyte" and va["size"] == 4:
            d["bw"] = list(struct.unpack_from("<%dB" % (nv * 4), buf, va["offset"]))

    d["tris"] = list(struct.unpack_from("<%dI" % (h["num_triangles"] * 3), buf,
                                        h["ofs_triangles"])) if h["num_triangles"] else []

    # frames
    d["frames"] = None
    d["frames_end"] = h["ofs_frames"]
    if h["num_frames"] and h["num_poses"]:
        need = h["num_frames"] * h["num_framechannels"] * 2
        if h["ofs_frames"] + need <= len(buf):
            vals16 = struct.unpack_from("<%dH" % (h["num_frames"] * h["num_framechannels"]),
                                        buf, h["ofs_frames"])
            it = iter(vals16)
            frames = []
            for _ in range(h["num_frames"]):
                fr = []
                for p in d["poses"]:
                    v = []
                    for ch in range(10):
                        x = p["offset"][ch]
                        if p["mask"] & (1 << ch):
                            x += next(it) * p["scale"][ch]
                        v.append(x)
                    fr.append((tuple(v[0:3]), tuple(v[3:7]), tuple(v[7:10])))
                frames.append(fr)
            d["frames"] = frames
            d["frames_end"] = h["ofs_frames"] + need
    return d


def iqm_baseframe(d):
    """World bind matrix per joint, exactly as models_iqm.cpp:204-225 builds it."""
    out = []
    for j in d["joints"]:
        m = m_from_trs(j["t"], j["q"], j["s"])
        out.append(m if j["parent"] < 0 else m_mul(out[j["parent"]], m))
    return out


def iqm_frame_world(d, fi):
    out = []
    for i, (t, q, s) in enumerate(d["frames"][fi]):
        m = m_from_trs(t, q, s)
        p = d["poses"][i]["parent"]
        out.append(m if p < 0 else m_mul(out[p], m))
    return out


def bbox_of(pos, idx=None):
    if not pos:
        return None
    rng = range(len(pos) // 3) if idx is None else idx
    it = iter(rng)
    try:
        i0 = next(it)
    except StopIteration:
        return None
    mn = [pos[i0 * 3 + a] for a in range(3)]
    mx = list(mn)
    for i in rng:
        for a in range(3):
            v = pos[i * 3 + a]
            if v < mn[a]:
                mn[a] = v
            if v > mx[a]:
                mx[a] = v
    return mn, mx


# =====================================================================================
# verification
# =====================================================================================

AXIS = {"x": 0, "y": 1, "z": 2}


def verify(path, cfg, predicted=None, rep=None):
    """Parse the FINAL file's bytes and check everything. Returns (report, measurements)."""
    rep = rep or Report()
    d = iqm_read(path)
    h = d["hdr"]
    exp = (cfg or {}).get("expect", {}) or {}
    m = {}

    print("\n---- VERIFY %s ----" % path)
    rep.info("sha256", d["sha256"])

    # ---- header --------------------------------------------------------------------
    if d["magic"] != b"INTERQUAKEMODEL\0":
        rep.fail("magic", "got %r, models_iqm.cpp requires INTERQUAKEMODEL\\0" % d["magic"])
        return rep, m
    rep.ok("magic", "INTERQUAKEMODEL\\0")
    rep.ok("version", "%d" % h["version"]) if h["version"] == 2 else \
        rep.fail("version", "%d (loader requires 2)" % h["version"])
    if h["filesize"] == d["disk_size"]:
        rep.ok("filesize", "header %d == disk %d" % (h["filesize"], d["disk_size"]))
    else:
        rep.fail("filesize", "header %d != disk %d" % (h["filesize"], d["disk_size"]))
    if h["num_text"] == 0:
        rep.fail("num_text", "0 -> models_iqm.cpp:82 returns false, model never loads")
    else:
        rep.ok("num_text", "%d" % h["num_text"])

    # empty-export detector (the silent 124-byte success)
    if h["num_meshes"] == 0 or h["num_vertexes"] == 0:
        rep.fail("empty_export", "num_meshes=%d num_vertexes=%d -- nothing was selected, "
                                 "or the selection contained no MESH"
                 % (h["num_meshes"], h["num_vertexes"]))

    rep.info("counts", "meshes=%d verts=%d tris=%d joints=%d poses=%d anims=%d "
                       "frames=%d framechannels=%d"
             % (h["num_meshes"], h["num_vertexes"], h["num_triangles"], h["num_joints"],
                h["num_poses"], h["num_anims"], h["num_frames"], h["num_framechannels"]))

    if h["num_meshes"] > 32:
        rep.fail("surface_cap", "%d surfaces > MD3_MAX_SURFACES(32): r_data/models.cpp:1273 "
                                "sizes surfaceskinids to 32 and models_iqm.cpp:520 indexes "
                                "it by mesh number -> OOB read" % h["num_meshes"])
    else:
        rep.ok("surface_cap", "%d <= 32" % h["num_meshes"])

    # ---- vertex array formats vs the loader ---------------------------------------
    seen = {}
    bad = []
    for va in d["vas"]:
        seen[va["type"]] = (va["format"], va["size"])
        allow = ENGINE_OK.get(va["type"])
        if allow is None:
            rep.info("va:%s" % va["type"], "%s x%d -- type not dispatched by "
                     "LoadGeometry, silently ignored (safe)" % (va["format"], va["size"]))
        elif (va["format"], va["size"]) in allow:
            rep.ok("va:%s" % va["type"], "%s x%d ACCEPTED" % (va["format"], va["size"]))
        else:
            bad.append(va["type"])
            rep.fail("va:%s" % va["type"], "%s x%d -> I_FatalError at load. accepted: %s"
                     % (va["format"], va["size"], allow))
    for req in ("POSITION", "TEXCOORD", "NORMAL"):
        if req not in seen:
            rep.fail("va_missing", "%s array absent" % req)
    if h["num_joints"] and ("BLENDINDEXES" not in seen or "BLENDWEIGHTS" not in seen):
        rep.fail("va_missing", "%d joints but no BLENDINDEXES/BLENDWEIGHTS -- the mesh is "
                               "not bound to the skeleton" % h["num_joints"])

    # ---- offsets in bounds ---------------------------------------------------------
    if h["num_triangles"]:
        end = h["ofs_adjacency"] + h["num_triangles"] * 12
        if h["ofs_adjacency"] == 0 or end > d["disk_size"]:
            rep.fail("adjacency", "ofs_adjacency=%d end=%d size=%d -- models_iqm.cpp:120 "
                     "reads it unconditionally" % (h["ofs_adjacency"], end, d["disk_size"]))
        else:
            rep.ok("adjacency", "in bounds [%d,%d)" % (h["ofs_adjacency"], end))

    if d["tris"]:
        mx = max(d["tris"])
        if mx >= h["num_vertexes"]:
            rep.fail("tri_index", "max %d >= num_vertexes %d" % (mx, h["num_vertexes"]))
        else:
            rep.ok("tri_index", "max %d < %d" % (mx, h["num_vertexes"]))

    # ---- joints --------------------------------------------------------------------
    names = [j["name"] for j in d["joints"]]
    m["joint_names"] = names
    if h["num_joints"]:
        viol = [i for i, j in enumerate(d["joints"]) if j["parent"] >= i]
        oob = [i for i, j in enumerate(d["joints"]) if j["parent"] >= h["num_joints"]]
        if viol:
            rep.fail("joint_order", "parent>=index at %s -> I_FatalError models_iqm.cpp:164"
                     % viol[:8])
        else:
            rep.ok("joint_order", "0 parent-before-child violations")
        if oob:
            rep.fail("joint_parent", "out of bounds at %s -> I_FatalError :168" % oob[:8])
        roots = [i for i, j in enumerate(d["joints"]) if j["parent"] < 0]
        rep.info("joint_roots", "%s" % [names[i] for i in roots])
        if len(roots) != 1:
            rep.warn("joint_roots", "%d root joints (expected 1)" % len(roots))
        badq = [names[i] for i, j in enumerate(d["joints"])
                if abs(math.sqrt(sum(c * c for c in j["q"])) - 1.0) > 1e-4]
        if badq:
            rep.fail("joint_quat", "non-unit quaternions: %s" % badq[:8])
        else:
            rep.ok("joint_quat", "all unit")
        # joint scale should be exactly 1 -- see jointData() 1/65536 quantisation
        worst = max(max(abs(c - 1.0) for c in j["s"]) for j in d["joints"])
        negs = [names[i] for i, j in enumerate(d["joints"]) if min(j["s"]) < 0]
        if negs:
            rep.fail("joint_scale", "NEGATIVE joint scale on %s -- a mirrored armature. "
                                    "An IQM joint rotation is a quaternion and cannot carry "
                                    "a reflection; mirror the file instead (mirror_x)."
                     % negs[:6])
        elif worst > 1e-4:
            rep.warn("joint_scale", "worst |scale-1| = %.9f (armature world scale was not "
                                    "normalised to 1.0; jointData() quantises to 1/65536)"
                     % worst)
        else:
            rep.ok("joint_scale", "worst |scale-1| = %.3e" % worst)
        if "joints" in exp and exp["joints"] != h["num_joints"]:
            rep.fail("joint_count", "expected %d, got %d" % (exp["joints"], h["num_joints"]))
        if "joint_names" in exp:
            missing = [n for n in exp["joint_names"] if n not in names]
            extra = [n for n in names if n not in exp["joint_names"]]
            if missing or extra:
                rep.fail("joint_names", "missing=%s extra=%s" % (missing[:8], extra[:8]))
            else:
                rep.ok("joint_names", "all %d expected names present" % len(names))
        if h["num_frames"] and h["num_poses"] != h["num_joints"]:
            rep.fail("pose_count", "num_poses %d != num_joints %d"
                     % (h["num_poses"], h["num_joints"]))
    elif exp.get("joints"):
        rep.fail("joint_count", "0 joints written -- either nothing but a mesh was selected "
                                "(findArmature() fell back to find_armature() and got None) "
                                "or the mesh has no armature modifier")

    # ---- weights -------------------------------------------------------------------
    if d["bi"] is not None:
        mx = max(d["bi"]) if d["bi"] else 0
        if mx >= max(h["num_joints"], 1):
            rep.fail("blend_index", "max %d >= num_joints %d" % (mx, h["num_joints"]))
        else:
            rep.ok("blend_index", "max %d < num_joints %d" % (mx, h["num_joints"]))
        used = sorted({d["bi"][i] for i in range(len(d["bi"])) if d["bw"][i] > 0}) \
            if d["bw"] else []
        unref = [names[i] for i in range(h["num_joints"]) if i not in used]
        rep.info("joints_with_geom", "%d of %d (unreferenced: %s)"
                 % (len(used), h["num_joints"], unref[:10]))
    if d["bw"] is not None:
        sums = {}
        zero = 0
        for v in range(h["num_vertexes"]):
            s = sum(d["bw"][v * 4:v * 4 + 4])
            sums[s] = sums.get(s, 0) + 1
            if s == 0:
                zero += 1
        rep.info("weight_sums", "%s" % dict(sorted(sums.items())[:6]))
        offb = sum(c for s, c in sums.items() if abs(s - 255) > 1)
        if zero:
            rep.fail("weight_zero", "%d vertices with total weight 0 (collapse to origin)"
                     % zero)
        if offb:
            rep.fail("weight_sum", "%d vertices whose ubyte weights do not sum to 255" % offb)
        elif not zero:
            rep.ok("weight_sum", "all %d vertices sum to 255" % h["num_vertexes"])

    # ---- frames --------------------------------------------------------------------
    if h["num_frames"]:
        maskbits = sum(bin(p["mask"]).count("1") for p in d["poses"])
        if maskbits != h["num_framechannels"]:
            rep.fail("framechannels", "sum of pose mask bits %d != num_framechannels %d"
                     % (maskbits, h["num_framechannels"]))
        else:
            rep.ok("framechannels", "%d == sum of pose mask bits" % maskbits)
        need = h["num_frames"] * h["num_framechannels"] * 2
        end = h["ofs_frames"] + need
        tail = h["ofs_bounds"] if h["ofs_bounds"] else d["disk_size"]
        if d["frames"] is None:
            rep.fail("frames", "frame block truncated: need %d bytes from %d, file is %d"
                     % (need, h["ofs_frames"], d["disk_size"]))
        elif end > tail:
            rep.fail("frames", "frame block %d..%d overruns %d" % (h["ofs_frames"], end, tail))
        else:
            rep.ok("frames", "%d frames x %d channels = %d bytes, ends at %d (next block %d)"
                   % (h["num_frames"], h["num_framechannels"], need, end, tail))
        tot = sum(a["num"] for a in d["anims"])
        rep.info("anims", "; ".join("%s first=%d num=%d fps=%.1f loop=%d"
                                    % (a["name"], a["first"], a["num"], a["fps"], a["flags"] & 1)
                                    for a in d["anims"]) or "(none)")
        if tot != h["num_frames"]:
            rep.warn("anim_frames", "sum of anim frames %d != num_frames %d"
                     % (tot, h["num_frames"]))
        if "frames" in exp and exp["frames"] != h["num_frames"]:
            rep.fail("frame_count", "expected %d frames, got %d"
                     % (exp["frames"], h["num_frames"]))
        if "anim_names" in exp:
            got = [a["name"] for a in d["anims"]]
            if got != exp["anim_names"]:
                rep.fail("anim_names", "expected %s got %s" % (exp["anim_names"], got))
            else:
                rep.ok("anim_names", "%s" % got)

        # is the animation real, or a flat wrong-action export?
        if d["frames"] and h["num_joints"]:
            movers = 0
            maxdeg = []
            for i in range(h["num_joints"]):
                q0 = d["frames"][0][i][1]
                n0 = math.sqrt(sum(c * c for c in q0)) or 1.0
                best = 0.0
                for fi in range(h["num_frames"]):
                    q = d["frames"][fi][i][1]
                    n = math.sqrt(sum(c * c for c in q)) or 1.0
                    dot = abs(sum(q0[c] * q[c] for c in range(4)) / (n0 * n))
                    best = max(best, 2.0 * math.degrees(math.acos(min(1.0, dot))))
                maxdeg.append(best)
                if best > 1.0:
                    movers += 1
            rep.info("rot_range", "joints rotating >1deg: %d of %d; top: %s"
                     % (movers, h["num_joints"],
                        ["%s=%.1fdeg" % (names[i], maxdeg[i])
                         for i in sorted(range(len(maxdeg)),
                                         key=lambda i: -maxdeg[i])[:4]]))
            need_mov = exp.get("min_moving_joints")
            if need_mov is not None:
                if movers < need_mov:
                    rep.fail("animation", "only %d joints move >1deg, expected >=%d -- this "
                             "is the wrong-active-action failure (a *_cntrl object action "
                             "gives a flat skeleton)" % (movers, need_mov))
                else:
                    rep.ok("animation", "%d joints move >1deg (>= %d)" % (movers, need_mov))
    else:
        rep.info("frames", "num_frames=0 -- models_iqm.cpp:259-265 synthesises a bind-pose "
                           "frame, so a bones-only/ZScript-driven model is fine")
        if exp.get("frames"):
            rep.fail("frame_count", "expected %d frames, got 0" % exp["frames"])

    # ---- geometry measurements (raw POSITION bytes) --------------------------------
    if d["pos"]:
        mn, mx = bbox_of(d["pos"])
        span = [mx[a] - mn[a] for a in range(3)]
        diag = math.sqrt(sum(s * s for s in span))
        m["bbox_min"], m["bbox_max"], m["span"], m["diag"] = mn, mx, span, diag
        rep.info("raw_bbox", "min=(%.8f, %.8f, %.8f) max=(%.8f, %.8f, %.8f)"
                 % (mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]))
        rep.info("raw_span", "x=%.8f y=%.8f z=%.8f  diag=%.8f" % (span[0], span[1], span[2], diag))

        # signed volume -- magnitude reported alongside the sign (never read a sign off ~0)
        sv = 0.0
        av = 0.0
        for t in range(0, len(d["tris"]), 3):
            a = d["tris"][t]; b = d["tris"][t + 1]; c = d["tris"][t + 2]
            p = d["pos"][a * 3:a * 3 + 3]; q = d["pos"][b * 3:b * 3 + 3]
            r = d["pos"][c * 3:c * 3 + 3]
            term = (p[0] * (q[1] * r[2] - q[2] * r[1])
                    - p[1] * (q[0] * r[2] - q[2] * r[0])
                    + p[2] * (q[0] * r[1] - q[1] * r[0]))
            sv += term
            av += abs(term)
        ratio = (sv / av) if av > 0 else 0.0
        m["signed_volume"] = sv
        m["signed_volume_ratio"] = ratio
        sign = 0 if abs(ratio) < 0.05 else (1 if sv > 0 else -1)
        m["signed_volume_sign"] = sign
        rep.info("signed_volume", "sum=%.9e  sum|term|=%.9e  ratio=%+.6f  sign=%s"
                 % (sv, av, ratio, sign if sign else "INCONCLUSIVE(|ratio|<0.05)"))
        if exp.get("signed_volume_sign") is not None:
            if sign == 0:
                rep.warn("winding", "ratio %.6f too small to read a sign from" % ratio)
            elif sign != exp["signed_volume_sign"]:
                rep.fail("winding", "signed-volume sign %+d, expected %+d -- faces are "
                         "inside-out relative to the reference build"
                         % (sign, exp["signed_volume_sign"]))
            else:
                rep.ok("winding", "signed-volume sign %+d matches reference" % sign)

    # ---- bind-pose sanity: the DISCRIMINATING test ---------------------------------
    # (skinning the bind pose by frame 0 is degenerate -- it passes for a mesh moved
    #  entirely off its skeleton.  Dominant-bone centroid distance is not.)
    if d["pos"] and h["num_joints"] and d["bi"] and d["bw"]:
        base = iqm_baseframe(d)
        acc = [[0.0, 0.0, 0.0, 0] for _ in range(h["num_joints"])]
        for v in range(h["num_vertexes"]):
            ws = d["bw"][v * 4:v * 4 + 4]
            best = max(range(4), key=lambda i: ws[i])
            if ws[best] == 0:
                continue
            j = d["bi"][v * 4 + best]
            for a in range(3):
                acc[j][a] += d["pos"][v * 3 + a]
            acc[j][3] += 1
        ds = []
        for j in range(h["num_joints"]):
            if acc[j][3] == 0:
                continue
            cen = [acc[j][a] / acc[j][3] for a in range(3)]
            hd = (base[j][3], base[j][7], base[j][11])
            ds.append(math.dist(cen, hd))
        if ds:
            mean = sum(ds) / len(ds)
            pct = 100.0 * mean / m["diag"] if m.get("diag") else 0.0
            m["centroid_pct"] = pct
            lim = exp.get("max_centroid_pct", 25.0)
            msg = ("mean joint<->dominant-vertex-centroid distance %.6f = %.2f%% of the "
                   "mesh diagonal (worst %.2f%%)"
                   % (mean, pct, 100.0 * max(ds) / m["diag"]))
            if pct > lim:
                rep.fail("bind_pose", msg + " -- limit %.1f%%. The mesh is bound to a "
                         "skeleton it does not sit on (two armatures selected, or the "
                         "vertex groups resolved by name against the wrong hand)." % lim)
            else:
                rep.ok("bind_pose", msg)
        # joint cloud vs mesh: a skeleton at a different scale from its mesh (the classic
        # "re-parenting re-introduced an ancestor transform" failure) shows up here as a
        # joint cloud collapsed near the origin.
        if h["num_joints"] > 1:
            hp = [(base[j][3], base[j][7], base[j][11]) for j in range(h["num_joints"])]
            lo = [min(p[a] for p in hp) for a in range(3)]
            hi = [max(p[a] for p in hp) for a in range(3)]
            cd = math.sqrt(sum((hi[a] - lo[a]) ** 2 for a in range(3)))
            frac = cd / m["diag"] if m.get("diag") else 0.0
            txt = ("joint-cloud diagonal %.6f = %.1f%% of the mesh diagonal %.6f"
                   % (cd, frac * 100.0, m.get("diag", 0.0)))
            if frac < 0.20 or frac > 3.0:
                rep.fail("joint_cloud", txt + " -- the skeleton is not at the same scale "
                         "as the geometry it drives")
            else:
                rep.ok("joint_cloud", txt)

        # every joint inside the mesh bbox
        outs = [names[j] for j in range(h["num_joints"])
                if any(not (m["bbox_min"][a] - 1e-6 <= base[j][3 + 4 * a] <= m["bbox_max"][a] + 1e-6)
                       for a in range(3))]
        if outs:
            rep.warn("joints_in_bbox", "outside the mesh bbox: %s" % outs[:8])
        else:
            rep.ok("joints_in_bbox", "all %d joints inside the mesh bbox" % h["num_joints"])

    # ---- the size gate -------------------------------------------------------------
    tgt = (cfg or {}).get("target")
    if tgt and m.get("span"):
        ax = AXIS[tgt["axis"].lower()]
        meas = m["span"][ax]
        want = float(tgt["metres"])
        tol = float(tgt.get("tolerance_pct", 0.25)) / 100.0
        m["measured_metres"] = meas
        err = (meas - want) / want if want else 0.0
        m["size_error"] = err
        txt = ("file %s-span = %.8f m, target %.8f m, error %+.4f%% (tolerance +-%.3f%%)"
               % (tgt["axis"], meas, want, err * 100.0, tol * 100.0))
        if abs(err) <= tol:
            rep.ok("size", txt)
            m["size_ok"] = True
        else:
            rep.fail("size", txt)
            m["size_ok"] = False

    # ---- prediction check ----------------------------------------------------------
    if predicted and m.get("span"):
        worst = max(abs(m["span"][a] - predicted[a]) for a in range(3))
        rel = worst / max(predicted) if max(predicted) else 0.0
        txt = ("predicted span (%.8f, %.8f, %.8f) vs raw bytes (%.8f, %.8f, %.8f), "
               "worst abs %.3e (%.2e relative)"
               % (predicted[0], predicted[1], predicted[2],
                  m["span"][0], m["span"][1], m["span"][2], worst, rel))
        if rel < 1e-5:
            rep.ok("prediction", txt)
        else:
            rep.fail("prediction", txt + " -- the export multiplier was NOT what the "
                     "normalisation predicted; something re-evaluated between the gate "
                     "and the export")

    print("---- end verify ----\n")
    return rep, m


# =====================================================================================
# mirror: reflect a written IQM through the x=0 plane, exactly
# =====================================================================================

def mirror_file_x(src, dst, reverse_winding=True):
    with open(src, "rb") as fh:
        buf = bytearray(fh.read())
    d = iqm_read(src)
    h = d["hdr"]

    def negf(off):
        v, = struct.unpack_from("<f", buf, off)
        struct.pack_into("<f", buf, off, -v)

    # vertex arrays: POSITION.x, NORMAL.x, TANGENT.x and TANGENT.w (handedness)
    for va in d["vas"]:
        n = h["num_vertexes"]
        stride = FMT_SIZE[va["format_id"]] * va["size"]
        if va["type"] == "POSITION" and va["format"] == "float":
            for i in range(n):
                negf(va["offset"] + i * stride)
        elif va["type"] == "NORMAL" and va["format"] == "float":
            for i in range(n):
                negf(va["offset"] + i * stride)
        elif va["type"] == "TANGENT" and va["format"] == "float" and va["size"] == 4:
            for i in range(n):
                negf(va["offset"] + i * stride)
                negf(va["offset"] + i * stride + 12)

    # triangles: reverse winding so faces stay outward after the reflection
    if reverse_winding:
        for t in range(h["num_triangles"]):
            o = h["ofs_triangles"] + t * 12
            a, b, c = struct.unpack_from("<3I", buf, o)
            struct.pack_into("<3I", buf, o, a, c, b)
        # adjacency is now stale; the engine reads it (models_iqm.cpp:120) but never
        # uses it, so it is left as-is rather than invalidated.

    # joints: L -> M L M  ==  translate.x *= -1, quat.y *= -1, quat.z *= -1
    for i in range(h["num_joints"]):
        o = h["ofs_joints"] + i * 48
        negf(o + 8)        # translate.x
        negf(o + 24)       # quat.y
        negf(o + 28)       # quat.z

    # poses/frames: same conjugation.  value = offset + u16*scale, so negating BOTH
    # offset and scale negates every decoded value exactly.
    for i in range(h["num_poses"]):
        o = h["ofs_poses"] + i * 88
        for ch in (0, 4, 5):          # translate.x, quat.y, quat.z
            negf(o + 8 + ch * 4)      # channeloffset
            negf(o + 48 + ch * 4)     # channelscale

    # per-frame bounds: mirror the x range (bbmins.x, bbmaxs.x swap and negate)
    if h["ofs_bounds"]:
        for f in range(h["num_frames"]):
            o = h["ofs_bounds"] + f * 32
            lo, = struct.unpack_from("<f", buf, o)
            hi, = struct.unpack_from("<f", buf, o + 12)
            struct.pack_into("<f", buf, o, -hi)
            struct.pack_into("<f", buf, o + 12, -lo)

    with open(dst, "wb") as fh:
        fh.write(buf)
    return dst


def mirror_selfcheck(orig, mirrored, rep):
    a = iqm_read(orig)
    b = iqm_read(mirrored)
    if a["hdr"]["num_vertexes"] != b["hdr"]["num_vertexes"]:
        rep.fail("mirror", "vertex count changed")
        return
    worst = 0.0
    n = a["hdr"]["num_vertexes"]
    for i in range(n):
        worst = max(worst,
                    abs(b["pos"][i * 3] + a["pos"][i * 3]),
                    abs(b["pos"][i * 3 + 1] - a["pos"][i * 3 + 1]),
                    abs(b["pos"][i * 3 + 2] - a["pos"][i * 3 + 2]))
    rep.ok("mirror_verts", "max |mirrored - diag(-1,1,1)*original| over %d verts = %.3e"
           % (n, worst)) if worst < 1e-9 else \
        rep.fail("mirror_verts", "reflection is not exact: worst %.3e" % worst)

    ba, bb = iqm_baseframe(a), iqm_baseframe(b)
    worstj = 0.0
    for i in range(len(ba)):
        pa = (ba[i][3], ba[i][7], ba[i][11])
        pb = (bb[i][3], bb[i][7], bb[i][11])
        worstj = max(worstj, abs(pb[0] + pa[0]), abs(pb[1] - pa[1]), abs(pb[2] - pa[2]))
    if worstj < 1e-7:
        rep.ok("mirror_joints", "bind-pose world heads reflect exactly, worst %.3e" % worstj)
    else:
        rep.fail("mirror_joints", "bind-pose heads do not reflect: worst %.3e" % worstj)

    if a["frames"] and b["frames"]:
        wf = 0.0
        step = max(1, len(a["frames"]) // 24)
        for fi in range(0, len(a["frames"]), step):
            wa, wb = iqm_frame_world(a, fi), iqm_frame_world(b, fi)
            for i in range(len(wa)):
                pa = (wa[i][3], wa[i][7], wa[i][11])
                pb = (wb[i][3], wb[i][7], wb[i][11])
                wf = max(wf, abs(pb[0] + pa[0]), abs(pb[1] - pa[1]), abs(pb[2] - pa[2]))
        if wf < 1e-6:
            rep.ok("mirror_anim", "posed joint positions reflect on %d sampled frames, "
                   "worst %.3e" % (len(range(0, len(a["frames"]), step)), wf))
        else:
            rep.fail("mirror_anim", "animation does not reflect: worst %.3e" % wf)


# =====================================================================================
# MODELDEF
# =====================================================================================

def emit_modeldef(cfg, meas, iqmpath):
    md = cfg.get("modeldef") or {}
    tgt = cfg["target"]
    mupm = float(md.get("map_units_per_metre", 34.0))
    ax = AXIS[tgt["axis"].lower()]
    measured = meas["span"][ax]
    want = float(tgt["metres"])
    # derived from the VERIFIED measurement, not from the nominal target
    s = mupm * want / measured
    d = iqm_read(iqmpath)
    mats = cfg.get("materials") or {}
    lines = []
    lines.append("// generated by pipe_fbx2iqm.py -- do not hand-edit Scale")
    lines.append("// source        : %s" % cfg["source_fbx"])
    lines.append("// iqm sha256    : %s" % d["sha256"])
    lines.append("// file %s-span  : %.8f m  (target %.8f m)" % (tgt["axis"], measured, want))
    lines.append("// map units/m   : %.4f  (vr_vunits_per_meter default, "
                 "hw_vrmodes.cpp:635)" % mupm)
    lines.append("// Scale         : %.4f * %.8f / %.8f = %.8f"
                 % (mupm, want, measured, s))
    lines.append("Model %s" % md.get("actor", cfg["name"]))
    lines.append("{")
    lines.append('\tPath "%s"' % md.get("path", "models"))
    lines.append('\tModel 0 "%s"' % os.path.basename(iqmpath))
    lines.append("\tScale %.8f %.8f %.8f" % (s, s, s))
    for i, mesh in enumerate(d["meshes"]):
        skin = mats.get(mesh["material"])
        if skin is None:
            die("material %r used by surface %d (%s) has no entry in config['materials']. "
                "models_iqm.cpp:537 silently DOES NOT DRAW a surface whose skin does not "
                "resolve, so every surface needs a SurfaceSkin."
                % (mesh["material"], i, mesh["name"]))
        lines.append('\tSurfaceSkin 0 %d "%s"   // %s / %s'
                     % (i, skin, mesh["name"], mesh["material"]))
    for extra in md.get("extra_lines", []):
        lines.append("\t" + extra)
    lines.append("}")
    return "\n".join(lines) + "\n"


# =====================================================================================
# Blender side
# =====================================================================================

def bl_update():
    bpy.context.view_layer.update()


def bl_reset_and_enable():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon_utils.enable("iqm_export", default_set=False, persistent=False)
    if "iqm" not in dir(bpy.ops.export):
        die("bpy.ops.export.iqm not present after enabling iqm_export. "
            "read_factory_settings unregisters addons; enable it AFTER the reset.")


def bl_scale_of(mw):
    """Column lengths + determinant -- never .to_scale() (it hides a reflection in signs)."""
    cols = [math.sqrt(sum(mw[r][c] ** 2 for r in range(3))) for c in range(3)]
    det = mw.to_3x3().determinant()
    return cols, det


def bl_import(cfg, rep):
    src = cfg["source_fbx"]
    if not os.path.isfile(src):
        die("source_fbx not found: %s" % src)
    opts = dict(filepath=src, use_custom_normals=True, use_anim=True,
                automatic_bone_orientation=False, ignore_leaf_bones=False,
                global_scale=1.0, use_image_search=False)
    opts.update(cfg.get("fbx_import_options", {}))
    bpy.ops.import_scene.fbx(**opts)
    bl_update()
    rep.info("import", "%s -> %d objects" % (os.path.basename(src), len(bpy.data.objects)))


def bl_resolve(cfg, rep):
    names = cfg["meshes"]
    meshes = []
    for n in names:
        o = bpy.data.objects.get(n)
        if o is None or o.type != "MESH":
            avail = sorted(x.name for x in bpy.data.objects if x.type == "MESH")
            die("mesh %r not in the FBX. available meshes:\n  %s" % (n, "\n  ".join(avail)))
        meshes.append(o)
    # Names are captured BEFORE anything is removed, and the filter runs off that
    # capture rather than off the objects.  bpy.data.objects.remove() invalidates
    # the Python wrapper, so a later `m.name` on a deleted entry raises
    # "ReferenceError: StructRNA of type Object has been removed" -- and it does
    # not raise here, it raises hundreds of lines downstream in bl_normalise,
    # which is a long way from the option that caused it.  Excluding a mesh used
    # to leave a dead reference in this list; the option had never been used with
    # a non-empty value.
    excl = set(cfg.get("exclude_meshes", []) or [])
    if excl:
        names = [m.name for m in meshes]
        for n in excl:
            o = bpy.data.objects.get(n)
            if o:
                bpy.data.objects.remove(o, do_unlink=True)
                rep.info("exclude", "removed %s" % n)
        meshes = [m for m, nm in zip(meshes, names) if nm not in excl]
        rep.info("exclude", "%d meshes remain after exclusion" % len(meshes))
    arm = None
    rig = cfg.get("rig", {})
    if rig.get("mode") == "existing":
        arm = bpy.data.objects.get(rig["armature"])
        if arm is None or arm.type != "ARMATURE":
            avail = sorted(x.name for x in bpy.data.objects if x.type == "ARMATURE")
            die("armature %r not found. available: %s" % (rig.get("armature"), avail))
    return meshes, arm


def bl_normalise(cfg, meshes, arm, rep):
    """Left-multiply every participating object's matrix_world by ONE matrix X.
       Returns (X, g)."""
    orient = cfg.get("orientation", "fbx_native")
    if orient == "fbx_native":
        R = mathutils.Matrix.Rotation(math.radians(-90.0), 4, "X")
    elif orient == "blender":
        R = mathutils.Matrix.Identity(4)
    else:
        die("orientation must be 'fbx_native' or 'blender', got %r" % orient)

    ref = arm if arm is not None else meshes[0]
    cols, det = bl_scale_of(ref.matrix_world)
    rep.info("ref_scale", "%s world column lengths = (%.9f, %.9f, %.9f)  det3 = %+.9e"
             % (ref.name, cols[0], cols[1], cols[2], det))
    spread = max(cols) / min(cols) - 1.0 if min(cols) > 0 else 9e9
    if spread > 1e-3:
        die("%s has non-uniform world scale %s -- refusing to guess a normalisation factor"
            % (ref.name, cols))
    if det < 0:
        die("%s has a NEGATIVE-determinant world matrix (det3=%+.9e, |det|^(1/3)=%.9f). "
            "It is a mirrored object. Export the positive-determinant twin and set "
            "\"mirror_x\": true instead -- an IQM joint rotation is a quaternion and "
            "cannot carry a reflection." % (ref.name, det, abs(det) ** (1.0 / 3.0)))

    g = 1.0 / cols[0]
    X = mathutils.Matrix.Scale(g, 4) @ R
    rep.info("normalise", "X = Scale(%.9f) @ Rotation(%s), so the armature world scale "
             "lands on 1.0 exactly (jointData() quantises joint scale to 1/65536)"
             % (g, "-90deg X" if orient == "fbx_native" else "identity"))

    objs = list(meshes) + ([arm] if arm else [])
    before = {o.name: o.matrix_world.copy() for o in objs}
    # armature first: meshes parented to it inherit, then we overwrite them explicitly
    for o in ([arm] if arm else []) + list(meshes):
        o.matrix_world = X @ before[o.name]
        bl_update()
    bl_update()

    worst = 0.0
    for o in objs:
        want = X @ before[o.name]
        got = o.matrix_world
        dv = max(abs(want[r][c] - got[r][c]) for r in range(4) for c in range(4))
        worst = max(worst, dv)
    if worst > 1e-6:
        die("NORMALISATION GATE FAILED: after view_layer.update() an object's re-read "
            "matrix_world differs from X @ original by %.3e" % worst)
    rep.ok("gate:matrix_world", "all %d objects re-read as X @ original, worst deviation "
           "%.3e" % (len(objs), worst))
    if arm is not None:
        cols2, det2 = bl_scale_of(arm.matrix_world)
        rep.ok("gate:arm_scale", "%s world scale now (%.9f, %.9f, %.9f) det3=%+.9e"
               % (arm.name, cols2[0], cols2[1], cols2[2], det2))
    return X, g


def bl_world_verts(o):
    mw = o.matrix_world
    return [mw @ v.co for v in o.data.vertices]


def bl_measure(meshes, rep):
    mn = [1e30] * 3
    mx = [-1e30] * 3
    n = 0
    for o in meshes:
        for co in bl_world_verts(o):
            n += 1
            for a in range(3):
                mn[a] = min(mn[a], co[a])
                mx[a] = max(mx[a], co[a])
    span = [mx[a] - mn[a] for a in range(3)]
    rep.info("pre_scale_span", "over %d verts: x=%.8f y=%.8f z=%.8f (normalised units)"
             % (n, span[0], span[1], span[2]))
    return mn, mx, span


# ---- rig building (guns: 33 loose part meshes, no source skeleton) -------------------

def _mesh_points(meshes, names):
    pts = []
    for n in names:
        o = bpy.data.objects.get(n)
        if o is None:
            die("rig references mesh %r which is not in the scene" % n)
        pts.extend(bl_world_verts(o))
    if not pts:
        die("rig point source %s yielded no vertices" % names)
    return pts


def _resolve_point(spec, k, rep):
    if isinstance(spec, (list, tuple)):
        return mathutils.Vector([float(x) for x in spec])
    off = mathutils.Vector([float(x) for x in spec.get("offset", (0, 0, 0))])
    if "metres" in spec:
        return mathutils.Vector([float(x) / k for x in spec["metres"]]) + off
    if "pick_of" in spec:
        # centroid of the vertices sitting at one extreme along one axis -- e.g. the
        # muzzle crown.  eps is relative to the picked set's own bbox diagonal.
        pts = _mesh_points(None, spec["pick_of"])
        a = AXIS[spec.get("axis", "z").lower()]
        hi = spec.get("extreme", "max") == "max"
        lo3 = [min(q[i] for q in pts) for i in range(3)]
        hi3 = [max(q[i] for q in pts) for i in range(3)]
        diag = math.sqrt(sum((hi3[i] - lo3[i]) ** 2 for i in range(3)))
        eps = float(spec.get("eps", 0.01)) * diag
        edge = hi3[a] if hi else lo3[a]
        sel = [q for q in pts if abs(q[a] - edge) <= eps]
        if not sel:
            die("pick_of selected no vertices for %r" % spec)
        p = mathutils.Vector((sum(q[0] for q in sel) / len(sel),
                              sum(q[1] for q in sel) / len(sel),
                              sum(q[2] for q in sel) / len(sel)))
        if rep:
            rep.info("pick_of", "%s %s%s: %d of %d verts within %.5f -> (%.5f, %.5f, %.5f)"
                     % (spec["pick_of"], "max" if hi else "min", spec.get("axis", "z"),
                        len(sel), len(pts), eps, p[0], p[1], p[2]))
        return p + off
    for key, fn in (("centroid_of", "cen"), ("bbox_center_of", "cc"),
                    ("bbox_min_of", "lo"), ("bbox_max_of", "hi")):
        if key in spec:
            pts = _mesh_points(None, spec[key])
            if fn == "cen":
                p = mathutils.Vector((sum(q[0] for q in pts) / len(pts),
                                      sum(q[1] for q in pts) / len(pts),
                                      sum(q[2] for q in pts) / len(pts)))
            else:
                lo = mathutils.Vector([min(q[a] for q in pts) for a in range(3)])
                hi = mathutils.Vector([max(q[a] for q in pts) for a in range(3)])
                p = lo if fn == "lo" else hi if fn == "hi" else (lo + hi) * 0.5
            return p + off
    die("unrecognised bone head spec: %r" % spec)


def _loose_components(o):
    """Connected components of a mesh, as lists of vertex indices."""
    n = len(o.data.vertices)
    parent = list(range(n))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for e in o.data.edges:
        a, b = find(e.vertices[0]), find(e.vertices[1])
        if a != b:
            parent[a] = b
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)
    return list(groups.values())


def bl_build_rig(cfg, meshes, k, rep):
    rig = cfg["rig"]
    arm_data = bpy.data.armatures.new(rig.get("name", cfg["name"] + "_rig"))
    arm = bpy.data.objects.new(rig.get("name", cfg["name"] + "_rig"), arm_data)
    bpy.context.scene.collection.objects.link(arm)
    arm.matrix_world = mathutils.Matrix.Identity(4)   # bone heads are given in world space
    bl_update()

    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm_data.edit_bones
    default_tail = mathutils.Vector(rig.get("tail_offset", [0.0, 0.0, 0.05]))
    order = []
    for b in rig["bones"]:
        e = eb.new(b["name"])
        head = _resolve_point(b.get("head", [0, 0, 0]), k, rep)
        tail = head + mathutils.Vector(b["tail_offset"]) if "tail_offset" in b \
            else head + default_tail
        e.head = head
        e.tail = tail
        e.roll = float(b.get("roll", 0.0))
        e.use_connect = False
        order.append(b["name"])
    for b in rig["bones"]:
        if b.get("parent"):
            if b["parent"] not in eb:
                die("bone %r has unknown parent %r" % (b["name"], b["parent"]))
            eb[b["name"]].parent = eb[b["parent"]]
    bpy.ops.object.mode_set(mode="OBJECT")
    bl_update()
    rep.ok("rig:build", "%d bones: %s" % (len(order), ", ".join(order[:8]) +
                                          (" ..." if len(order) > 8 else "")))

    # ---- vertex groups, one bone per part (binding granularity is the VERTEX) -------
    bind = dict(rig.get("bind", {}))
    default_bone = rig.get("bind_default")
    splits = {s["mesh"]: s for s in rig.get("splits", [])}
    assigned = {}
    for o in meshes:
        if o.name in splits:
            comps = _loose_components(o)
            spec = splits[o.name]["assign"]
            if len(comps) != len(spec):
                die("split of %s: mesh has %d loose components, config assigns %d"
                    % (o.name, len(comps), len(spec)))
            targets = [mathutils.Vector(_resolve_point(s["nearest"], k, rep)) for s in spec]
            wv = bl_world_verts(o)
            used = set()
            for ci, comp in enumerate(comps):
                cen = mathutils.Vector((0, 0, 0))
                for i in comp:
                    cen += wv[i]
                cen /= len(comp)
                best = min((t for t in range(len(spec)) if t not in used),
                           key=lambda t: (targets[t] - cen).length)
                used.add(best)
                bone = spec[best]["bone"]
                g = o.vertex_groups.get(bone) or o.vertex_groups.new(name=bone)
                g.add(comp, 1.0, "REPLACE")
                assigned.setdefault(bone, 0)
                assigned[bone] += len(comp)
                rep.info("split:%s" % o.name, "component %d (%d verts, centroid %.5f %.5f "
                         "%.5f) -> %s" % (ci, len(comp), cen[0], cen[1], cen[2], bone))
            continue
        bone = bind.get(o.name, default_bone)
        if bone is None:
            die("mesh %r has no bind entry and no rig.bind_default" % o.name)
        if bone not in arm_data.bones:
            die("mesh %r binds to unknown bone %r" % (o.name, bone))
        g = o.vertex_groups.new(name=bone)
        g.add(list(range(len(o.data.vertices))), 1.0, "REPLACE")
        assigned.setdefault(bone, 0)
        assigned[bone] += len(o.data.vertices)
    rep.ok("rig:bind", "%d bones carry geometry; unweighted bones: %s"
           % (len(assigned), [b.name for b in arm_data.bones if b.name not in assigned]))

    # ---- per-part materials, BEFORE the join ---------------------------------------
    #
    # An asset textured per-part in Substance arrives with one material over every
    # mesh and a separate texture set for each part.  Those sets each occupy their
    # own 0-1 UV space, so the parts OVERLAP in UV and no single skin can cover the
    # model -- the Beretta's UVs sprawl over thirteen tiles from (-2,-1) to (1,1)
    # with only 44.6% inside the first one.
    #
    # Splitting by material is the non-destructive fix: every part keeps its own UVs
    # and its own full-resolution texture, and the export writes one surface per
    # material instead of one for the lot.  The alternative -- repacking every island
    # into a single atlas and baking fourteen sets down into it -- loses resolution
    # and cannot be undone.
    #
    # config:  "material_by_mesh": { "<substring of mesh name>": "<material name>" }
    # Longest substring wins, so "magazine_bottom" beats "magazine".  Every material
    # named here still needs its own entry in config["materials"] to get a skin.
    mbm = cfg.get("material_by_mesh") or {}
    if mbm:
        keys = sorted(mbm.keys(), key=len, reverse=True)
        hits, misses = {}, []
        for o in meshes:
            low = o.name.lower()
            match = next((k for k in keys if k.lower() in low), None)
            if match is None:
                misses.append(o.name)
                continue
            name = mbm[match]
            mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
            o.data.materials.clear()
            o.data.materials.append(mat)
            hits.setdefault(name, []).append(o.name)
        if misses:
            die("material_by_mesh matched no pattern for: %s. Every mesh must map to a "
                "material or it inherits whichever slot the join happens to give it, "
                "which is exactly the overlap this option exists to prevent." % misses[:8])
        rep.ok("materials:split", "%d meshes -> %d materials (%s)"
               % (len(meshes), len(hits),
                  ", ".join("%s:%d" % (k, len(v)) for k, v in sorted(hits.items()))))

    # ---- join to stay under MD3_MAX_SURFACES ---------------------------------------
    if cfg.get("join_meshes", True) and len(meshes) > 1:
        pre = {}
        for o in meshes:
            for g in o.vertex_groups:
                pre[g.name] = pre.get(g.name, 0) + sum(
                    1 for v in o.data.vertices if any(e.group == g.index for e in v.groups))
        bpy.ops.object.select_all(action="DESELECT")
        for o in meshes:
            o.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        bl_update()
        joined = bpy.context.view_layer.objects.active
        post = {}
        for g in joined.vertex_groups:
            post[g.name] = sum(1 for v in joined.data.vertices
                               if any(e.group == g.index for e in v.groups))
        bad = [k2 for k2 in pre if pre[k2] != post.get(k2, 0)]
        if bad:
            die("vertex groups did not survive the join: %s"
                % [(b, pre[b], post.get(b, 0)) for b in bad[:6]])
        nog = sum(1 for v in joined.data.vertices if len(v.groups) == 0)
        multi = sum(1 for v in joined.data.vertices if len(v.groups) > 1)
        if nog:
            die("%d vertices have no vertex group after the join -- they would collapse "
                "to the origin" % nog)
        rep.ok("rig:join", "%d meshes -> 1 object, %d groups preserved exactly, "
               "%d verts with 0 groups, %d with >1, material slots %s"
               % (len(meshes), len(pre), nog, multi,
                  [m.name for m in joined.data.materials if m]))
        meshes = [joined]
    # Parent to the armature for a usable .blend.  Assigning .parent keeps matrix_basis
    # and REPLACES the ancestor chain, so an object still parented to the FBX root empty
    # silently acquires that empty's inverse scale/rotation the moment the depsgraph is
    # evaluated.  Save the world matrix, re-impose it, update, and check.
    for o in meshes:
        if not any(mo.type == "ARMATURE" for mo in o.modifiers):
            o.modifiers.new("Armature", "ARMATURE").object = arm
        keep = o.matrix_world.copy()
        o.parent = arm
        o.matrix_parent_inverse = arm.matrix_world.inverted()
        bl_update()
        o.matrix_world = keep
        bl_update()
        dv = max(abs(keep[r][c] - o.matrix_world[r][c]) for r in range(4) for c in range(4))
        if dv > 1e-6:
            die("re-parenting %s to the armature moved it: worst matrix_world delta %.3e"
                % (o.name, dv))
    bl_update()
    return meshes, arm


# ---- animation clips ----------------------------------------------------------------

def bl_action_fcurves(a):
    """Blender 4.4+/5.x slotted actions: Action.fcurves is gone on layered actions."""
    try:
        return list(a.fcurves)
    except AttributeError:
        pass
    out = []
    for layer in getattr(a, "layers", []):
        for strip in getattr(layer, "strips", []):
            for cb in getattr(strip, "channelbags", []):
                out.extend(cb.fcurves)
    return out


def bl_pick_action(cfg, arm, rep):
    rig = cfg.get("rig", {})
    want = rig.get("action")
    ad = arm.animation_data
    bone_actions = [a.name for a in bpy.data.actions
                    if any(fc.data_path.startswith("pose.bones")
                           for fc in bl_action_fcurves(a))]
    rep.info("bone_actions", "%s" % bone_actions)
    if want:
        act = bpy.data.actions.get(want)
        if act is None:
            die("action %r not found. bone actions present: %s" % (want, bone_actions))
        if want not in bone_actions:
            die("action %r has no pose.bones fcurves -- exporting it gives a flat skeleton "
                "(a *_cntrl object action). bone actions: %s" % (want, bone_actions))
        if ad is None:
            ad = arm.animation_data_create()
        ad.action = act
        bl_update()
    if ad is None or ad.action is None:
        rep.warn("action", "armature has no active action -- the export will contain "
                           "0 frames (fine for a ZScript-driven model)")
        return None
    if ad.action.name not in bone_actions:
        die("armature's active action %r is not a bone action -- flat export. "
            "set rig.action to one of %s" % (ad.action.name, bone_actions))
    rep.ok("action", "%s, frame range %s" % (ad.action.name, tuple(ad.action.frame_range)))
    return ad.action


def bl_make_clips(cfg, arm, base_action, rep):
    """collectAnimsAuto exports NLA strips first, then the active action, and dedupes on
       action NAME -- so each clip needs its own named copy."""
    clips = cfg.get("animations")
    if not clips:
        return
    if base_action is None:
        die("config lists animations but the armature has no bone action to slice")
    ad = arm.animation_data
    for tr in list(ad.nla_tracks):
        ad.nla_tracks.remove(tr)
    for c in clips:
        act = base_action.copy()
        act.name = c["name"]
        act.use_frame_range = True
        act.frame_start = float(c["start"])
        act.frame_end = float(c["end"])
        tr = ad.nla_tracks.new()
        tr.name = c["name"]
        tr.strips.new(c["name"], int(c["start"]), act)
    ad.action = None          # otherwise the full take is exported as an extra clip
    bl_update()
    rep.ok("clips", "%d NLA clips: %s" % (len(clips), [c["name"] for c in clips]))


# ---- export -------------------------------------------------------------------------

def bl_export(cfg, meshes, arm, k, path, rep):
    for o in bpy.context.scene.objects:
        o.select_set(False)
    for o in meshes:
        o.select_set(True)
    if arm is not None:
        arm.select_set(True)
        bpy.context.view_layer.objects.active = arm
    else:
        bpy.context.view_layer.objects.active = meshes[0]
    bl_update()

    sel = [o.name for o in bpy.context.selected_objects]
    arms = [o.name for o in bpy.context.selected_objects if o.type == "ARMATURE"]
    if not sel:
        die("nothing selected -- iqm_export writes a valid 124-byte header with zero "
            "meshes and reports FINISHED")
    if len(arms) > 1:
        die("%s armatures selected. findArmature() takes the FIRST one and binds every "
            "selected mesh's vertex groups to it BY NAME, silently producing a mesh bound "
            "to the wrong skeleton. Export one armature per file." % arms)
    rep.ok("selection", "%s (armature: %s)" % (sel, arms[0] if arms else "none"))

    opts = dict(filepath=path, file_format="IQM", usemesh=True,
                usemods=bool(cfg.get("apply_modifiers", False)),
                usebbox=bool(cfg.get("usebbox", False)),
                usecol=False, usescale=k, matfmt="m",
                derigify=False, boneorder="")
    rep.info("export", "usescale=%.9f (float32 %.9f) matfmt=m usebbox=%s usemods=%s"
             % (k, f32(k), opts["usebbox"], opts["usemods"]))
    bpy.ops.export.iqm(**opts)
    if not os.path.isfile(path):
        die("iqm_export reported success but wrote no file at %s" % path)
    rep.ok("wrote", "%s (%d bytes)" % (path, os.path.getsize(path)))


# =====================================================================================
# build driver
# =====================================================================================

def build(cfg):
    rep = Report()
    print("\n================ BUILD %s ================" % cfg["name"])
    out_dir = cfg["out_dir"]
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, cfg.get("out_name", cfg["name"] + ".iqm"))

    bl_reset_and_enable()
    bl_import(cfg, rep)
    meshes, arm = bl_resolve(cfg, rep)
    X, g = bl_normalise(cfg, meshes, arm, rep)
    mn, mx, span = bl_measure(meshes, rep)

    tgt = cfg["target"]
    ax = AXIS[tgt["axis"].lower()]
    if span[ax] <= 0:
        die("pre-scale span along %s is zero" % tgt["axis"])
    k = f32(float(tgt["metres"]) / span[ax])

    if cfg.get("rig", {}).get("mode") == "build":
        meshes, arm = bl_build_rig(cfg, meshes, k, rep)
        mn2, mx2, span2 = bl_measure(meshes, rep)   # re-measure the joined object
        worst = max(abs(span2[a] - span[a]) for a in range(3))
        if worst > 1e-6 * max(span):
            die("building the rig moved the geometry: world span was (%.8f, %.8f, %.8f) "
                "and is now (%.8f, %.8f, %.8f). The join or the re-parent re-introduced "
                "an ancestor transform -- the bone heads were computed in the OLD frame, "
                "so the skeleton and the mesh would ship at different scales/orientations."
                % (span[0], span[1], span[2], span2[0], span2[1], span2[2]))
        rep.ok("gate:rig_frame", "world span unchanged by rig build + join "
               "(worst delta %.3e)" % worst)
        mn, mx, span = mn2, mx2, span2

    base_action = bl_pick_action(cfg, arm, rep) if arm is not None else None
    bl_make_clips(cfg, arm, base_action, rep)

    if cfg.get("save_blend"):
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out_dir, cfg["name"] + ".blend"))

    attempt = 0
    predicted = None
    while True:
        attempt += 1
        predicted = [span[a] * f32(k) for a in range(3)]
        rep.info("predict#%d" % attempt,
                 "usescale=%.9f -> file span will be (%.8f, %.8f, %.8f) m; "
                 "target %s = %.8f m"
                 % (f32(k), predicted[0], predicted[1], predicted[2],
                    tgt["axis"], float(tgt["metres"])))
        # last chance: re-read the world geometry immediately before the export call, so
        # a depsgraph flush between the gate and the export cannot go unnoticed
        _, _, span_now = bl_measure(meshes, rep)
        drift = max(abs(span_now[a] - span[a]) for a in range(3))
        if drift > 1e-6 * max(span):
            die("world span drifted between the gate and the export: (%.8f, %.8f, %.8f) "
                "-> (%.8f, %.8f, %.8f)" % tuple(list(span) + list(span_now)))
        rep.ok("gate:pre_export", "world span stable, worst drift %.3e" % drift)
        bl_export(cfg, meshes, arm, f32(k), out, rep)
        vrep, meas = verify(out, cfg, predicted=predicted)
        rep.rows.extend(vrep.rows)
        if meas.get("size_ok", True) and not vrep.failures:
            break
        if meas.get("size_ok") is False and attempt < 3:
            corr = f32(k * float(tgt["metres"]) / meas["measured_metres"])
            rep.warn("correct#%d" % attempt,
                     "measured %.8f m vs target %.8f m -> usescale %.9f -> %.9f, re-export"
                     % (meas["measured_metres"], float(tgt["metres"]), f32(k), corr))
            k = corr
            continue
        break

    if [r for r in rep.rows if r[0] == "FAIL"]:
        for sev, n, msg in rep.rows:
            if sev == "FAIL":
                print("   FAIL %-24s %s" % (n, msg))
        die("verification failed after %d export attempt(s) -- NOT shipping %s"
            % (attempt, out))

    final = out
    if cfg.get("mirror_x"):
        mirrored = out
        tmp = out + ".unmirrored"
        os.replace(out, tmp)
        mirror_file_x(tmp, mirrored, cfg.get("mirror_reverse_winding", True))
        print("\n---- MIRROR SELF-CHECK ----")
        mrep = Report()
        mirror_selfcheck(tmp, mirrored, mrep)
        rep.rows.extend(mrep.rows)
        vrep, meas = verify(mirrored, cfg, predicted=None)
        rep.rows.extend(vrep.rows)
        os.remove(tmp)
        if [r for r in rep.rows if r[0] == "FAIL"]:
            die("mirrored file failed verification -- NOT shipping %s" % mirrored)
        final = mirrored
    else:
        vrep, meas = verify(final, cfg, predicted=None)

    md = emit_modeldef(cfg, meas, final)
    mdpath = os.path.join(out_dir, cfg["name"] + ".modeldef.txt")
    with open(mdpath, "w", encoding="utf-8") as fh:
        fh.write(md)
    print("\n================ MODELDEF (%s) ================\n%s" % (mdpath, md))

    npass = sum(1 for r in rep.rows if r[0] == "PASS")
    nwarn = sum(1 for r in rep.rows if r[0] == "WARN")
    print("================ OK: %s  (%d PASS, %d WARN, 0 FAIL) ================\n"
          % (final, npass, nwarn))
    return final


# =====================================================================================
# main
# =====================================================================================

def main():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = argv[1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--config")
    ap.add_argument("--verify")
    a = ap.parse_args(argv)

    cfg = None
    if a.config:
        with open(a.config, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)

    if a.verify:
        rep, meas = verify(a.verify, cfg)
        if rep.failures:
            die("%d check(s) failed" % len(rep.failures))
        print("VERIFY OK: %s" % a.verify)
        return

    if cfg is None:
        die("--config is required to build")
    if not HAVE_BPY:
        die("building needs Blender: "
            '"<blender.exe>" -b --factory-startup --python %s -- --config %s'
            % (os.path.abspath(__file__), a.config))
    build(cfg)


if __name__ == "__main__":
    main()
