# Runs inside RenderDoc (tools/rdc_run.ps1): lists a capture's actions (draws, clears, copies, presents) with their
# colour outputs, and saves the presented image and each draw's first colour target as PNG.
#   qrenderdoc.exe --python tools/rdc_summary.py   (env RDC_FILE = the .rdc, RDC_OUT = a folder for the report)
import os
import sys

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
status = cap.OpenFile(cap_path, "", None)
if status != rd.ResultCode.Succeeded:
    log("open failed: %s" % status)
    os._exit(1)
status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if status != rd.ResultCode.Succeeded:
    log("replay failed: %s" % status)
    os._exit(1)


def save(rid, name):
    t = rd.TextureSave()
    t.resourceId = rid
    t.destType = rd.FileType.PNG
    t.mip = 0
    t.slice.sliceIndex = 0
    t.alpha = rd.AlphaMapping.Discard
    ctrl.SaveTexture(t, os.path.join(out_dir, name))


n = 0


def walk(actions, depth=0):
    global n
    for a in actions:
        flags = a.flags
        kind = []
        for f, s in ((rd.ActionFlags.Drawcall, "draw"), (rd.ActionFlags.Clear, "clear"), (rd.ActionFlags.Copy, "copy"),
                     (rd.ActionFlags.Present, "present"), (rd.ActionFlags.Dispatch, "dispatch")):
            if flags & f:
                kind.append(s)
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        log("%s%5d %-8s %-40s n=%d outs=%s" % ("  " * depth, a.eventId, "/".join(kind) or "-", a.GetName(ctrl.GetStructuredFile())[:40],
                                              a.numIndices, [str(o) for o in outs]))
        if (flags & rd.ActionFlags.Drawcall) and outs and n < 40:
            ctrl.SetFrameEvent(a.eventId, True)
            save(outs[0], "draw_%05d.png" % a.eventId)
            n += 1
        if (flags & rd.ActionFlags.Present):
            ctrl.SetFrameEvent(a.eventId, True)
            if a.copyDestination != rd.ResourceId.Null():
                save(a.copyDestination, "present_%05d.png" % a.eventId)
        if a.children:
            walk(a.children, depth + 1)


walk(ctrl.GetRootActions())
log("done")
ctrl.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
report.close()
os._exit(0)
