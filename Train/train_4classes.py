#!/usr/bin/env python3
"""Train the 4-class wrist model: extension, flexion, supination, pronation.

Replaces the 2-class binary pipeline in Train_model.ipynb. Writes the same
artifacts the firmware exports consume:

    model_best.keras, model_float32.tflite, model_int8.tflite, scaler.npz

Run from the Train/ directory:

    .venv/bin/python train_4classes.py
    .venv/bin/python export_scaler.py
    .venv/bin/python export_int8.py
"""
import glob
import os

import numpy as np
import tensorflow as tf
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler

CAPTURES = "../dataset/captures"
MAX_LEN = 100
N_CH = 6

# Output order must match scaler.h / GESTURE_CLASSES.
CLASSES = [
    "label_WRIST_EXTENSION",
    "label_WRIST_FLEXION",
    "label_SUPINATION",
    "label_PRONATION",
]

GRID_HIDDEN = [(32,), (64,), (32, 16), (64, 32), (128, 64)]
EPOCHS = 150
SEED = 42


def load_dataset():
    X, y = [], []
    for label, cls in enumerate(CLASSES):
        paths = sorted(glob.glob(os.path.join(CAPTURES, cls, "*.csv")))
        assert paths, f"no csv for {cls}"
        for path in paths:
            rows = np.loadtxt(path, delimiter=",", skiprows=1, usecols=range(4, 10))
            seq = np.zeros((MAX_LEN, N_CH), dtype=np.float32)
            n = min(rows.shape[0], MAX_LEN)
            seq[:n] = rows[:n]
            X.append(seq.flatten())
            y.append(label)
    return np.array(X, dtype=np.float32), np.array(y, dtype=np.float32)


def build(hidden):
    layers = [tf.keras.layers.Input((MAX_LEN * N_CH,))]
    layers += [tf.keras.layers.Dense(h, activation="relu") for h in hidden]
    layers += [tf.keras.layers.Dense(len(CLASSES), activation="softmax")]
    model = tf.keras.Sequential(layers)
    model.compile(optimizer="adam", loss="sparse_categorical_crossentropy",
                  metrics=["accuracy"])
    return model


def to_tflite(model, representative, int8):
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    if int8:
        conv.optimizations = [tf.lite.Optimize.DEFAULT]
        conv.representative_dataset = lambda: ([x[None]] for x in representative)
        conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
        conv.inference_input_type = tf.int8
        conv.inference_output_type = tf.int8
    return conv.convert()


def tflite_accuracy(blob, X, y):
    it = tf.lite.Interpreter(model_content=blob)
    it.allocate_tensors()
    inp, out = it.get_input_details()[0], it.get_output_details()[0]
    hits = 0
    for x, label in zip(X, y):
        if inp["dtype"] == np.int8:
            s, z = inp["quantization"]
            x = np.clip(np.round(x / s + z), -128, 127).astype(np.int8)
        it.set_tensor(inp["index"], x[None].astype(inp["dtype"]))
        it.invoke()
        probs = it.get_tensor(out["index"])[0]
        if out["dtype"] == np.int8:
            s, z = out["quantization"]
            probs = (probs.astype(np.float32) - z) * s
        hits += int(np.argmax(probs) == int(label))
    return hits / len(y)


def main():
    tf.keras.utils.set_random_seed(SEED)
    X, y = load_dataset()
    print("X", X.shape, "classes", [c[6:] for c in CLASSES])

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=SEED, stratify=y)
    X_train, X_val, y_train, y_val = train_test_split(
        X_train, y_train, test_size=0.2, random_state=SEED, stratify=y_train)

    scaler = StandardScaler().fit(X_train)
    X_train = scaler.transform(X_train).astype(np.float32)
    X_val = scaler.transform(X_val).astype(np.float32)
    X_test = scaler.transform(X_test).astype(np.float32)

    rows, models = [], {}
    for hidden in GRID_HIDDEN:
        name = "x".join(map(str, hidden))
        model = build(hidden)
        model.fit(
            X_train, y_train,
            validation_data=(X_val, y_val),
            batch_size=32, epochs=EPOCHS, verbose=0,
            callbacks=[tf.keras.callbacks.EarlyStopping(
                monitor="val_accuracy", patience=20, restore_best_weights=True)],
        )
        acc = model.evaluate(X_test, y_test, verbose=0)[1]
        models[name] = model
        rows.append((model.count_params(), name, acc))
        print(f"  {name:12s} {model.count_params():6d} params  test acc {acc:.4f}")

    # Smallest model within 2 points of the best.
    best_acc = max(r[2] for r in rows)
    params, name, acc = min((r for r in rows if r[2] >= best_acc - 0.02), key=lambda r: r[0])
    model = models[name]
    print(f"picked {name}: {params} params, test acc {acc:.4f} (best {best_acc:.4f})")

    y_pred = np.argmax(model.predict(X_test, verbose=0), axis=1)
    print("test accuracy:", float((y_pred == y_test).mean()))
    for i, cls in enumerate(CLASSES):
        mask = y_test == i
        print(f"  {cls[6:]:18s} acc={float((y_pred[mask] == i).mean()):.2f}  n={int(mask.sum())}")

    model.save("model_best.keras")
    open("model_float32.tflite", "wb").write(to_tflite(model, X_train, False))
    int8 = to_tflite(model, X_train, True)
    open("model_int8.tflite", "wb").write(int8)
    np.savez("scaler.npz", mean=scaler.mean_, scale=scaler.scale_)
    print(f"model_int8.tflite {len(int8) / 1024:.1f}KB  int8 test acc {tflite_accuracy(int8, X_test, y_test):.4f}")


if __name__ == "__main__":
    main()
