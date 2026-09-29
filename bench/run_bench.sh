#!/usr/bin/env bash
# run_bench.sh — A/B/C 브로드캐스트 방식 N 스윕 벤치마크
# uniform : 밀도 고정 d=0.05  -> WMAX = round(sqrt(N/d)) - 1  (월드 ∝ N, 버킷당 인원 일정 → C가 ~O(N))
# clustered: 작은 고정 월드(3x3 버킷=59) -> N↑ 시 밀도↑ (worst case, 격자 붕괴 관찰)
# 각 런: 서버 기동 → 봇 N접속 → 워밍업 → 측정창(정상상태) 동안 tick CSV 수집 + CPU% 측정
set -u
cd "$(dirname "$0")"
mkdir -p results

VIEW=20
DENSITY="0.05"
WARMUP=3
MEASURE=6
MOVE_MS=50
NS=(100 300 500 1000 2000)
MODES=(A B C)
DISTS=(uniform clustered)
CLK=$(getconf CLK_TCK)   # 보통 100

RUNS=results/runs.csv
echo "dist,mode,n,wmax,warmup_ms,measure_ms,cpu_pct" > "$RUNS"

cpu_jiffies(){ # $1=pid -> utime+stime
  awk '{print $14+$15}' /proc/$1/stat 2>/dev/null || echo 0
}

for dist in "${DISTS[@]}"; do
  for n in "${NS[@]}"; do
    if [ "$dist" = "uniform" ]; then
      # WMAX = round(sqrt(n/d)) - 1
      wmax=$(python3 -c "import math;print(max(1,round(math.sqrt($n/$DENSITY))-1))")
    else
      wmax=$((3*VIEW-1))   # 59
    fi
    for mode in "${MODES[@]}"; do
      tag="${dist}_${n}_${mode}"
      csv="results/${tag}.csv"
      echo ">>> $tag  (WMAX=$wmax)"

      pkill -f game_server_bench >/dev/null 2>&1; sleep 0.3
      BC_MODE=$mode WMAX=$wmax VIEW=$VIEW CSV="$csv" ./game_server_bench >/tmp/srv_$tag.log 2>&1 &
      SRV=$!
      # 기동 대기
      for i in $(seq 1 50); do grep -q "서버 대기" /tmp/srv_$tag.log 2>/dev/null && break; sleep 0.1; done

      ./bot "$n" 9000 "$MOVE_MS" >/tmp/bot_$tag.log 2>&1 &
      BOT=$!

      sleep "$WARMUP"
      c0=$(cpu_jiffies $SRV); ms0=$(date +%s%3N)
      sleep "$MEASURE"
      c1=$(cpu_jiffies $SRV); ms1=$(date +%s%3N)

      dj=$((c1-c0)); dms=$((ms1-ms0))
      cpu=$(python3 -c "print(round(($dj/$CLK)/($dms/1000.0)*100,1))" 2>/dev/null || echo 0)

      kill -TERM $BOT 2>/dev/null; kill -TERM $SRV 2>/dev/null
      wait $BOT 2>/dev/null; wait $SRV 2>/dev/null
      sleep 0.2

      conn=$(grep -o "connected=[0-9]*" /tmp/bot_$tag.log | head -1)
      echo "    cpu=${cpu}%  ${conn}  ticks=$(($(wc -l < "$csv")-1))"
      echo "${dist},${mode},${n},${wmax},$((WARMUP*1000)),$((MEASURE*1000)),${cpu}" >> "$RUNS"
    done
  done
done
pkill -f game_server_bench >/dev/null 2>&1
echo "DONE. runs -> $RUNS"
