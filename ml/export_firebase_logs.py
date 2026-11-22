"""
export_firebase_logs.py
-----------------------
Downloads the sensor log rows that the ESP32 writes to Firebase /logs and
saves them as a CSV file for training.

How to run (Windows PowerShell, inside the ml folder):
    python export_firebase_logs.py

You will be asked for:
    - the Realtime Database URL  (Firebase console > Realtime Database)
    - the Firebase Web API key   (Firebase console > Project settings > General)
    - the DASHBOARD user's email and password (the one you use to sign in)
Nothing is saved to disk except the CSV.

Output:
    data/sensor_logs.csv
"""

import csv
import getpass
import os
import sys
from datetime import datetime, timezone, timedelta

import requests

OUTPUT_FILE = os.path.join("data", "sensor_logs.csv")
SAST = timezone(timedelta(hours=2))

COLUMNS = [
    "key", "ts", "datetime", "hour", "event", "mode", "source",
    "temperature", "humidity", "light", "ambientLight", "motion",
    "lightOn", "fanOn", "ruleLight", "ruleFan", "aiLight", "aiFan",
]


def ask(name, prompt, secret=False):
    value = os.environ.get(name)
    if value:
        return value
    return getpass.getpass(prompt) if secret else input(prompt).strip()


def sign_in(api_key, email, password):
    url = ("https://identitytoolkit.googleapis.com/v1/"
           f"accounts:signInWithPassword?key={api_key}")
    r = requests.post(url, json={"email": email, "password": password,
                                 "returnSecureToken": True}, timeout=20)
    if r.status_code != 200:
        sys.exit(f"Sign-in failed: {r.json().get('error', {}).get('message', r.text)}")
    return r.json()["idToken"]


def main():
    database_url = ask("FIREBASE_DATABASE_URL",
                       "Realtime Database URL (https://...firebaseio.com): ").rstrip("/")
    api_key = ask("FIREBASE_API_KEY", "Firebase Web API key: ")
    email = ask("FIREBASE_EMAIL", "Dashboard email: ")
    password = ask("FIREBASE_PASSWORD", "Dashboard password: ", secret=True)

    token = sign_in(api_key, email, password)
    print("Signed in. Downloading /logs ...")

    r = requests.get(f"{database_url}/logs.json", params={"auth": token}, timeout=60)
    if r.status_code != 200:
        sys.exit(f"Download failed ({r.status_code}): {r.text}")

    data = r.json() or {}
    if not data:
        sys.exit("No rows in /logs yet. Run the ESP32 for a while first.")

    rows = []
    for key, row in data.items():
        if not isinstance(row, dict):
            continue
        ts = row.get("ts")
        row = dict(row)
        row["key"] = key
        if isinstance(ts, (int, float)):
            dt = datetime.fromtimestamp(ts / 1000, SAST)
            row["datetime"] = dt.strftime("%Y-%m-%d %H:%M:%S")
            # If the ESP32 clock was not synced (hour = -1), use the server time.
            if row.get("hour", -1) in (-1, None):
                row["hour"] = dt.hour
        rows.append(row)

    rows.sort(key=lambda x: x.get("ts", 0))

    os.makedirs("data", exist_ok=True)
    with open(OUTPUT_FILE, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    modes = {}
    for row in rows:
        modes[row.get("mode")] = modes.get(row.get("mode"), 0) + 1
    print(f"Saved {len(rows)} rows to {OUTPUT_FILE}")
    print("Rows per mode:", modes)
    print("Only MANUAL-mode rows are used as training labels "
          "(they record the user's own choices).")


if __name__ == "__main__":
    main()
