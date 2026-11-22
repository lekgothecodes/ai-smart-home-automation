"""
train_model.py
--------------
Trains two Decision Tree classifiers from REAL logged data:
    1. Light model : should the light be ON?
    2. Fan model   : should the fan be ON?

Training labels come from MANUAL mode rows: in Manual mode the user decides,
so each row records "in these conditions, the user wanted light X and fan Y".
The model therefore learns the user's own preferences from data - it is not a
hand-written threshold rule.

Then it:
    - tests each model on the most recent 25 % of the data (not seen in training)
    - compares the model with the rule-based baseline on the same test rows
    - estimates the energy each method would use
    - saves figures for your report/slides
    - writes ../firmware/SmartHome_ESP32/model.h so the ESP32 runs the model

How to run (inside the ml folder):
    python train_model.py
    python train_model.py --csv data/sensor_logs.csv --max-depth 4

Outputs:
    results/report.md                 accuracy, comparison, energy estimate
    results/light_tree.png, fan_tree.png
    results/light_confusion.png, fan_confusion.png
    results/feature_importance.png
    ../firmware/SmartHome_ESP32/model.h
"""

import argparse
import os
import sys
from datetime import datetime

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from sklearn.metrics import (accuracy_score, confusion_matrix,
                             ConfusionMatrixDisplay, classification_report)
from sklearn.tree import DecisionTreeClassifier, export_text, plot_tree

FEATURES = ["temperature", "humidity", "ambientLight", "motion", "hour"]
# C parameter names used in model.h (same order as FEATURES)
C_NAMES = ["temperature", "humidity", "light", "motion", "hour"]

BULB_W = 3.0          # 12 V 3 W bulb
FAN_W = 12.0 * 0.12   # 12 V x 0.12 A = 1.44 W

TARGETS = {
    "light": {"label": "lightOn", "rule": "ruleLight", "watts": BULB_W},
    "fan":   {"label": "fanOn",   "rule": "ruleFan",   "watts": FAN_W},
}

MIN_ROWS = 40
MIN_PER_CLASS = 8


def load(csv_path, allow_synthetic, modes):
    if not os.path.exists(csv_path):
        sys.exit(f"{csv_path} not found. Run export_firebase_logs.py first.")
    df = pd.read_csv(csv_path)

    if "dataset" in df.columns and (df["dataset"] == "synthetic").any():
        if not allow_synthetic:
            sys.exit("This CSV contains SYNTHETIC test rows. They are only for "
                     "checking the software. Use real data, or pass "
                     "--allow-synthetic for a pipeline test.")
        print("WARNING: training on SYNTHETIC test data - do not report these results.")

    df = df[df["mode"].isin(modes)].copy()
    df = df.dropna(subset=FEATURES + ["lightOn", "fanOn"])
    if "ts" in df.columns:
        df = df.sort_values("ts")
    for col in ["motion", "lightOn", "fanOn", "ruleLight", "ruleFan"]:
        if col in df.columns:
            df[col] = df[col].astype(int)
    return df.reset_index(drop=True)


def time_split(df, test_fraction=0.25):
    # Rows close in time are almost identical, so a random split would let the
    # model "see" the test rows during training. Splitting by time is honest:
    # train on the past, test on the future.
    cut = int(len(df) * (1 - test_fraction))
    return df.iloc[:cut], df.iloc[cut:]


def tree_to_c(model, func_name):
    t = model.tree_
    one = list(model.classes_).index(1)
    lines = []

    def leaf_prob(node):
        v = t.value[node][0]
        total = float(v.sum())
        return float(v[one]) / total if total > 0 else 0.0

    def walk(node, depth):
        pad = "  " * depth
        if t.children_left[node] == t.children_right[node]:      # leaf
            lines.append(f"{pad}return {leaf_prob(node):.4f}f;")
            return
        name = C_NAMES[t.feature[node]]
        lines.append(f"{pad}if ({name} <= {t.threshold[node]:.4f}f) {{")
        walk(t.children_left[node], depth + 1)
        lines.append(f"{pad}}} else {{")
        walk(t.children_right[node], depth + 1)
        lines.append(f"{pad}}}")

    walk(0, 1)
    args = ", ".join(f"float {n}" for n in C_NAMES)
    unused = " ".join(f"(void){n};" for n in C_NAMES)
    body = "\n".join(lines)
    return (f"inline float {func_name}({args}) {{\n  {unused}\n{body}\n}}\n")


