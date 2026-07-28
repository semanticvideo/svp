#!/usr/bin/env python3
"""Generate synchronized RC2 DOCX, PDF, and public release ZIP artifacts."""

import hashlib
import os
import platform
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path
from xml.etree import ElementTree

import markdown
from docx import Document
from docx.shared import Inches
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, ByteStringObject


REPO_ROOT = Path(__file__).resolve().parents[1]
SPEC_MARKDOWN = REPO_ROOT / "spec/SVP_v1_0_RC2.md"
SPEC_DOCX = REPO_ROOT / "spec/SVP_v1_0_RC2.docx"
SPEC_PDF = REPO_ROOT / "spec/SVP_v1_0_RC2.pdf"
RELEASE_ZIP = REPO_ROOT / "releases/SVP_v1_0_RC2_Release_Package.zip"
PACKAGE_ROOT = "SVP_v1_0_RC2_Release_Package"
FIXED_ZIP_TIME = (2026, 6, 19, 20, 25, 0)
EXPECTED_SYSTEM = "Darwin"
EXPECTED_MACHINE = "arm64"
EXPECTED_SOFFICE_VERSION = (
    "LibreOfficeDev 26.8.0.0.alpha0 "
    "2c87e51eeaa2b413ff4ae097b2705eea1995d8e5"
)
REQUIRED_TEXT = (
    "Semantic Video Package (SVP) v1.0 Release Candidate 2",
    "Exactly one model-lock.json MUST exist at the model-cache root",
    "Installation MUST assemble the complete set in a separate staging directory",
)


def require_generation_environment() -> str:
    system = platform.system()
    machine = platform.machine()
    if (system, machine) != (EXPECTED_SYSTEM, EXPECTED_MACHINE):
        raise SystemExit(
            "RC2 artifacts require the tested Apple Silicon environment: "
            f"expected {EXPECTED_SYSTEM} {EXPECTED_MACHINE}, got {system} {machine}"
        )

    soffice = shutil.which("soffice")
    if not soffice:
        raise SystemExit("soffice is required to generate RC2 release artifacts")
    result = subprocess.run(
        [soffice, "--version"],
        check=True,
        text=True,
        capture_output=True,
    )
    version = result.stdout.strip()
    if version != EXPECTED_SOFFICE_VERSION:
        raise SystemExit(
            "RC2 artifacts require the tested LibreOffice build: "
            f"expected {EXPECTED_SOFFICE_VERSION!r}, got {version!r}"
        )
    return soffice


def normalize_text(value: str) -> str:
    value = value.replace("`", "")
    return " ".join(value.split())


