"""
CyberRover — AI anomaly detection (كشف الشذوذ بالذكاء الاصطناعي)

The whitelist catches known rules (evil twin, weak encryption, dead zone).
This model catches what no rule describes: it learns the normal radio "fingerprint" of
every checkpoint from past patrols, then flags a checkpoint that looks unusual,
for example a new strong device in a room, a sudden signal drop, or many new networks.

Model: Isolation Forest (scikit-learn), trained on deviations from each checkpoint's baseline.

Usage:
    python anomaly_detection.py --demo                      # synthetic data, no robot needed
    python anomaly_detection.py --train data/baseline.jsonl # learn normal patrols
    python anomaly_detection.py --check data/scans.jsonl    # score a new patrol
"""
import argparse
import json
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import IsolationForest

from alerts import send_alert

MODEL_PATH = Path("data/anomaly_model.joblib")
FEATURES = ["d_best_rssi", "d_network_count", "d_unknown_count", "d_mean_rssi", "d_channels", "d_strongest_unknown"]


def record_features(rec: dict) -> dict:
    nets = rec.get("networks", [])
    unknown = [n for n in nets if n.get("status") != "OK"]
    return {
        "checkpoint": rec.get("checkpoint", "?"),
        "best_rssi": rec.get("best_rssi", -120),
        "network_count": len(nets),
        "unknown_count": len(unknown),
        "mean_rssi": float(np.mean([n["rssi"] for n in nets])) if nets else -120.0,
        "channels": len({n["ch"] for n in nets}),
        "strongest_unknown": max([n["rssi"] for n in unknown], default=-120),
    }


def load(path: str) -> pd.DataFrame:
    rows = [record_features(json.loads(l)) for l in Path(path).read_text(encoding="utf-8").splitlines() if l.strip()]
    return pd.DataFrame(rows)


def add_deviations(df: pd.DataFrame, baseline: pd.DataFrame) -> pd.DataFrame:
    """Each feature minus that checkpoint's normal (median) value."""
    base = baseline.groupby("checkpoint").median(numeric_only=True)
    out = df.copy()
    for col in ["best_rssi", "network_count", "unknown_count", "mean_rssi", "channels", "strongest_unknown"]:
        ref = out["checkpoint"].map(base[col]).fillna(base[col].median())
        out["d_" + col] = out[col] - ref
    return out


def train(baseline: pd.DataFrame) -> dict:
    X = add_deviations(baseline, baseline)[FEATURES]
    model = IsolationForest(n_estimators=200, contamination=0.03, random_state=42).fit(X)
    bundle = {"model": model, "baseline": baseline}
    MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
    joblib.dump(bundle, MODEL_PATH)
    return bundle


def check(bundle: dict, patrol: pd.DataFrame, notify: bool = True) -> pd.DataFrame:
    X = add_deviations(patrol, bundle["baseline"])
    X["score"] = bundle["model"].decision_function(X[FEATURES])   # lower = more unusual
    X["anomaly"] = bundle["model"].predict(X[FEATURES]) == -1
    if notify:
        for _, r in X[X["anomaly"]].iterrows():
            send_alert(r["checkpoint"], {"type": "ANOMALY", "score": r["score"], "rssi": r["best_rssi"]})
    return X


# ---------------- Demo data (simulated, for testing without the robot) ----------------
CHECKPOINTS = {  # checkpoint: (normal best RSSI, normal network count)
    "QR-01": (-48, 9), "QR-02": (-55, 11), "QR-03": (-76, 6), "QR-04": (-52, 10),
    "QR-05": (-58, 12), "QR-06": (-61, 8), "QR-07": (-50, 7),
}


def fake_record(cp, rng, rssi_shift=0, extra_unknown=0, unknown_rssi=-85):
    best, count = CHECKPOINTS[cp]
    nets = [{"ssid": "Hotel-Guest", "bssid": f"AC:84:C6:10:20:0{i}", "ch": [1, 6, 11][i % 3],
             "rssi": int(best + rssi_shift - 6 * i + rng.normal(0, 2)), "auth": "WPA2", "status": "OK"} for i in range(3)]
    for j in range(count - 3 + extra_unknown):
        strong = j >= count - 3
        nets.append({"ssid": f"Neighbor-{j}", "bssid": f"10:00:00:00:00:{j:02X}", "ch": int(rng.choice([1, 6, 11, 3])),
                     "rssi": int((unknown_rssi if strong else -82) + rng.normal(0, 3)), "auth": "WPA2", "status": "UNKNOWN"})
    return {"checkpoint": cp, "best_rssi": max(n["rssi"] for n in nets if n["status"] == "OK"), "networks": nets}


def demo():
    rng = np.random.default_rng(7)
    baseline = pd.DataFrame([record_features(fake_record(cp, rng)) for _ in range(30) for cp in CHECKPOINTS])
    bundle = train(baseline)

    tonight = [fake_record(cp, rng) for cp in CHECKPOINTS]
    tonight[1] = fake_record("QR-02", rng, rssi_shift=-17)                          # AP failed / blocked
    tonight[4] = fake_record("QR-05", rng, extra_unknown=1, unknown_rssi=-38)       # new strong device in room
    result = check(bundle, pd.DataFrame([record_features(r) for r in tonight]), notify=True)

    print("\nبيانات محاكاة · Simulated patrol results")
    print(result[["checkpoint", "best_rssi", "network_count", "strongest_unknown", "score", "anomaly"]]
          .to_string(index=False, float_format=lambda v: f"{v:.3f}"))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", action="store_true")
    ap.add_argument("--train")
    ap.add_argument("--check")
    a = ap.parse_args()
    if a.demo:
        demo()
    elif a.train:
        train(load(a.train))
        print("Model saved to", MODEL_PATH)
    elif a.check:
        print(check(joblib.load(MODEL_PATH), load(a.check)).to_string(index=False))
    else:
        ap.print_help()
