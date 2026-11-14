"""Tests for reading a board's devicetree."""

from collections.abc import Callable
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
from typing import Any, NoReturn

import pytest

from esphome.components.zephyr import dts_lookup
from esphome.components.zephyr.const import (
    KEY_BOARD_DIR_CACHE,
    KEY_BOARD_EDT_CACHE,
    KEY_CPP_PATH,
    KEY_DTS_INCLUDE_PATHS,
    KEY_ZEPHYR,
)
from esphome.components.zephyr.dts_lookup import (
    _find_board_dir,
    _find_cpp,
    _find_dts_file,
    _find_revision_overlay,
    _get_dts_include_paths,
    _get_edt,
    _iter_nodes,
)
from esphome.core import CORE


@pytest.fixture(autouse=True)
def zephyr_data() -> None:
    """The devicetree caches as set_core_data leaves them."""
    CORE.data[KEY_ZEPHYR] = {
        KEY_CPP_PATH: "",
        KEY_DTS_INCLUDE_PATHS: None,
        KEY_BOARD_DIR_CACHE: {},
        KEY_BOARD_EDT_CACHE: {},
    }


@pytest.fixture
def zephyr_base(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """An installed workspace's zephyr/ folder, with the devicetree library."""
    base = tmp_path / "zephyr"
    (base / "scripts" / "dts" / "python-devicetree" / "src").mkdir(parents=True)
    monkeypatch.setattr(dts_lookup, "get_zephyr_base", lambda: base)
    return base


@pytest.fixture
def fake_zephyr_base(tmp_path: Path) -> Path:
    """A boards/ tree in Zephyr's layout, with real upstream file names: folders
    with several qualified .dts files, and names that repeat the <soc> segment."""
    boards = tmp_path / "boards"
    single = boards / "espressif" / "esp32h2_devkitm"
    single.mkdir(parents=True)
    (single / "esp32h2_devkitm.dts").write_text("/ { };")

    dual = boards / "espressif" / "esp32c6_devkitc"
    dual.mkdir(parents=True)
    (dual / "esp32c6_devkitc_lpcore.dts").write_text("/* lpcore */")
    (dual / "esp32c6_devkitc_hpcore.dts").write_text("/* hpcore */")

    rpi_pico = boards / "raspberrypi" / "rpi_pico"
    rpi_pico.mkdir(parents=True)
    (rpi_pico / "rpi_pico.dts").write_text("/* plain */")
    (rpi_pico / "rpi_pico_rp2040_mcuboot.dts").write_text("/* mcuboot */")
    (rpi_pico / "rpi_pico_rp2040_w.dts").write_text("/* w */")
    (rpi_pico / "rpi_pico_rp2040_w_mcuboot.dts").write_text("/* w mcuboot */")

    nrf54l15dk = boards / "nordic" / "nrf54l15dk"
    nrf54l15dk.mkdir(parents=True)
    for suffix in (
        "nrf54l05_cpuapp",
        "nrf54l05_cpuflpr",
        "nrf54l10_cpuapp",
        "nrf54l10_cpuapp_ns",
        "nrf54l10_cpuflpr",
        "nrf54l15_cpuapp",
        "nrf54l15_cpuapp_ns",
        "nrf54l15_cpuflpr",
        "nrf54l15_cpuflpr_xip",
    ):
        (nrf54l15dk / f"nrf54l15dk_{suffix}.dts").write_text(f"/* {suffix} */")

    feather = boards / "adafruit" / "feather_nrf52840"
    feather.mkdir(parents=True)
    (feather / "adafruit_feather_nrf52840.dts").write_text("/* plain */")
    (feather / "adafruit_feather_nrf52840_nrf52840_uf2.dts").write_text("/* uf2 */")
    (feather / "adafruit_feather_nrf52840_nrf52840_sense.dts").write_text("/* sense */")
    (feather / "adafruit_feather_nrf52840_nrf52840_sense_uf2.dts").write_text(
        "/* sense uf2 */"
    )

    rpi_pico2 = boards / "raspberrypi" / "rpi_pico2"
    rpi_pico2.mkdir(parents=True)
    (rpi_pico2 / "rpi_pico2_rp2350a_m33.dts").write_text("/* m33 */")
    (rpi_pico2 / "rpi_pico2_rp2350a_m33_mcuboot.dts").write_text("/* m33 mcuboot */")
    (rpi_pico2 / "rpi_pico2_rp2350a_m33_w.dts").write_text("/* m33 w */")
    (rpi_pico2 / "rpi_pico2_rp2350a_m33_w_mcuboot.dts").write_text(
        "/* m33 w mcuboot */"
    )

    return tmp_path


def test_find_board_dir_locates_vendor_subfolder(fake_zephyr_base: Path) -> None:
    result = _find_board_dir(fake_zephyr_base, "esp32h2_devkitm/esp32h2")
    assert result == fake_zephyr_base / "boards" / "espressif" / "esp32h2_devkitm"


def test_find_board_dir_returns_none_for_unknown_board(fake_zephyr_base: Path) -> None:
    assert _find_board_dir(fake_zephyr_base, "no_such_board/soc") is None


def test_find_board_dir_without_a_boards_folder(tmp_path: Path) -> None:
    assert _find_board_dir(tmp_path, "esp32h2_devkitm") is None


def test_find_board_dir_strips_revision_suffix(fake_zephyr_base: Path) -> None:
    result = _find_board_dir(fake_zephyr_base, "esp32h2_devkitm@1.0.0/esp32h2")
    assert result == fake_zephyr_base / "boards" / "espressif" / "esp32h2_devkitm"


def test_find_board_dir_falls_back_to_board_yml_names(tmp_path: Path) -> None:
    """Folders named without the vendor prefix are found through board.yml, in
    either of its two schemas; an unreadable file is skipped."""
    boards = tmp_path / "boards" / "arduino"
    (boards / "broken").mkdir(parents=True)
    (boards / "broken" / "board.yml").write_text("board: [")
    (boards / "listed").mkdir()
    (boards / "listed" / "board.yml").write_text("- not a mapping\n")
    (boards / "uno_r4").mkdir()
    (boards / "uno_r4" / "board.yml").write_text("boards:\n  - name: arduino_uno_r4\n")
    (boards / "giga_r1").mkdir()
    (boards / "giga_r1" / "board.yml").write_text("board:\n  name: arduino_giga_r1\n")

    assert _find_board_dir(tmp_path, "arduino_uno_r4") == boards / "uno_r4"
    assert _find_board_dir(tmp_path, "arduino_giga_r1/stm32h747xx/m7") == (
        boards / "giga_r1"
    )


@pytest.mark.parametrize("board", ["esp32h2_devkitm", "no_such_board"])
def test_find_board_dir_scans_once_per_board(
    fake_zephyr_base: Path, board: str
) -> None:
    """A found folder and a miss are both remembered; the second call reads no disk."""
    first = _find_board_dir(fake_zephyr_base, board)
    shutil.rmtree(fake_zephyr_base / "boards")
    assert _find_board_dir(fake_zephyr_base, board) == first


@pytest.mark.parametrize(
    ("folder", "board", "expected"),
    [
        ("espressif/esp32h2_devkitm", "esp32h2_devkitm/esp32h2", "esp32h2_devkitm"),
        # The base .dts never carries the revision
        (
            "espressif/esp32h2_devkitm",
            "esp32h2_devkitm@1.0.0/esp32h2",
            "esp32h2_devkitm",
        ),
        # The qualifier picks the file, not glob order
        (
            "espressif/esp32c6_devkitc",
            "esp32c6_devkitc/esp32c6/hpcore",
            "esp32c6_devkitc_hpcore",
        ),
        (
            "espressif/esp32c6_devkitc",
            "esp32c6_devkitc/esp32c6/lpcore",
            "esp32c6_devkitc_lpcore",
        ),
        # File names that repeat the <soc> segment
        ("raspberrypi/rpi_pico", "rpi_pico/rp2040/mcuboot", "rpi_pico_rp2040_mcuboot"),
        (
            "raspberrypi/rpi_pico",
            "rpi_pico_rp2040_w/rp2040/mcuboot",
            "rpi_pico_rp2040_w_mcuboot",
        ),
        ("raspberrypi/rpi_pico", "rpi_pico/rp2040", "rpi_pico"),
        # Neighbours sharing a long prefix
        (
            "nordic/nrf54l15dk",
            "nrf54l15dk/nrf54l15/cpuapp",
            "nrf54l15dk_nrf54l15_cpuapp",
        ),
        (
            "nordic/nrf54l15dk",
            "nrf54l15dk/nrf54l15/cpuapp_ns",
            "nrf54l15dk_nrf54l15_cpuapp_ns",
        ),
        (
            "nordic/nrf54l15dk",
            "nrf54l15dk/nrf54l05/cpuapp",
            "nrf54l15dk_nrf54l05_cpuapp",
        ),
        # Four and five segments
        (
            "adafruit/feather_nrf52840",
            "adafruit_feather_nrf52840/nrf52840/sense/uf2",
            "adafruit_feather_nrf52840_nrf52840_sense_uf2",
        ),
        (
            "adafruit/feather_nrf52840",
            "adafruit_feather_nrf52840/nrf52840/sense",
            "adafruit_feather_nrf52840_nrf52840_sense",
        ),
        (
            "adafruit/feather_nrf52840",
            "adafruit_feather_nrf52840/nrf52840/uf2",
            "adafruit_feather_nrf52840_nrf52840_uf2",
        ),
        (
            "raspberrypi/rpi_pico2",
            "rpi_pico2/rp2350a/m33/w/mcuboot",
            "rpi_pico2_rp2350a_m33_w_mcuboot",
        ),
        (
            "raspberrypi/rpi_pico2",
            "rpi_pico2/rp2350a/m33/mcuboot",
            "rpi_pico2_rp2350a_m33_mcuboot",
        ),
        ("raspberrypi/rpi_pico2", "rpi_pico2/rp2350a/m33", "rpi_pico2_rp2350a_m33"),
    ],
)
def test_find_dts_file(
    fake_zephyr_base: Path, folder: str, board: str, expected: str
) -> None:
    board_dir = fake_zephyr_base / "boards" / folder
    assert _find_dts_file(board_dir, board) == board_dir / f"{expected}.dts"


def test_find_dts_file_falls_back_to_the_only_dts(tmp_path: Path) -> None:
    board_dir = tmp_path / "only_board"
    board_dir.mkdir()
    (board_dir / "only_board_defconfig.dts").write_text("")
    assert _find_dts_file(board_dir, "other_name/soc") is None
    (board_dir / "renamed.dts").write_text("/* only */")
    assert _find_dts_file(board_dir, "other_name/soc") == board_dir / "renamed.dts"


@pytest.mark.parametrize(
    "qualifiers",
    [
        pytest.param([], id="1_segment"),
        pytest.param(["soc"], id="2_segments"),
        pytest.param(["soc", "cpu"], id="3_segments"),
        pytest.param(["soc", "sense", "uf2"], id="4_segments"),
        pytest.param(["soc", "m33", "w", "mcuboot"], id="5_segments"),
    ],
)
def test_find_board_dir_dts_file_and_revision_overlay_at_each_depth(
    tmp_path: Path, qualifiers: list[str]
) -> None:
    board_dir = tmp_path / "boards" / "acme" / "depth_board"
    board_dir.mkdir(parents=True)
    suffix = ("_" + "_".join(qualifiers)) if qualifiers else ""
    (board_dir / f"depth_board{suffix}.dts").write_text("/* base */")
    (board_dir / f"depth_board{suffix}_1_0_0.overlay").write_text("/* rev 1.0.0 */")
    board = "depth_board@1.0.0" + "".join(f"/{q}" for q in qualifiers)

    found_board_dir = _find_board_dir(tmp_path, board)
    assert found_board_dir == board_dir
    assert _find_dts_file(board_dir, board) == board_dir / f"depth_board{suffix}.dts"
    assert _find_revision_overlay(board_dir, board, "1.0.0") == (
        board_dir / f"depth_board{suffix}_1_0_0.overlay"
    )


def _revisioned_board_dir(base: Path) -> Path:
    """A board declaring revisions 1.4.0 and 2.0.0 (closest lower one matches),
    with one overlay per revision, in actinius_icarus's real layout."""
    board_dir = base / "boards" / "actinius" / "icarus"
    board_dir.mkdir(parents=True)
    (board_dir / "board.yml").write_text(
        "board:\n"
        "  name: actinius_icarus\n"
        "  vendor: actinius\n"
        "  revision:\n"
        "    format: major.minor.patch\n"
        "    default: '2.0.0'\n"
        "    revisions:\n"
        "      - name: '1.4.0'\n"
        "      - name: '2.0.0'\n"
    )
    (board_dir / "actinius_icarus_nrf9160_ns.dts").write_text("/* base */")
    (board_dir / "actinius_icarus_nrf9160_ns_1_4_0.overlay").write_text(
        "/* rev 1.4.0 */"
    )
    (board_dir / "actinius_icarus_nrf9160_ns_2_0_0.overlay").write_text(
        "/* rev 2.0.0 */"
    )
    return board_dir


def test_find_revision_overlay(tmp_path: Path) -> None:
    board_dir = _revisioned_board_dir(tmp_path)
    board = "actinius_icarus/nrf9160/ns"
    assert _find_revision_overlay(board_dir, board, "2.0.0") == (
        board_dir / "actinius_icarus_nrf9160_ns_2_0_0.overlay"
    )
    assert _find_revision_overlay(board_dir, board, "9.9.9") is None


def _fake_preprocess_dts_file(
    src_file: Path, zephyr_base: Path, extra_include_dirs: list[str]
) -> str:
    """Stands in for cpp: inlines each `#include "<path>"`, expands no macros."""
    parts = []
    for line in src_file.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith('#include "') and stripped.endswith('"'):
            parts.append(Path(stripped[10:-1]).read_text(encoding="utf-8"))
        else:
            parts.append(line)
    return "\n".join(parts)


def _fake_edtlib(ctor: Callable[..., object]) -> Callable[[Path], type]:
    return lambda base: type("_FakeEdtlib", (), {"EDT": staticmethod(ctor)})


@pytest.fixture
def edt_texts(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    """Record the merged devicetree text each _get_edt() call would parse."""
    seen: list[str] = []

    def _fake_edt_ctor(path: str, bindings_dirs: object, **kwargs: object) -> object:
        seen.append(Path(path).read_text(encoding="utf-8"))
        return object()

    monkeypatch.setattr(dts_lookup, "_preprocess_dts_file", _fake_preprocess_dts_file)
    monkeypatch.setattr(dts_lookup, "_load_edtlib", _fake_edtlib(_fake_edt_ctor))
    return seen


@pytest.mark.parametrize(
    ("board", "merged", "left_out"),
    [
        # 1.9.0 resolves to the closest lower declared revision, 1.4.0
        ("actinius_icarus@1.9.0/nrf9160/ns", "/* rev 1.4.0 */", "/* rev 2.0.0 */"),
        # No revision: the board's default
        ("actinius_icarus/nrf9160/ns", "/* rev 2.0.0 */", "/* rev 1.4.0 */"),
    ],
)
def test_get_edt_merges_the_revision_overlay(
    edt_texts: list[str], zephyr_base: Path, board: str, merged: str, left_out: str
) -> None:
    _revisioned_board_dir(zephyr_base)

    _get_edt(board)

    assert len(edt_texts) == 1
    assert "/* base */" in edt_texts[0]
    assert merged in edt_texts[0]
    assert left_out not in edt_texts[0]


def test_get_edt_uses_the_base_tree_for_an_unknown_revision(
    edt_texts: list[str], zephyr_base: Path
) -> None:
    _revisioned_board_dir(zephyr_base)

    _get_edt("actinius_icarus@0.1.0/nrf9160/ns")

    assert edt_texts == ["/* base */"]


def test_get_edt_without_the_revision_overlay_file(
    edt_texts: list[str], zephyr_base: Path
) -> None:
    board_dir = _revisioned_board_dir(zephyr_base)
    (board_dir / "actinius_icarus_nrf9160_ns_2_0_0.overlay").unlink()

    _get_edt("actinius_icarus/nrf9160/ns")

    assert edt_texts == ["/* base */"]


def test_get_edt_ignores_revision_when_board_has_none(
    edt_texts: list[str], zephyr_base: Path
) -> None:
    _board_tree(zephyr_base)

    _get_edt("my_board@1.0.0")

    assert edt_texts == ["/ { };"]


def test_get_edt_is_cached_per_board(edt_texts: list[str], zephyr_base: Path) -> None:
    _board_tree(zephyr_base)

    first = _get_edt("my_board")

    assert _get_edt("my_board") is first
    assert len(edt_texts) == 1


def _board_tree(base: Path) -> None:
    board = base / "boards" / "espressif" / "my_board"
    board.mkdir(parents=True)
    (board / "my_board.dts").write_text("/ { };")


def test_get_edt_warns_once_when_cpp_missing(
    monkeypatch: pytest.MonkeyPatch,
    zephyr_base: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    _board_tree(zephyr_base)
    monkeypatch.setattr(dts_lookup, "_find_cpp", lambda: None)
    monkeypatch.setattr(dts_lookup, "_load_edtlib", _fake_edtlib(lambda *a, **k: 1))

    assert _get_edt("my_board") is None
    assert _get_edt("my_board") is None

    warnings = [r for r in caplog.records if "Can't read the devicetree" in r.message]
    assert len(warnings) == 1
    assert "No working C preprocessor" in warnings[0].message


@pytest.mark.parametrize(
    ("prepare", "message"),
    [
        (lambda base: None, "no folder for it under"),
        (
            lambda base: (base / "boards" / "acme" / "my_board").mkdir(parents=True),
            "no .dts file in",
        ),
    ],
)
def test_get_edt_warns_when_the_board_files_are_missing(
    monkeypatch: pytest.MonkeyPatch,
    zephyr_base: Path,
    caplog: pytest.LogCaptureFixture,
    prepare: Callable[[Path], object],
    message: str,
) -> None:
    (zephyr_base / "boards").mkdir()
    prepare(zephyr_base)
    monkeypatch.setattr(dts_lookup, "_load_edtlib", _fake_edtlib(lambda *a, **k: 1))

    assert _get_edt("my_board") is None
    assert message in caplog.text


def test_get_edt_warns_when_the_workspace_is_missing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    monkeypatch.setattr(dts_lookup, "get_zephyr_base", lambda: tmp_path / "missing")

    assert _get_edt("my_board") is None
    assert "is not installed" in caplog.text


def test_get_edt_warns_without_the_devicetree_library(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    monkeypatch.setattr(dts_lookup, "get_zephyr_base", lambda: tmp_path)

    assert _get_edt("my_board") is None
    assert "the workspace has no devicetree library in" in caplog.text


def test_get_edt_warns_when_devicetree_parse_fails(
    monkeypatch: pytest.MonkeyPatch,
    zephyr_base: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    _board_tree(zephyr_base)
    (zephyr_base / "dts" / "bindings").mkdir(parents=True)

    def _reject(path: str, bindings_dirs: list[str], **kwargs: Any) -> NoReturn:
        assert bindings_dirs == [str(zephyr_base / "dts" / "bindings")]
        raise ValueError("unknown binding for compatible 'acme,bus'")

    monkeypatch.setattr(dts_lookup, "_preprocess_dts_file", _fake_preprocess_dts_file)
    monkeypatch.setattr(dts_lookup, "_load_edtlib", _fake_edtlib(_reject))

    assert _get_edt("my_board") is None
    assert "devicetree parse failed: unknown binding" in caplog.text


@pytest.mark.parametrize("cpp_found", [True, False])
def test_get_edt_leaves_no_temp_files(
    monkeypatch: pytest.MonkeyPatch,
    zephyr_base: Path,
    tmp_path: Path,
    cpp_found: bool,
) -> None:
    """The cpp wrapper and its output are removed, on success and on failure."""
    _board_tree(zephyr_base)
    temp_dir = tmp_path / "tmp"
    temp_dir.mkdir()
    monkeypatch.setattr(tempfile, "tempdir", str(temp_dir))
    monkeypatch.setattr(dts_lookup, "_load_edtlib", _fake_edtlib(lambda *a, **k: 1))
    if cpp_found:
        monkeypatch.setattr(
            dts_lookup, "_preprocess_dts_file", _fake_preprocess_dts_file
        )
    else:
        monkeypatch.setattr(dts_lookup, "_find_cpp", lambda: None)

    assert _get_edt("my_board") == (1 if cpp_found else None)
    assert not list(temp_dir.iterdir())


def test_load_edtlib_imports_the_workspace_copy(
    zephyr_base: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    src = zephyr_base / "scripts" / "dts" / "python-devicetree" / "src"
    (src / "devicetree").mkdir()
    (src / "devicetree" / "__init__.py").write_text("")
    (src / "devicetree" / "edtlib.py").write_text(f"WHERE = {str(src)!r}\n")
    # A copy, so the modules the import adds don't outlive the test
    monkeypatch.setattr(
        sys, "modules", {**sys.modules, "devicetree": ModuleType("devicetree")}
    )
    monkeypatch.setattr(sys, "path", list(sys.path))

    edtlib = dts_lookup._load_edtlib(zephyr_base)

    assert edtlib is not None
    assert str(src) == edtlib.WHERE
    assert str(src.resolve()) not in sys.path


def test_load_edtlib_reports_a_broken_copy(
    zephyr_base: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(sys, "path", list(sys.path))
    monkeypatch.setattr(sys, "modules", {**sys.modules, "devicetree": None})
    with pytest.raises(dts_lookup._DtsReadError, match="can't load edtlib from .*: "):
        dts_lookup._load_edtlib(zephyr_base)


def test_load_edtlib_without_the_workspace_copy(tmp_path: Path) -> None:
    with pytest.raises(dts_lookup._DtsReadError, match="has no devicetree library"):
        dts_lookup._load_edtlib(tmp_path)


def test_preprocess_dts_file_runs_cpp_with_the_include_folders(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    seen: list[list[str]] = []

    def _fake_run(cmd: list[str], **kwargs: Any) -> SimpleNamespace:
        seen.append(cmd)
        return SimpleNamespace(stdout="/ { };")

    monkeypatch.setattr(dts_lookup, "_find_cpp", lambda: "/usr/bin/cpp")
    monkeypatch.setattr(dts_lookup, "_get_dts_include_paths", lambda base: ["/inc"])
    monkeypatch.setattr(dts_lookup.subprocess, "run", _fake_run)

    src = tmp_path / "board.dts"
    assert dts_lookup._preprocess_dts_file(src, tmp_path, ["/board"]) == "/ { };"
    assert seen == [
        [
            "/usr/bin/cpp",
            "-x",
            "assembler-with-cpp",
            "-nostdinc",
            "-E",
            "-P",
            "-I",
            "/inc",
            "-I",
            "/board",
            str(src),
        ]
    ]


@pytest.mark.parametrize(
    ("stderr", "message"),
    [
        ("first\nboard.dts:3: error", "cpp failed: board.dts:3: error"),
        ("", "no output"),
    ],
)
def test_preprocess_dts_file_reports_the_cpp_error(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, stderr: str, message: str
) -> None:
    def _fail(cmd: list[str], **kwargs: Any) -> NoReturn:
        raise subprocess.CalledProcessError(1, cmd, stderr=stderr)

    monkeypatch.setattr(dts_lookup, "_find_cpp", lambda: "/usr/bin/cpp")
    monkeypatch.setattr(dts_lookup, "_get_dts_include_paths", lambda base: [])
    monkeypatch.setattr(dts_lookup.subprocess, "run", _fail)
    with pytest.raises(dts_lookup._DtsReadError, match=message):
        dts_lookup._preprocess_dts_file(tmp_path / "board.dts", tmp_path, [])


def _timed_out_run(cmd: list[str], **kwargs: Any) -> NoReturn:
    raise subprocess.TimeoutExpired(cmd, kwargs["timeout"])


def test_preprocess_dts_file_timeout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(dts_lookup, "_find_cpp", lambda: "/usr/bin/cpp")
    monkeypatch.setattr(dts_lookup, "_get_dts_include_paths", lambda base: [])
    monkeypatch.setattr(dts_lookup.subprocess, "run", _timed_out_run)
    with pytest.raises(dts_lookup._DtsReadError, match="cpp timed out"):
        dts_lookup._preprocess_dts_file(tmp_path / "board.dts", tmp_path, [])


def test_find_cpp_skips_broken_candidate_picks_working_one(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        dts_lookup.shutil, "which", {"cpp": "/usr/bin/cpp", "gcc": "/usr/bin/gcc"}.get
    )
    monkeypatch.setattr(
        dts_lookup.subprocess,
        "run",
        lambda cmd, **kwargs: SimpleNamespace(returncode=1 if "cpp" in cmd[0] else 0),
    )
    assert _find_cpp() == "/usr/bin/gcc"


def test_find_cpp_returns_none_when_every_candidate_fails(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(dts_lookup.shutil, "which", lambda name: f"/usr/bin/{name}")
    monkeypatch.setattr(
        dts_lookup.subprocess,
        "run",
        lambda cmd, **kwargs: SimpleNamespace(returncode=1),
    )
    assert _find_cpp() is None


def test_find_cpp_caches_first_working_candidate(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[str] = []

    def _fake_run(cmd: list[str], **kwargs: Any) -> SimpleNamespace:
        calls.append(cmd[0])
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(dts_lookup.shutil, "which", lambda name: f"/usr/bin/{name}")
    monkeypatch.setattr(dts_lookup.subprocess, "run", _fake_run)

    assert _find_cpp() == "/usr/bin/cpp"
    assert _find_cpp() == "/usr/bin/cpp"
    assert calls == ["/usr/bin/cpp"]


def test_cpp_candidate_check_uses_real_command(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    seen: list[list[str]] = []

    def _fake_run(cmd: list[str], **kwargs: Any) -> SimpleNamespace:
        seen.append(cmd)
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(dts_lookup.subprocess, "run", _fake_run)

    assert dts_lookup._cpp_candidate_works("/usr/bin/cpp")
    assert "-nostdinc" in seen[0]
    assert seen[0][-1].endswith(".dts")


def test_cpp_candidate_check_missing_binary() -> None:
    assert not dts_lookup._cpp_candidate_works("/nonexistent/cpp")


def test_cpp_candidate_check_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(dts_lookup.subprocess, "run", _timed_out_run)
    assert not dts_lookup._cpp_candidate_works("/usr/bin/cpp")


def test_dts_include_paths_cover_zephyr_and_the_hal_modules(tmp_path: Path) -> None:
    """Board files #include the SoC files in the vendor's HAL module."""
    base = tmp_path / "zephyr"
    for sub in ("dts/arm", "dts/bindings", "dts/common"):
        (base / sub).mkdir(parents=True)
    (base / "dts" / "README").write_text("")
    (tmp_path / "modules" / "hal" / "stm32" / "dts").mkdir(parents=True)
    (tmp_path / "modules" / "hal" / "cmsis").mkdir(parents=True)

    paths = _get_dts_include_paths(base)

    assert paths[:2] == [str(base / "include"), str(base / "dts")]
    assert sorted(paths[2:]) == sorted(
        [
            str(base / "dts" / "arm"),
            str(base / "dts" / "common"),
            str(tmp_path / "modules" / "hal" / "stm32" / "dts"),
        ]
    )
    # Built once per build
    assert _get_dts_include_paths(tmp_path / "other") is paths


def test_dts_include_paths_of_a_bare_tree(tmp_path: Path) -> None:
    assert _get_dts_include_paths(tmp_path) == [
        str(tmp_path / "include"),
        str(tmp_path / "dts"),
    ]


def test_iter_nodes_flattens_scc_order() -> None:
    edt = SimpleNamespace(scc_order=["a", ["b", "c"], "d"])
    assert list(_iter_nodes(edt)) == ["a", "b", "c", "d"]
