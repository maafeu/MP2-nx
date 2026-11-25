# Runs inside RenderDoc (tools/rdc_run.ps1 -Script tools/rdc_pixel.py): the pixel history of a few pixels of the
# frame's main colour target, draw by draw, with each draw's shaders' textures and samplers. For comparing two
# renderers' captures of the same frame (MP2-nx: the Vulkan and the OpenGL renderer).
#   env RDC_FILE = the .rdc, RDC_OUT = a folder for the report, RDC_PIXELS = "x,y;x,y" in guest pixels (640x480)
import os

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

textures = {t.resourceId: t for t in ctl.GetTextures()}


def actions(lst):
    for a in lst:
        yield a
        yield from actions(a.children)


draws = [a for a in actions(ctl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
# the main target: the colour output most draws write
count = {}
for a in draws:
    for o in a.outputs:
        if o != rd.ResourceId.Null():
            count[o] = count.get(o, 0) + 1
main = max(count, key=count.get)
mt = textures[main]
log("%d draws; main target %s %dx%d (%d draws)" % (len(draws), main, mt.width, mt.height, count[main]))
scale = mt.width / 640.0


def describe(eid):
    ctl.SetFrameEvent(eid, True)
    st = ctl.GetPipelineState()
    parts = []
    try:
        ro = st.GetReadOnlyResources(rd.ShaderStage.Pixel)
        for b in ro:
            for r in b.resources:
                if r.resourceId == rd.ResourceId.Null():
                    continue
                t = textures.get(r.resourceId)
                if t:
                    parts.append("tex %dx%d m%d %s" % (t.width, t.height, t.mips, t.format.Name()))
        for s in st.GetSamplers(rd.ShaderStage.Pixel):
            for smp in s.samplers:
                parts.append("smp f(%s) addr(%s,%s) lod[%g,%g] bias %g aniso %g" % (
                    smp.filter.minify, smp.addressU, smp.addressV, smp.minLOD, smp.maxLOD, smp.mipBias,
                    smp.maxAnisotropy))
    except Exception as e:  # noqa
        parts.append("state: %s" % e)
    return "; ".join(parts)


for spec in os.environ.get("RDC_PIXELS", "300,350").split(";"):
    gx, gy = [int(v) for v in spec.split(",")]
    x, y = int(gx * scale), int(gy * scale)
    log("")
    log("pixel %d,%d (target %d,%d)" % (gx, gy, x, y))
    hist = ctl.PixelHistory(main, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    for m in hist:
        if not m.passed and not m.shaderDiscarded:
            pass
        pre = m.preMod.col.floatValue
        post = m.postMod.col.floatValue
        sh = m.shaderOut.col.floatValue
        a = ctl.GetAction(m.eventId)
        idx = next((i for i, d in enumerate(draws) if d.eventId == m.eventId), -1)
        log("  draw %d eid %d %s %s: shader %.3f %.3f %.3f %.3f -> %.3f %.3f %.3f %.3f%s" % (
            idx, m.eventId, a.GetName(ctl.GetStructuredFile()) if a else "?",
            "pass" if m.passed else "FAIL(%s)" % ("depth" if m.depthTestFailed else "stencil" if m.stencilTestFailed else
                                               "scissor" if m.scissorClipped else "backface" if m.backfaceCulled else
                                               "discard" if m.shaderDiscarded else "other"),
            sh[0], sh[1], sh[2], sh[3], post[0], post[1], post[2], post[3], ""))
        if m.passed and idx >= 0:
            log("      " + describe(m.eventId))

ctl.Shutdown()
cap.Shutdown()
report.close()
os._exit(0)
