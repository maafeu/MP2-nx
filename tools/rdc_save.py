# Runs inside RenderDoc (tools/rdc_run.ps1 -Script tools/rdc_save.py): saves, at the draws RDC_DRAWS names
# ("138,139"), each texture the pixel shader reads and the draw's colour target, as PNG in RDC_OUT (draw index =
# position among the capture's draws, as tools/rdc_state.py numbers them).
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


def actions(lst):
    for a in lst:
        yield a
        yield from actions(a.children)


draws = [a for a in actions(ctl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]


def save(rid, name):
    s = rd.TextureSave()
    s.resourceId = rid
    s.destType = rd.FileType.PNG
    s.alpha = rd.AlphaMapping.Discard
    ok = ctl.SaveTexture(s, os.path.join(out_dir, name))
    log("%s %s" % (name, ok))


for spec in os.environ.get("RDC_DRAWS", "138").split(","):
    i = int(spec)
    a = draws[i]
    ctl.SetFrameEvent(a.eventId, True)
    st = ctl.GetPipelineState()
    for k, d in enumerate(st.GetReadOnlyResources(rd.ShaderStage.Pixel)):
        if d.descriptor.resource != rd.ResourceId.Null():
            save(d.descriptor.resource, "d%03d_tex%d.png" % (i, k))
    for k, o in enumerate(a.outputs):
        if o != rd.ResourceId.Null():
            save(o, "d%03d_out%d.png" % (i, k))

ctl.Shutdown()
cap.Shutdown()
report.close()
os._exit(0)