def placeholder_c(func_name):
    args = ", ".join(f"float {n}" for n in C_NAMES)
    unused = " ".join(f"(void){n};" for n in C_NAMES)
    return (f"inline float {func_name}({args}) {{\n  {unused}\n"
            f"  return -1.0f;  // not enough data to train this model\n}}\n")


def save_tree_png(model, name, path):
    plt.figure(figsize=(14, 7))
    plot_tree(model, feature_names=list(model.feature_names_in_), class_names=["OFF", "ON"],
              filled=True, rounded=True, fontsize=9, impurity=False,
              proportion=True)
    plt.title(f"Decision Tree - {name}")
    plt.tight_layout()
    plt.savefig(path, dpi=150)
    plt.close()


def save_confusion(y_true, y_pred, name, path):
    cm = confusion_matrix(y_true, y_pred, labels=[0, 1])
    ConfusionMatrixDisplay(cm, display_labels=["OFF", "ON"]).plot(cmap="Blues")
    plt.title(f"{name} model - test data")
    plt.tight_layout()
    plt.savefig(path, dpi=150)
    plt.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=os.path.join("data", "sensor_logs.csv"))
    ap.add_argument("--max-depth", type=int, default=4)
    ap.add_argument("--min-leaf", type=int, default=5)
    ap.add_argument("--modes", default="manual",
                    help="comma list of modes whose rows are training labels")
    ap.add_argument("--no-hour", action="store_true",
                    help="ignore time of day (use if the ESP32 clock is not synced)")
    ap.add_argument("--allow-synthetic", action="store_true")
    ap.add_argument("--model-out",
                    default=os.path.join("..", "firmware", "SmartHome_ESP32", "model.h"))
    args = ap.parse_args()

    # "hour" is the last feature, so dropping it keeps the C index mapping valid.
    FEATURES_USED = FEATURES[:-1] if args.no_hour else FEATURES

    df = load(args.csv, args.allow_synthetic, args.modes.split(","))
    print(f"Training rows (modes {args.modes}): {len(df)}")
    if len(df) < MIN_ROWS:
        sys.exit(f"Need at least {MIN_ROWS} rows; you have {len(df)}. "
                 "Collect more data in Manual mode.")

    os.makedirs("results", exist_ok=True)
    train, test = time_split(df)
    report = [f"# Model training report\n",
              f"Generated: {datetime.now():%Y-%m-%d %H:%M}\n",
              f"Data file: `{args.csv}`  \nRows used: {len(df)} "
              f"(train {len(train)}, test {len(test)}; split by time)\n",
              f"Features: {', '.join(FEATURES_USED)}  \n"
              f"Model: DecisionTreeClassifier(max_depth={args.max_depth}, "
              f"min_samples_leaf={args.min_leaf})\n"]
    c_funcs = {}
    importances = {}
    trained_any = False

    for name, cfg in TARGETS.items():
        y_train, y_test = train[cfg["label"]], test[cfg["label"]]
        counts = y_train.value_counts().to_dict()
        func = f"model{name.capitalize()}Probability"
        report.append(f"\n## {name.capitalize()} model\n")
        report.append(f"Training labels: ON = {counts.get(1, 0)}, OFF = {counts.get(0, 0)}\n")

        if counts.get(1, 0) < MIN_PER_CLASS or counts.get(0, 0) < MIN_PER_CLASS:
            msg = (f"Not trained: need at least {MIN_PER_CLASS} ON and "
                   f"{MIN_PER_CLASS} OFF examples in the training part.")
            print(f"[{name}] {msg}")
            report.append(msg + " The ESP32 will use the rules for this device.\n")
            c_funcs[name] = placeholder_c(func)
            continue

        model = DecisionTreeClassifier(max_depth=args.max_depth,
                                       min_samples_leaf=args.min_leaf,
                                       class_weight="balanced", random_state=42)
        model.fit(train[FEATURES_USED], y_train)
        pred = model.predict(test[FEATURES_USED])
        acc = accuracy_score(y_test, pred)

        rule_line = ""
        if cfg["rule"] in test.columns:
            rule_acc = accuracy_score(y_test, test[cfg["rule"]])
            rule_line = (f"| Rule-based baseline | {rule_acc:.1%} |\n")
        report.append("| Method | Accuracy vs user's choice (test data) |\n|---|---|\n"
                      f"| Decision Tree | {acc:.1%} |\n" + rule_line)

        # Energy estimate over the test period (each row ~ one log interval)
        if "ts" in test.columns and len(test) > 1:
            gaps = test["ts"].diff().fillna(30000).clip(upper=120000) / 3.6e6  # hours
        else:
            gaps = pd.Series([30 / 3600] * len(test), index=test.index)
        w = cfg["watts"]
        e_user = float((y_test * gaps).sum() * w)
        e_model = float((pd.Series(pred, index=test.index) * gaps).sum() * w)
        e_always = float(gaps.sum() * w)
        e_rule = float((test[cfg["rule"]] * gaps).sum() * w) if cfg["rule"] in test else float("nan")
        report.append(f"\nEstimated energy over the test period ({gaps.sum():.2f} h, {w:.2f} W load):\n\n"
                      "| Policy | Energy (Wh) |\n|---|---|\n"
                      f"| Always ON | {e_always:.2f} |\n| User (manual) | {e_user:.2f} |\n"
                      f"| Rule-based | {e_rule:.2f} |\n| Decision Tree | {e_model:.2f} |\n")

        report.append("\nClassification report (test data):\n\n```\n" +
                      classification_report(y_test, pred, labels=[0, 1],
                                            target_names=["OFF", "ON"], zero_division=0) +
                      "```\n")
        report.append("\nLearned tree (as text):\n\n```\n" +
                      export_text(model, feature_names=FEATURES_USED) + "```\n")

        save_tree_png(model, name, os.path.join("results", f"{name}_tree.png"))
        save_confusion(y_test, pred, name.capitalize(),
                       os.path.join("results", f"{name}_confusion.png"))
        importances[name] = dict(zip(FEATURES_USED, model.feature_importances_))

        # Re-train on ALL rows for the version that goes onto the ESP32.
        final = DecisionTreeClassifier(max_depth=args.max_depth,
                                       min_samples_leaf=args.min_leaf,
                                       class_weight="balanced", random_state=42)
        final.fit(df[FEATURES_USED], df[cfg["label"]])
        c_funcs[name] = tree_to_c(final, func)
        trained_any = True
        print(f"[{name}] test accuracy {acc:.1%}"
              + (f" (rules {rule_acc:.1%})" if rule_line else ""))

    if importances:
        imp = pd.DataFrame(importances).fillna(0)
        imp.plot(kind="barh", figsize=(7, 4))
        plt.xlabel("Importance (share of the tree's decisions)")
        plt.title("Which inputs the models rely on")
        plt.tight_layout()
        plt.savefig(os.path.join("results", "feature_importance.png"), dpi=150)
        plt.close()

    version = datetime.now().strftime("dt-%Y%m%d-%H%M")
    if args.allow_synthetic:
        version += "-SYNTHETIC-TEST"
    header = [
        "// model.h - GENERATED by ml/train_model.py. Do not edit by hand.",
        f"// Version: {version}",
        f"// Trained on {len(df)} logged rows (modes: {args.modes}).",
        "// Feature order: temperature, humidity, light (ambient ADC), motion (0/1), hour",
        "#pragma once",
        "",
        f"#define MODEL_TRAINED {1 if trained_any else 0}",
        f'#define MODEL_VERSION "{version}"',
        "",
        c_funcs["light"],
        c_funcs["fan"],
    ]
    with open(args.model_out, "w", encoding="utf-8") as f:
        f.write("\n".join(header))
    report.append(f"\nModel version written to ESP32: `{version}`\n")
    with open(os.path.join("results", "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(report))

    print(f"\nWrote {args.model_out}  (MODEL_TRAINED = {1 if trained_any else 0})")
    print("Wrote results/report.md and figures in results/")
    print("Next: open the sketch in Arduino IDE and upload it again.")


if __name__ == "__main__":
    main()
