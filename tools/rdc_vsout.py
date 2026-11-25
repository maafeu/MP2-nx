# Runs inside RenderDoc (tools/rdc_run.ps1 -Script tools/rdc_vsout.py): the vertex shader's outputs of the first
# vertices of draw RDC_DRAW (index among the capture's draws), every output element by name, and the shader's
# disassembly, for comparing two renderers' captures of the same draw.
import os
import struct

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_dir = os.environ["RDC_OUT"]
os.makedirs(out_dir, exist_ok=True)
report = open(os.path.join(out_dir, "summary.txt"), "w")


def log(s):
    report.write(s + "\n")
    report.flush()


rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
if cap.OpenFile(cap_path, "", None) != rd.ResultCode.Succeeded:
    log("open failed")
    os._exit(1)
status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
if status != rd.ResultCode.Succeeded:
    log("replay failed: %s" % status)
    os._exit(1)


def actions(lst):
    for a in lst:
        yield a
        yield from actions(a.children)


draws = [a for a in actions(ctl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
a = draws[int(os.environ.get("RDC_DRAW", "39"))]
ctl.SetFrameEvent(a.eventId, True)
st = ctl.GetPipelineState()
refl = st.GetShaderReflection(rd.ShaderStage.Vertex)
mesh = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
log("draw eid %d, %d indices, vs out stride %d" % (a.eventId, a.numIndices, mesh.vertexByteStride))
data = ctl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, mesh.vertexByteStride * 8)
# outputs in signature order, each its component count of floats, packed as RenderDoc's post-VS buffer is
sig = list(refl.outputSignature)
for v in range(min(8, len(data) // max(mesh.vertexByteStride, 1))):
    base = v * mesh.vertexByteStride
    off = 0
    parts = []
    for s in sig:
        n = s.compCount
        vals = struct.unpack_from("<%df" % n, data, base + off)
        off += 4 * n
        parts.append("%s%d=%s" % (s.semanticName or s.varName, s.semanticIndex, " ".join("%.4f" % x for x in vals)))
    log("v%d %s" % (v, " | ".join(parts)))
log("")
# the vertex constants (b0) the game set for this draw, to tell game state from renderer
for blk in range(2):
    try:
        cb = st.GetConstantBlock(rd.ShaderStage.Vertex, blk, 0).descriptor
        raw = ctl.GetBufferData(cb.resource, cb.byteOffset, min(cb.byteSize or 3072, 3072))
        vals = struct.unpack_from("<%df" % (len(raw) // 4), raw)
        # b0 (c-96..c95) is the block whose first register is not the target size b1 starts with
        name = "dc" if vals[:2] in ((640.0, 480.0),) else "c"
        for i in range(0, len(vals), 4):
            if any(vals[i:i + 4]):
                log("%s%-4d %s" % (name, i // 4 - (96 if name == "c" else 0), " ".join("%.5f" % x for x in vals[i:i + 4])))
    except Exception as e:  # noqa
        log("constants %d: %s" % (blk, e))
log("")
log("vertex inputs:")
for attr in st.GetVertexInputs():
    log("  %s fmt %s buf %d off %d" % (attr.name, attr.format.Name(), attr.vertexBuffer, attr.byteOffset))
log("")
for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
    r = st.GetShaderReflection(stage)
    if not r:
        continue
    targets = ctl.GetDisassemblyTargets(True)
    pipe = st.GetGraphicsPipelineObject()
    text = ctl.DisassembleShader(pipe, r, targets[0])
    log("==== %s (%s)" % (stage, targets[0]))
    log(text)

ctl.Shutdown()
cap.Shutdown()
report.close()
os._exit(0)
