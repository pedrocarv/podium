#!/usr/bin/env python3
"""Validates the PresentationML parts of a .pptx against the ECMA-376 Transitional XSDs.

Usage: validate_schema.py SCHEMA_DIR FILE.pptx...
SCHEMA_DIR holds the unzipped OfficeOpenXML-XMLSchema-Transitional.zip (from ECMA-376 Part 4).
Markup-compatibility content (mc:AlternateContent → Fallback, ignorable extension attributes) is
reduced first, as a consumer would, since the base schemas don't describe it.
"""
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

MC = "http://schemas.openxmlformats.org/markup-compatibility/2006"
STANDARD = (
    "http://schemas.openxmlformats.org/drawingml/2006/main",
    "http://schemas.openxmlformats.org/presentationml/2006/main",
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
)
SCHEMAS = {
    "slide": ("pml.xsd", "ppt/slides/"),
    "notes": ("pml.xsd", "ppt/notesSlides/"),
    "presentation": ("pml.xsd", "ppt/presentation.xml"),
    "layout": ("pml.xsd", "ppt/slideLayouts/"),
    "master": ("pml.xsd", "ppt/slideMasters/"),
    "theme": ("dml-main.xsd", "ppt/theme/"),
}


def reduce(element):
    for child in list(element):
        if child.tag == f"{{{MC}}}AlternateContent":
            index = list(element).index(child)
            element.remove(child)
            fallback = child.find(f"{{{MC}}}Fallback")
            for offset, item in enumerate(list(fallback) if fallback is not None else []):
                element.insert(index + offset, item)
        else:
            reduce(child)
    for name in list(element.attrib):
        if name.startswith("{") and not name[1:].startswith(STANDARD):
            del element.attrib[name]


def main():
    schema_dir, files = sys.argv[1], sys.argv[2:]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in files:
            with zipfile.ZipFile(path) as z:
                for name in z.namelist():
                    if not name.endswith(".xml") or "/_rels/" in name:
                        continue
                    kind = next((k for k, (_, prefix) in SCHEMAS.items() if name.startswith(prefix)), None)
                    if not kind:
                        continue
                    root = ET.fromstring(z.read(name))
                    reduce(root)
                    out = os.path.join(tmp, name.replace("/", "_"))
                    ET.ElementTree(root).write(out, xml_declaration=True, encoding="UTF-8")
                    xsd = os.path.join(schema_dir, SCHEMAS[kind][0])
                    result = subprocess.run(["xmllint", "--noout", "--schema", xsd, out], capture_output=True, text=True)
                    if result.returncode != 0:
                        failures += 1
                        print(f"{path}: {name}")
                        for line in result.stderr.splitlines()[:6]:
                            print("   ", line.replace(out, name))
    print("all parts valid" if not failures else f"{failures} invalid part(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