def render_html(markdown_text: str) -> str:
    body = markdown.markdown(
        markdown_text,
        extensions=["fenced_code", "tables", "sane_lists"],
        output_format="html5",
    )
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SVP Specification v1.0 RC2</title>
<style>
@page {{ size: letter; margin: 0.8in; }}
body {{ font-family: Arial, sans-serif; font-size: 10.5pt; line-height: 1.35; color: #111827; }}
h1 {{ font-size: 24pt; color: #123b5d; margin: 0 0 18pt; page-break-before: always; }}
h1:first-of-type {{ page-break-before: avoid; }}
h2 {{ font-size: 18pt; color: #174f78; margin: 18pt 0 8pt; page-break-after: avoid; }}
h3 {{ font-size: 14pt; color: #24658f; margin: 14pt 0 6pt; page-break-after: avoid; }}
h4, h5, h6 {{ font-size: 11.5pt; color: #24658f; margin: 12pt 0 5pt; page-break-after: avoid; }}
p {{ margin: 0 0 7pt; }}
pre {{ font-family: Menlo, Consolas, monospace; font-size: 8.5pt; background: #f3f4f6; border: 1px solid #d1d5db; padding: 7pt; white-space: pre-wrap; }}
code {{ font-family: Menlo, Consolas, monospace; font-size: 9pt; }}
table {{ border-collapse: collapse; width: 100%; margin: 8pt 0; }}
th {{ background: #dbeaf4; font-weight: bold; }}
th, td {{ border: 1px solid #9ca3af; padding: 4pt; vertical-align: top; }}
blockquote {{ margin: 8pt 0; padding: 6pt 10pt; border-left: 3pt solid #3b82a0; background: #eef6fa; }}
a {{ color: #075985; }}
</style>
</head>
<body>{body}</body>
</html>
"""


def run_soffice(soffice: str, source: Path, output_dir: Path, conversion: str,
                profile_dir: Path) -> Path:
    profile_uri = profile_dir.resolve().as_uri()
    result = subprocess.run(
        [
            soffice,
            "--headless",
            f"-env:UserInstallation={profile_uri}",
            "--convert-to",
            conversion,
            "--outdir",
            str(output_dir),
            str(source),
        ],
        check=True,
        text=True,
        capture_output=True,
        env={**os.environ, "HOME": str(profile_dir.parent)},
    )
    suffix = ".docx" if conversion.startswith("docx") else ".pdf"
    output = output_dir / (source.stem + suffix)
    if not output.is_file() or output.stat().st_size == 0:
        raise SystemExit(
            f"soffice did not create {output}: {result.stdout}\n{result.stderr}"
        )
    return output


def docx_text(path: Path) -> str:
    with zipfile.ZipFile(path) as archive:
        root = ElementTree.fromstring(archive.read("word/document.xml"))
    return " ".join(text for text in root.itertext())


def pdf_text(path: Path) -> str:
    return " ".join((page.extract_text() or "") for page in PdfReader(path).pages)


def normalize_docx(path: Path) -> None:
    normalized = path.with_suffix(".normalized.docx")
    with zipfile.ZipFile(path) as source, zipfile.ZipFile(normalized, "w") as output:
        for name in sorted(source.namelist()):
            write_zip_entry(output, name, source.read(name))
    normalized.replace(path)


def enforce_letter_page_geometry(path: Path) -> None:
    document = Document(path)
    for section in document.sections:
        section.page_width = Inches(8.5)
        section.page_height = Inches(11)
        section.top_margin = Inches(0.8)
        section.bottom_margin = Inches(0.8)
        section.left_margin = Inches(0.8)
        section.right_margin = Inches(0.8)
    document.save(path)


def normalize_pdf(path: Path, source_bytes: bytes) -> None:
    reader = PdfReader(path)
    writer = PdfWriter()
    for page in reader.pages:
        writer.add_page(page)
    writer.add_metadata({
        "/Title": "SVP Specification v1.0 RC2",
        "/Creator": "SVP RC2 artifact generator",
        "/Producer": "pypdf 6.0.0",
        "/CreationDate": "D:20260619202500Z",
        "/ModDate": "D:20260619202500Z",
    })
    identifier = hashlib.sha256(source_bytes).digest()[:16]
    writer._ID = ArrayObject([
        ByteStringObject(identifier),
        ByteStringObject(identifier),
    ])
    normalized = path.with_suffix(".normalized.pdf")
    with normalized.open("wb") as output:
        writer.write(output)
    normalized.replace(path)


def require_semantic_markers(label: str, value: str) -> None:
    normalized = normalize_text(value)
    for marker in REQUIRED_TEXT:
        if normalize_text(marker) not in normalized:
            raise SystemExit(f"{label} is missing RC2 marker: {marker}")


def write_zip_entry(archive: zipfile.ZipFile, name: str, data: bytes) -> None:
    info = zipfile.ZipInfo(name, FIXED_ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    archive.writestr(info, data, compresslevel=9)


def release_source_paths() -> list[Path]:
    fixed = [
        REPO_ROOT / "distribution/rc2-release/README.md",
        SPEC_MARKDOWN,
        SPEC_PDF,
        SPEC_DOCX,
        REPO_ROOT / "docs/SVP_v1_0_RC2_Repo_Update_Handoff.md",
        REPO_ROOT / "spec/review/SVP_v1_0_RC2_Review_Notes.md",
        REPO_ROOT / "spec/review/SVP_v1_0_RC1_to_RC2.diff",
        REPO_ROOT / "spec/companion/SVP_Model_Bundle_v1_RC2.md",
    ]
    fixed.extend(sorted((REPO_ROOT / "docs/SVP_Implementation_Phases_RC2_Update").rglob("*.md")))
    fixed.extend(sorted((REPO_ROOT / "spec/registries").glob("*.json")))
    fixed.extend(sorted((REPO_ROOT / "spec/schemas").glob("*.json")))
    return fixed


def package_name(path: Path) -> str:
    if path == REPO_ROOT / "distribution/rc2-release/README.md":
        relative = Path("README.md")
    else:
        relative = path.relative_to(REPO_ROOT)
    return f"{PACKAGE_ROOT}/{relative.as_posix()}"


def write_update_package(destination: Path) -> None:
    update_root = REPO_ROOT / "docs/SVP_Implementation_Phases_RC2_Update"
    with zipfile.ZipFile(destination, "w") as archive:
        for path in sorted(update_root.rglob("*.md")):
            name = f"SVP_Implementation_Phases_RC2_Update/{path.relative_to(update_root).as_posix()}"
            write_zip_entry(archive, name, path.read_bytes())


def write_release_package(destination: Path, update_package: Path) -> None:
    with zipfile.ZipFile(destination, "w") as archive:
        for path in release_source_paths():
            write_zip_entry(archive, package_name(path), path.read_bytes())
        write_zip_entry(
            archive,
            f"{PACKAGE_ROOT}/docs/SVP_Implementation_Phases_RC2_Update_Package.zip",
            update_package.read_bytes(),
        )


def require_zip_sources_match(archive_path: Path, update_package: Path) -> None:
    expected = {
        package_name(path): path.read_bytes()
        for path in release_source_paths()
    }
    update_package_name = (
        f"{PACKAGE_ROOT}/docs/SVP_Implementation_Phases_RC2_Update_Package.zip"
    )
    expected[update_package_name] = update_package.read_bytes()

    with zipfile.ZipFile(archive_path) as archive:
        actual_names = archive.namelist()
        if len(actual_names) != len(set(actual_names)):
            raise SystemExit(f"{archive_path} contains duplicate entries")
        if set(actual_names) != set(expected):
            missing = sorted(set(expected) - set(actual_names))
            extra = sorted(set(actual_names) - set(expected))
            raise SystemExit(
                f"{archive_path} source inventory mismatch: "
                f"missing={missing}, extra={extra}"
            )
        for name, source_bytes in expected.items():
            if archive.read(name) != source_bytes:
                raise SystemExit(
                    f"{archive_path} entry differs from tracked source: {name}"
                )

    update_root = REPO_ROOT / "docs/SVP_Implementation_Phases_RC2_Update"
    expected_updates = {
        f"SVP_Implementation_Phases_RC2_Update/{path.relative_to(update_root).as_posix()}":
            path.read_bytes()
        for path in sorted(update_root.rglob("*.md"))
    }
    with zipfile.ZipFile(update_package) as archive:
        actual_names = archive.namelist()
        if len(actual_names) != len(set(actual_names)):
            raise SystemExit(f"{update_package} contains duplicate entries")
        if set(actual_names) != set(expected_updates):
            missing = sorted(set(expected_updates) - set(actual_names))
            extra = sorted(set(actual_names) - set(expected_updates))
            raise SystemExit(
                f"{update_package} source inventory mismatch: "
                f"missing={missing}, extra={extra}"
            )
        for name, source_bytes in expected_updates.items():
            if archive.read(name) != source_bytes:
                raise SystemExit(
                    f"{update_package} entry differs from tracked source: {name}"
                )


def main() -> None:
    soffice = require_generation_environment()
    markdown_text = SPEC_MARKDOWN.read_text(encoding="utf-8")
    require_semantic_markers("RC2 Markdown", markdown_text)

    with tempfile.TemporaryDirectory(prefix="svp-rc2-artifacts-") as temporary:
        temporary_root = Path(temporary)
        html_path = temporary_root / "SVP_v1_0_RC2.html"
        html_path.write_text(render_html(markdown_text), encoding="utf-8")
        generated = temporary_root / "generated"
        generated.mkdir()
        docx = run_soffice(
            soffice,
            html_path,
            generated,
            "docx:Office Open XML Text",
            temporary_root / "lo-html-profile",
        )
        enforce_letter_page_geometry(docx)
        normalize_docx(docx)
        pdf = run_soffice(
            soffice,
            docx,
            generated,
            "pdf:writer_pdf_Export",
            temporary_root / "lo-pdf-profile",
        )
        normalize_pdf(pdf, markdown_text.encode("utf-8"))
        require_semantic_markers("generated RC2 DOCX", docx_text(docx))
        require_semantic_markers("generated RC2 PDF", pdf_text(pdf))

        update_package = temporary_root / "SVP_Implementation_Phases_RC2_Update_Package.zip"
        write_update_package(update_package)

        shutil.copyfile(docx, SPEC_DOCX)
        shutil.copyfile(pdf, SPEC_PDF)
        write_release_package(RELEASE_ZIP, update_package)
        require_zip_sources_match(RELEASE_ZIP, update_package)

    print(
        "generated, byte-synchronized, and semantically verified RC2 DOCX, "
        "PDF, and release ZIP"
    )


if __name__ == "__main__":
    main()
