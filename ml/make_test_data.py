"""
make_test_data.py  -  SYNTHETIC data for SOFTWARE TESTING ONLY
--------------------------------------------------------------
Creates a fake CSV so you can check that Python, scikit-learn and the
training script work BEFORE you have real logs. Every row is marked
dataset = "synthetic" and train_model.py refuses it unless you pass
--allow-synthetic.

NEVER present results from this file as project results. The real model
must be trained on data logged by your ESP32.

    python make_test_data.py
    python train_model.py --csv data/synthetic_test.csv --allow-synthetic
"""
import csv
import os
import random

random.seed(1)
os.makedirs("data", exist_ok=True)
path = os.path.join("data", "synthetic_test.csv")

rows = []
ts = 1_790_000_000_000
for i in range(600):
    ts += 30_000
    hour = (8 + i // 120) % 24
    temp = round(random.uniform(22, 31), 2)
    hum = round(random.uniform(40, 70), 2)
    amb = random.randint(300, 3500)
    motion = 1 if random.random() < 0.5 else 0
    light_on = 1 if (motion and amb < 1400) or (hour >= 19 and motion) else 0
    fan_on = 1 if temp > 27.5 and random.random() > 0.1 else 0
    rows.append({
        "dataset": "synthetic", "ts": ts, "hour": hour, "event": "periodic",
        "mode": "manual", "source": "manual", "temperature": temp,
        "humidity": hum, "light": amb, "ambientLight": amb, "motion": motion,
        "lightOn": light_on, "fanOn": fan_on,
        "ruleLight": 1 if motion and amb < 1500 else 0,
        "ruleFan": 1 if temp >= 27 else 0, "aiLight": 0, "aiFan": 0,
    })

with open(path, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
    w.writeheader()
    w.writerows(rows)
print(f"Wrote {len(rows)} SYNTHETIC rows to {path} (testing only)")
