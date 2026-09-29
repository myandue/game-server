#!/usr/bin/env python3
# aggregate.py — tick CSV들을 정상상태 창만 골라 (dist,mode,n)별 평균으로 요약.
import csv, os, math, json, sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES  = os.path.join(HERE, "results")
TARGET_HZ = 20.0  # 50ms tick 목표 주파수

def load_runs():
    runs = {}
    with open(os.path.join(RES, "runs.csv")) as f:
        for r in csv.DictReader(f):
            runs[(r["dist"], r["mode"], int(r["n"]))] = r
    return runs

def summarize(dist, mode, n, warmup_ms, measure_ms):
    path = os.path.join(RES, f"{dist}_{n}_{mode}.csv")
    if not os.path.exists(path): return None
    lo, hi = warmup_ms, warmup_ms + measure_ms
    build=[]; send=[]; intended=[]; written=[]; ncur=[]; tms=[]
    with open(path) as f:
        for r in csv.DictReader(f):
            t = float(r["t_ms"])
            if t < lo or t >= hi: continue
            build.append(float(r["build_us"])); send.append(float(r["send_us"]))
            intended.append(float(r["intended_bytes"])); written.append(float(r["written_bytes"]))
            ncur.append(float(r["n"])); tms.append(t)
    if len(build) < 2: return None
    span_s = (max(tms)-min(tms))/1000.0 or (measure_ms/1000.0)
    ticks = len(build)
    tick_rate = (ticks-1)/span_s if span_s>0 else 0
    avg=lambda a: sum(a)/len(a)
    b=avg(build); s=avg(send); it=avg(intended); wr=avg(written)
    return {
        "dist":dist, "mode":mode, "n":n,
        "ticks":ticks, "tick_rate":round(tick_rate,1),
        "build_us":round(b,1), "send_us":round(s,1), "tick_us":round(b+s,1),
        "intended_per_tick":round(it,0), "written_per_tick":round(wr,0),
        "bytes_per_sec":round(it*TARGET_HZ,0),   # 목표 20Hz 기준 전송량
        "n_seen":round(avg(ncur),0),
        # CPU%(파생): 단일 스레드 서버라 실제 tick 소요시간 × tick_rate 가 곧 코어 점유율
        "cpu_est":round((b+s)*tick_rate/1e6*100,1),
    }

def main():
    runs = load_runs()
    rows=[]
    for (dist,mode,n),r in runs.items():
        s = summarize(dist,mode,n,int(r["warmup_ms"]),int(r["measure_ms"]))
        if s: s["cpu_pct"]=float(r["cpu_pct"]); s["wmax"]=int(r["wmax"]); rows.append(s)
    order={"A":0,"B":1,"C":2}
    rows.sort(key=lambda x:(x["dist"],x["n"],order[x["mode"]]))
    cols=["dist","mode","n","wmax","ticks","tick_rate","build_us","send_us","tick_us",
          "intended_per_tick","written_per_tick","bytes_per_sec","cpu_est","cpu_pct"]
    out=os.path.join(RES,"summary.csv")
    with open(out,"w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=cols,extrasaction="ignore"); w.writeheader()
        for r in rows: w.writerow(r)
    print(json.dumps(rows,ensure_ascii=False))
    print(f"\nwrote {out} ({len(rows)} rows)", file=sys.stderr)

if __name__=="__main__": main()
