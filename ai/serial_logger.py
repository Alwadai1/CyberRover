"""
CyberRover — serial logger
يقرأ تقارير الروبوت من منفذ USB ويحفظها في ملف scans.jsonl، ويرسل تنبيه Telegram عند وجود تهديد.

Usage:
    python serial_logger.py --port COM5            (Windows)
    python serial_logger.py --port /dev/ttyUSB0    (Linux)
"""
import argparse
import json
from datetime import datetime
from pathlib import Path

import serial  # pip install pyserial

from alerts import send_alert


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="data/scans.jsonl")
    args = ap.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    with serial.Serial(args.port, args.baud, timeout=1) as ser, out.open("a", encoding="utf-8") as f:
        print(f"Listening on {args.port} … writing to {out}")
        while True:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line.startswith("{"):
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            record["time"] = datetime.now().isoformat(timespec="seconds")
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
            f.flush()

            cp = record.get("checkpoint", "?")
            print(f"[{record['time']}] {cp}: {record.get('best_rssi')} dBm, {record.get('coverage')}, "
                  f"{len(record.get('alerts', []))} alerts")
            for alert in record.get("alerts", []):
                send_alert(cp, alert)


if __name__ == "__main__":
    main()
