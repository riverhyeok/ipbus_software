import csv

INPUT = "batch_20_stream.bin"
OUTPUT = "first_1000_words_40bit.csv"
N_WORDS = 1000

with open(INPUT, "rb") as fin, open(OUTPUT, "w", newline="") as fout:
    writer = csv.writer(fout)
    writer.writerow(["word_index", "word_hex_40bit"])

    for i in range(N_WORDS):
        raw = fin.read(5)  # 40 bit = 5 bytes
        if len(raw) < 5:
            print(f"Stopped: only {i} complete words found.")
            break

        word = int.from_bytes(raw, byteorder="big")
        writer.writerow([i, f"0x{word:010X}"])

print(f"Saved to {OUTPUT}")
