"""Synchronize the tracked firmware PRD DOCX with the current repo contract."""

from __future__ import annotations

import argparse
import copy
import os
import tempfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile
from xml.etree import ElementTree as ET


WORD_NS = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
W = f"{{{WORD_NS}}}"
ET.register_namespace("w", WORD_NS)

RECONCILIATION_TEXTS = (
    "Reconciliation terhadap HEAD",
    "Main sekarang tiga commit di atas baseline historis: 98e4439 mengubah runtime dan schema pada ingest, fuzzy, safety, history, read API, simulator, serta research trace; 292aa47 menambahkan PRD/TSD firmware; ad65d5f menambahkan skill dan tooling tanpa dampak runtime.",
    "Kontrak firmware mempertahankan POST /api/ingest sebagai summary-v2. Arsip telemetry v3 memakai route batch baru dan jalur validasi/penyimpanan terpisah; jalur hardware tidak memanggil inference legacy.",
    "Data archive bersifat additive terhadap trace_json dan safety_json pada decisions, bukan pengganti atau reinterpretasi kolom tersebut.",
    "Migration HEAD saat ini berakhir pada 004_device_research_trace.sql, sehingga nama archive yang diharapkan adalah 005_telemetry_archive.sql. Nomor ini harus dicek ulang tepat sebelum PR implementasi.",
    "History dashboard mempertahankan cursor (recorded_at, reading_id), sedangkan archive memakai (device_id, boot_id, sequence) dan dapat menyimpan UTC-null. Query, DTO, dan export archive tidak boleh mengubah urutan history atau kolom CSV yang sudah kompatibel.",
)


def paragraph_text(paragraph: ET.Element) -> str:
    return "".join(node.text or "" for node in paragraph.iter(f"{W}t"))


def replace_paragraph_text(paragraph: ET.Element, text: str) -> None:
    paragraph_properties = paragraph.find(f"{W}pPr")
    run_properties = None
    for run in paragraph.findall(f"{W}r"):
        candidate = run.find(f"{W}rPr")
        if candidate is not None:
            run_properties = copy.deepcopy(candidate)
            break

    for child in list(paragraph):
        if child is not paragraph_properties:
            paragraph.remove(child)

    run = ET.SubElement(paragraph, f"{W}r")
    if run_properties is not None:
        run.append(run_properties)
    text_node = ET.SubElement(run, f"{W}t")
    if text[:1].isspace() or text[-1:].isspace():
        text_node.set("{http://www.w3.org/XML/1998/namespace}space", "preserve")
    text_node.text = text


def new_paragraph(text: str) -> ET.Element:
    paragraph = ET.Element(f"{W}p")
    run = ET.SubElement(paragraph, f"{W}r")
    text_node = ET.SubElement(run, f"{W}t")
    text_node.text = text
    return paragraph


def reconcile_document(xml: bytes) -> bytes:
    root = ET.fromstring(xml)
    body = root.find(f".//{W}body")
    if body is None:
        raise ValueError("word/document.xml has no document body")

    paragraphs = body.findall(f"{W}p")
    for paragraph in paragraphs:
        text = paragraph_text(paragraph)
        if text.startswith("Baseline repo adalah RayaSatriatama/IoT-Trashcan"):
            replace_paragraph_text(
                paragraph,
                "Baseline historis repo adalah RayaSatriatama/IoT-Trashcan, "
                "branch main, commit "
                "6fed00f02b6ef7dabc159d924f798d344a6072ba. "
                "Reconciliation implementasi saat ini ditujukan ke main pada "
                "commit ad65d5fe73c30b16c61da917eb517cdf2a783ba6.",
            )
        elif text.startswith("Repo dan kode dibaca pada commit"):
            replace_paragraph_text(
                paragraph,
                "Repo dan kode historis dibaca pada commit "
                "6fed00f02b6ef7dabc159d924f798d344a6072ba; reconciliation "
                "dilakukan terhadap main commit "
                "ad65d5fe73c30b16c61da917eb517cdf2a783ba6 pada 30 September 2026.",
            )

    while True:
        existing = list(body.findall(f"{W}p"))
        for index, paragraph in enumerate(existing):
            if paragraph_text(paragraph) == RECONCILIATION_TEXTS[0]:
                for previous in existing[index : index + len(RECONCILIATION_TEXTS)]:
                    if previous in list(body):
                        body.remove(previous)
                break
        else:
            break

    additions = list(RECONCILIATION_TEXTS)
    insertion_index = len(body)
    if list(body) and list(body)[-1].tag == f"{W}sectPr":
        insertion_index -= 1
    for offset, text in enumerate(additions):
        body.insert(insertion_index + offset, new_paragraph(text))

    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def synchronize(input_path: Path, output_path: Path) -> None:
    with ZipFile(input_path) as source:
        document_xml = reconcile_document(source.read("word/document.xml"))
        output_path.parent.mkdir(parents=True, exist_ok=True)
        handle, temporary_name = tempfile.mkstemp(
            prefix=f".{output_path.stem}-", suffix=".docx", dir=output_path.parent
        )
        os.close(handle)
        try:
            with ZipFile(temporary_name, "w", ZIP_DEFLATED) as target:
                for info in source.infolist():
                    payload = document_xml if info.filename == "word/document.xml" else source.read(info.filename)
                    target.writestr(info, payload)
            os.replace(temporary_name, output_path)
        finally:
            if os.path.exists(temporary_name):
                os.unlink(temporary_name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    synchronize(args.input, args.output)


if __name__ == "__main__":
    main()
