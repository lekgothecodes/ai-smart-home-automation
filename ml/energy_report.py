"""
energy_report.py
----------------
Uses the REAL rows the ESP32 logged (data/sensor_logs.csv) to work out how
long the fan and bulb were ON in each mode, the energy used, and how much
was saved compared with leaving them ON all the time.

    python energy_report.py
    python energy_report.py --tariff 3.20 --home-bulb-w 60 --home-fan-w 50

--tariff       electricity price in Rand per kWh (use your municipality's
               current residential tariff and quote the source in your report)
--home-bulb-w  / --home-fan-w : power of a REAL household bulb and fan, used to
               scale the prototype's saving up to a real room (an estimate)

Output: results/energy_report.md
"""
import argparse
import os
import sys

import pandas as pd

BULB_W = 3.0          # prototype 12 V 3 W bulb
FAN_W = 12.0 * 0.12   # prototype 12 V 0.12 A fan = 1.44 W


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=os.path.join("data", "sensor_logs.csv"))
    ap.add_argument("--tariff", type=float, default=3.50, help="R per kWh")
    ap.add_argument("--home-bulb-w", type=float, default=60.0)
    ap.add_argument("--home-fan-w", type=float, default=50.0)
    a = ap.parse_args()

    if not os.path.exists(a.csv):
        sys.exit(f"{a.csv} not found. Run export_firebase_logs.py first.")
    df = pd.read_csv(a.csv)
    if "dataset" in df.columns and (df["dataset"] == "synthetic").any():
        sys.exit("This file contains SYNTHETIC rows. Use your real logged data.")
    df = df.dropna(subset=["ts", "lightOn", "fanOn", "mode"]).sort_values("ts")

    # Each row stands for the time until the next row (capped at 2 min so a
    # gap where the ESP32 was switched off is not counted).
    df["hours"] = df["ts"].diff().shift(-1).fillna(30000).clip(upper=120000) / 3.6e6

    lines = ["# Energy report (from real logged data)\n",
             f"Rows: {len(df)} · Logged time: {df['hours'].sum():.2f} h  \n"
             f"Tariff used: R{a.tariff:.2f}/kWh\n",
             "| Mode | Logged time (h) | Bulb ON (h) | Fan ON (h) | Energy used (Wh) | "
             "Always-ON energy (Wh) | Saving |",
             "|---|---|---|---|---|---|---|"]
    summary = {}
    for mode, g in df.groupby("mode"):
        h = g["hours"].sum()
        bulb_h = (g["lightOn"].astype(int) * g["hours"]).sum()
        fan_h = (g["fanOn"].astype(int) * g["hours"]).sum()
        used = bulb_h * BULB_W + fan_h * FAN_W
        always = h * (BULB_W + FAN_W)
        saving = 0 if always == 0 else 100 * (1 - used / always)
        summary[mode] = (h, bulb_h, fan_h, saving)
        lines.append(f"| {mode} | {h:.2f} | {bulb_h:.2f} | {fan_h:.2f} | {used:.2f} | "
                     f"{always:.2f} | {saving:.0f} % |")

    if "auto" in summary and summary["auto"][0] > 0:
        h, bulb_h, fan_h, saving = summary["auto"]
        bulb_frac, fan_frac = bulb_h / h, fan_h / h
        hours_year = 8 * 365   # assume the room is in use 8 hours a day
        always_kwh = (a.home_bulb_w + a.home_fan_w) * hours_year / 1000
        auto_kwh = (a.home_bulb_w * bulb_frac + a.home_fan_w * fan_frac) * hours_year / 1000
        lines += ["",
                  "## Scaled to a real room (estimate)",
                  f"Assumptions: {a.home_bulb_w:.0f} W bulb, {a.home_fan_w:.0f} W fan, "
                  "room used 8 h per day, and the same ON-fractions as measured in Auto mode "
                  f"(bulb {bulb_frac:.0%}, fan {fan_frac:.0%} of the time).\n",
                  "| | kWh per year | Cost per year |", "|---|---|---|",
                  f"| Always ON | {always_kwh:.0f} | R{always_kwh * a.tariff:.0f} |",
                  f"| With Auto control | {auto_kwh:.0f} | R{auto_kwh * a.tariff:.0f} |",
                  f"| **Saving** | **{always_kwh - auto_kwh:.0f}** | "
                  f"**R{(always_kwh - auto_kwh) * a.tariff:.0f}** |",
                  "",
                  "This is an estimate: it assumes the test conditions represent normal use."]
    else:
        lines.append("\nNo Auto-mode rows yet: run the system in Auto mode for at least 1 hour.")

    os.makedirs("results", exist_ok=True)
    with open(os.path.join("results", "energy_report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("\n".join(lines))
    print("\nSaved results/energy_report.md")


if __name__ == "__main__":
    main()
