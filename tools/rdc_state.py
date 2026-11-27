# Runs inside RenderDoc (tools/rdc_run.ps1 -Script tools/rdc_state.py): one line per draw with the state that
# decides its pixels (cull, front face, depth, stencil, blend, the pixel shader's textures and samplers), the same
# fields whichever API made the capture, so two renderers' captures of a frame diff line by line (MPayne2-nx: Vulkan
# and OpenGL). RDC_PICK = "x,y" in guest pixels also reads the main target there after each draw (slower).
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
count = {}
for a in draws:
    for o in a.outputs:
        if o != rd.ResourceId.Null():
            count[o] = count.get(o, 0) + 1
main = max(count, key=count.get) if count else rd.ResourceId.Null()
mt = textures.get(main)
log("%d draws; main target %dx%d" % (len(draws), mt.width if mt else 0, mt.height if mt else 0))
pick = os.environ.get("RDC_PICK", "")
px = None
if pick and mt:
    gx, gy = [int(v) for v in pick.split(",")]
    px = (int(gx * mt.width / 640.0), int(gy * mt.height / 480.0))

for i, a in enumerate(draws):
    ctl.SetFrameEvent(a.eventId, True)
    st = ctl.GetPipelineState()
    f = []
    try:
        r = st.GetRasterizer() if hasattr(st, "GetRasterizer") else None
    except Exception:
        r = None
    api = ctl.GetAPIProperties().pipelineType
    try:
        if api == rd.GraphicsAPI.Vulkan:
            vs = ctl.GetVulkanPipelineState()
            ras = vs.rasterizer
            f.append("cull %s front %s" % (ras.cullMode, "ccw" if ras.frontCCW else "cw"))
            ds = vs.depthStencil
            f.append("z %d/%d %s" % (ds.depthTestEnable, ds.depthWriteEnable, ds.depthFunction if ds.depthTestEnable else "-"))
            f.append("st %d" % ds.stencilTestEnable)
            f.append("bias %g/%g" % (ras.depthBias, ras.slopeScaledDepthBias))
        elif api == rd.GraphicsAPI.OpenGL:
            gs = ctl.GetGLPipelineState()
            ras = gs.rasterizer.state
            f.append("cull %s front %s" % (ras.cullMode, "ccw" if ras.frontCCW else "cw"))
            ds = gs.depthState
            f.append("z %d/%d %s" % (ds.depthEnable, ds.depthWrites, ds.depthFunction if ds.depthEnable else "-"))
            f.append("st %d" % gs.stencilState.stencilEnable)
            f.append("bias %g/%g" % (ras.depthBias, ras.slopeScaledDepthBias))
    except Exception as e:  # noqa
        f.append("state: %s" % e)
    try:
        bl = st.GetColorBlends()
        if bl:
            b = bl[0]
            f.append("blend %d %s %s %s wm %x" % (b.enabled, b.colorBlend.source, b.colorBlend.destination,
                                                  b.colorBlend.operation, b.writeMask))
    except Exception as e:  # noqa
        f.append("blend: %s" % e)
    try:
        for d in st.GetReadOnlyResources(rd.ShaderStage.Pixel):
            t = textures.get(d.descriptor.resource)
            if t:
                f.append("t%dx%d/%d %s" % (t.width, t.height, t.mips, t.format.Name()))
        for d in st.GetSamplers(rd.ShaderStage.Pixel):
            smp = d.sampler
            f.append("s[%s/%s/%s %s,%s lod %g..%g b%g a%g cmp %s]" % (
                smp.filter.minify, smp.filter.magnify, smp.filter.mip, smp.addressU, smp.addressV, smp.minLOD,
                smp.maxLOD, smp.mipBias, smp.maxAnisotropy, smp.compareFunction if smp.filter.filter == rd.FilterFunction.Comparison else "-"))
    except Exception as e:  # noqa
        f.append("res: %s" % e)
    line = "%3d n%-5d %s" % (i, a.numIndices, " | ".join(f))
    if px:
        v = ctl.PickPixel(main, px[0], px[1], rd.Subresource(0, 0, 0), rd.CompType.Typeless).floatValue
        line += " | px %.3f %.3f %.3f" % (v[0], v[1], v[2])
    log(line)

ctl.Shutdown()
cap.Shutdown()
report.close()
os._exit(0)
