#!/usr/bin/env python3
"""Extract xLights gettext strings and compile a PO catalog to GNU MO."""

import argparse
import ast
import json
import re
import struct
import warnings
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CALL = re.compile(r'_\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', re.DOTALL)
STRING = re.compile(r'"(?:\\.|[^"\\])*"', re.DOTALL)


def extract():
    messages = set()
    for directory in (ROOT / "src-ui-wx", ROOT / "src-core", ROOT / "xLights"):
        for path in directory.rglob("*"):
            if path.suffix not in {".cpp", ".h"}:
                continue
            try:
                source = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                source = path.read_text(encoding="utf-8-sig", errors="replace")
            for call in CALL.finditer(source):
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", SyntaxWarning)
                    value = "".join(ast.literal_eval(token) for token in STRING.findall(call.group(1)))
                messages.add(value)
    metadata_fields = {"label", "description", "tooltip", "checkboxLabel"}

    def collect_metadata(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key in metadata_fields and isinstance(child, str) and child.strip():
                    messages.add(child)
                collect_metadata(child)
        elif isinstance(value, list):
            for child in value:
                collect_metadata(child)

    for path in (ROOT / "resources" / "effectmetadata").glob("*.json"):
        try:
            collect_metadata(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError):
            continue
    return sorted(messages)


def write_template(path):
    entries = [
        'msgid ""',
        'msgstr ""',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"Language: es\\n"',
        "",
    ]
    for message in extract():
        if message.strip():
            entries.extend((f"msgid {json.dumps(message, ensure_ascii=False)}", 'msgstr ""', ""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(entries), encoding="utf-8", newline="\n")


def update_catalog(path):
    previous = parse_po(path) if path.exists() else {}
    messages = extract()
    lines = [
        'msgid ""',
        'msgstr ""',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"Language: es\\n"',
        '"Plural-Forms: nplurals=2; plural=(n != 1);\\n"',
        "",
    ]
    for message in messages:
        if message.strip():
            lines.extend((f"msgid {json.dumps(message, ensure_ascii=False)}", f"msgstr {json.dumps(previous.get(message, ''), ensure_ascii=False)}", ""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def parse_po(path):
    messages = {}
    current = {}
    field = None
    for line in path.read_text(encoding="utf-8").splitlines() + [""]:
        if not line.strip():
            if "msgid" in current:
                message_id = current["msgid"]
                message_translation = current.get("msgstr", "")
                if message_translation or message_id not in messages:
                    messages[message_id] = message_translation
            current = {}
            field = None
            continue
        if line.startswith("msgid ") or line.startswith("msgstr "):
            field = line.split(" ", 1)[0]
            current[field] = ast.literal_eval(line[len(field) + 1 :])
        elif line.startswith('"') and field:
            current[field] += ast.literal_eval(line)
    return messages


def compile_mo(po_path, output_path):
    messages = parse_po(po_path)
    messages = {key: value for key, value in messages.items() if value}
    ids = sorted(messages)
    originals = b""
    translations = b""
    original_table = []
    translation_table = []
    for key in ids:
        encoded = key.encode("utf-8")
        original_table.append((len(encoded), len(originals)))
        originals += encoded + b"\0"
    for key in ids:
        encoded = messages[key].encode("utf-8")
        translation_table.append((len(encoded), len(translations)))
        translations += encoded + b"\0"
    count = len(ids)
    header_size = 7 * 4
    original_offset = header_size + count * 8 * 2
    translation_offset = original_offset + len(originals)
    output = bytearray(struct.pack("<7I", 0x950412DE, 0, count, header_size, header_size + count * 8, 0, 0))
    for length, offset in original_table:
        output.extend(struct.pack("<2I", length, original_offset + offset))
    for length, offset in translation_table:
        output.extend(struct.pack("<2I", length, translation_offset + offset))
    output.extend(originals)
    output.extend(translations)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--extract", type=Path, help="write a POT template to this path")
    parser.add_argument("--update", type=Path, help="refresh a PO catalog, preserving existing translations")
    parser.add_argument("--compile", nargs=2, metavar=("PO", "MO"), help="compile PO to GNU MO")
    args = parser.parse_args()
    if args.extract:
        write_template(args.extract)
    if args.update:
        update_catalog(args.update)
    if args.compile:
        compile_mo(Path(args.compile[0]), Path(args.compile[1]))
    if not args.extract and not args.update and not args.compile:
        parser.error("specify --extract, --update or --compile")


if __name__ == "__main__":
    main()
