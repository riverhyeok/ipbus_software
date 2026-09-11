import csv
import struct
from collections import deque
from pathlib import Path

INPUT = Path("batch_20_stream.bin")
OUTPUT = Path("empty_event_examples_RIGHT.csv")

MAGIC_DAT = 0xBBBB0000


def is_data(w):
    return (w >> 39) & 1


def is_sync(w):
    return not is_data(w) and ((w >> 24) & 0x7FFF) == 0x3C5C


def is_header(w):
    return is_sync(w) and ((w >> 22) & 0x3) == 0


def is_trailer(w):
    return not is_data(w) and not is_sync(w)


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x2F) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def header_fields(w):
    return (w >> 14) & 0xFF, (w >> 12) & 0x3, w & 0xFFF


def trailer_fields(w):
    return (w >> 22) & 0x1FFFF, (w >> 16) & 0x3F, (w >> 8) & 0xFF, w & 0xFF


window = deque(maxlen=3)
right_low = 0
right_low_ready = False
word_pos = 0
examples = 0

with INPUT.open("rb") as fin, OUTPUT.open("w", newline="") as fout:
    writer = csv.writer(fout)
    writer.writerow([
        "example", "word_position", "role", "word_hex_40bit",
        "l1_counter", "type", "bcid",
        "chip_id", "status", "hits", "crc_field", "event_crc_check",
    ])

    while examples < 3:
        block_header = fin.read(8)
        if len(block_header) < 8:
            break

        magic, size_words = struct.unpack("<II", block_header)
        payload = fin.read(size_words * 4)

        if len(payload) != size_words * 4:
            print("Incomplete block encountered.")
            break

        if magic != MAGIC_DAT:
            continue

        for (w,) in struct.iter_unpack("<I", payload):
            if w == 0 or (w >> 31) == 0:
                continue

            tag = (w >> 29) & 0x7

            if tag == 0b100:
                right_low = w & 0xFFFFFF
                right_low_ready = True
                continue

            if tag != 0b101 or not right_low_ready:
                continue

            full40 = ((w & 0xFFFF) << 24) | right_low
            right_low_ready = False

            window.append((word_pos, full40))
            word_pos += 1

            if len(window) != 3:
                continue

            (p0, hdr), (p1, trl), (p2, next_hdr) = window

            if not (is_header(hdr) and is_trailer(trl) and is_header(next_hdr)):
                continue

            event_crc = crc8(hdr.to_bytes(5, "big") + trl.to_bytes(5, "big"))
            l1, typ, bcid = header_fields(hdr)
            chip, status, hits, crc_field = trailer_fields(trl)
            next_l1, next_typ, next_bcid = header_fields(next_hdr)

            examples += 1
            writer.writerow([examples, p0, "Header", f"0x{hdr:010X}",
                             l1, typ, bcid, "", "", "", "", "PASS" if event_crc == 0 else "FAIL"])
            writer.writerow([examples, p1, "Trailer", f"0x{trl:010X}",
                             "", "", "", chip, status, hits, f"0x{crc_field:02X}",
                             "PASS" if event_crc == 0 else "FAIL"])
            writer.writerow([examples, p2, "Next Header", f"0x{next_hdr:010X}",
                             next_l1, next_typ, next_bcid, "", "", "", "",
                             "PASS" if event_crc == 0 else "FAIL"])

            print(f"Found example {examples}: words {p0}, {p1}, {p2}")

print(f"Saved {examples} examples to {OUTPUT}")
