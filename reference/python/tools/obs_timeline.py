"""Video <-> SimConnect-log time mapping from an OBS websocket log (obs.jsonl written by the recording logger).

Each recording: t_start, t_end, path, and a linear fit video_s = a * t + b (t = logger clock, time.perf_counter)
from GetRecordStatus samples. Copied from the author's recording toolkit (build_timeline.recordings_from_obs).
"""


def recordings_from_obs(obs):
    recs, cur = [], None
    for e in obs:
        if e.get("type") == "RecordStateChanged":
            state = e["data"].get("outputState", "")
            if state.endswith("_STARTED"):
                cur = {"t_start": e["t"], "t_end": None, "path": e["data"].get("outputPath"), "samples": []}
                recs.append(cur)
            elif state.endswith("_STOPPED") and cur:
                cur["t_end"] = e["t"]
                cur["path"] = e["data"].get("outputPath") or cur["path"]
                cur = None
        elif e.get("type") == "RecordFileChanged" and cur:
            cur["path"] = e["data"].get("newOutputPath") or cur["path"]
        elif e.get("type") == "rec_status" and e.get("active") and not e.get("paused") and e.get("duration_ms") is not None:
            if cur is None:  # logger started while OBS was already recording
                cur = {"t_start": None, "t_end": None, "path": None, "samples": []}
                recs.append(cur)
            cur["samples"].append((e["t"], e["duration_ms"] / 1000.0, e.get("rtt", 0)))
    for r in recs:
        s = [x for x in r["samples"] if x[2] < 0.05] or r["samples"]
        if len(s) >= 2:
            n = len(s)
            mt = sum(x[0] for x in s) / n
            mv = sum(x[1] for x in s) / n
            var = sum((x[0] - mt) ** 2 for x in s)
            a = sum((x[0] - mt) * (x[1] - mv) for x in s) / var if var else 1.0
            b = mv - a * mt
            resid = max(abs(a * x[0] + b - x[1]) for x in s)
            r.update(a=a, b=b, fit="GetRecordStatus", samples_used=n, max_resid_s=round(resid, 4))
        elif r["t_start"] is not None:
            r.update(a=1.0, b=-r["t_start"], fit="RecordStateChanged only", samples_used=0, max_resid_s=None)
        else:
            r.update(a=1.0, b=-s[0][0] + s[0][1], fit="single sample", samples_used=1, max_resid_s=None)
        if r["t_start"] is None:
            r["t_start"] = -r["b"] / r["a"]
        del r["samples"]
    return recs
