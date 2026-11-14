"""Read a board's devicetree from the installed Zephyr workspace."""

from collections.abc import Iterator
import contextlib
import importlib
import logging
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import ModuleType
from typing import Any, Final

import yaml

from esphome.core import CORE, EsphomeError

from .board_revision import default_revision, parse_board_string, resolve_revision
from .const import (
    KEY_BOARD_DIR_CACHE,
    KEY_BOARD_EDT_CACHE,
    KEY_CPP_PATH,
    KEY_DTS_INCLUDE_PATHS,
    KEY_ZEPHYR,
)
from .framework import get_zephyr_base

_LOGGER = logging.getLogger(__name__)

_NOT_FOUND: Final = object()


def _load_edtlib(zephyr_base: Path) -> ModuleType:
    """Import Zephyr's own copy of edtlib; the PyPI devicetree package is older and
    rejects binding keys newer Zephyr versions add. Raises _DtsReadError."""
    bundled = (zephyr_base / "scripts" / "dts" / "python-devicetree" / "src").resolve()
    if not bundled.is_dir():
        raise _DtsReadError(f"the workspace has no devicetree library in {bundled}")
    # Another workspace's copy may be loaded already
    for key in [
        k for k in sys.modules if k == "devicetree" or k.startswith("devicetree.")
    ]:
        del sys.modules[key]
    sys.path.insert(0, str(bundled))
    importlib.invalidate_caches()
    try:
        return importlib.import_module("devicetree.edtlib")
    except ImportError as exc:
        raise _DtsReadError(f"can't load edtlib from {bundled}: {exc}") from exc
    finally:
        # Only needed for the import; the modules stay loaded
        with contextlib.suppress(ValueError):
            sys.path.remove(str(bundled))


def _get_edt(board: str) -> Any | None:
    """The parsed devicetree of ``board``, or None when it can't be read; cached per
    board, so the preprocessor runs once per board and build."""
    cache = CORE.data[KEY_ZEPHYR][KEY_BOARD_EDT_CACHE]
    if board in cache:
        result = cache[board]
        return None if result is _NOT_FOUND else result

    try:
        edt = _build_edt(board, get_zephyr_base())
    except _DtsReadError as err:
        _LOGGER.warning(
            "Can't read the devicetree of board '%s': %s",
            board,
            err,
        )
        cache[board] = _NOT_FOUND
        return None
    cache[board] = edt
    return edt


class _DtsReadError(Exception):
    """Why a board's devicetree couldn't be read."""


def _build_edt(board: str, zephyr_base: Path) -> Any:
    """Parse ``board``'s devicetree with its revision overlay."""
    if not zephyr_base.is_dir():
        raise _DtsReadError(f"the Zephyr workspace {zephyr_base} is not installed")

    edtlib = _load_edtlib(zephyr_base)

    board_dir = _find_board_dir(zephyr_base, board)
    if board_dir is None:
        raise _DtsReadError(f"no folder for it under {zephyr_base / 'boards'}")

    dts_file = _find_dts_file(board_dir, board)
    if dts_file is None:
        raise _DtsReadError(f"no .dts file in {board_dir}")

    # One preprocessor run over every file, as Zephyr's build does: an overlay's
    # macros may only be defined by the base tree's own #includes
    raw_files = [dts_file]

    requested_revision = parse_board_string(board).revision or default_revision(
        board_dir
    )
    if requested_revision is not None:
        resolved_revision, declares_revisions = resolve_revision(
            board_dir, requested_revision
        )
        if declares_revisions and resolved_revision is not None:
            overlay_file = _find_revision_overlay(board_dir, board, resolved_revision)
            if overlay_file is not None:
                raw_files.append(overlay_file)
            else:
                _LOGGER.debug(
                    "No revision overlay found for '%s' (revision '%s')",
                    board,
                    resolved_revision,
                )
        elif declares_revisions:
            _LOGGER.debug(
                "Revision '%s' is not one board '%s' declares; using the base tree",
                requested_revision,
                board,
            )

    with tempfile.NamedTemporaryFile(
        suffix=".dts", mode="w", delete=False, encoding="utf-8"
    ) as f:
        f.write("\n".join(f'#include "{path}"' for path in raw_files))
        wrapper_path = Path(f.name)

    try:
        preprocessed = _preprocess_dts_file(wrapper_path, zephyr_base, [str(board_dir)])
    finally:
        wrapper_path.unlink(missing_ok=True)

    with tempfile.NamedTemporaryFile(
        suffix=".dts", mode="w", delete=False, encoding="utf-8"
    ) as f:
        f.write(preprocessed)
        tmp_path = Path(f.name)

    try:
        bindings_dir = zephyr_base / "dts" / "bindings"
        bindings_dirs = [str(bindings_dir)] if bindings_dir.is_dir() else []
        return edtlib.EDT(
            str(tmp_path), bindings_dirs, warn_reg_unit_address_mismatch=False
        )
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        raise _DtsReadError(f"devicetree parse failed: {exc}") from exc
    finally:
        tmp_path.unlink(missing_ok=True)


def _find_board_dir(zephyr_base: Path, board: str) -> Path | None:
    """``board``'s folder under boards/, or None.

    Falls back to the ``name:`` in each board.yml, since some vendors name the folder
    without the vendor prefix the board name has.
    """
    cache = CORE.data[KEY_ZEPHYR][KEY_BOARD_DIR_CACHE]
    if board in cache:
        cached = cache[board]
        return Path(cached) if cached else None

    # The folder has the bare board name, without "@<revision>" or "/<qualifiers>"
    board_dirname = parse_board_string(board).name
    boards_root = zephyr_base / "boards"

    result = None
    if boards_root.is_dir():
        for parent in boards_root.iterdir():
            if (candidate := parent / board_dirname).is_dir():
                result = candidate
                break

        if result is None:
            # Recursive: some vendors add a category folder under their own
            for board_yml in boards_root.glob("*/**/board.yml"):
                try:
                    doc = yaml.safe_load(board_yml.read_text(encoding="utf-8"))
                except (OSError, yaml.YAMLError):
                    continue
                if not isinstance(doc, dict):
                    continue
                # Older board.yml files have one "board:", newer ones a "boards:" list
                entries = [doc["board"]] if "board" in doc else doc.get("boards", [])
                if any(
                    isinstance(entry, dict) and entry.get("name") == board_dirname
                    for entry in entries
                ):
                    result = board_yml.parent
                    break

    cache[board] = str(result) if result is not None else ""
    return result


def _find_qualified_file(board_dir: Path, board: str, suffix: str) -> Path | None:
    """``board``'s file ending in ``suffix``, trying less qualified names in turn.

    A board name is "<board>[/<soc>[/<variant>...]]". The file name joins the
    segments with "_", but drops <soc> when the board folder already holds it
    (esp32c6_devkitc/esp32c6/hpcore -> esp32c6_devkitc_hpcore.dts), so the full
    name is tried first, then without <soc>, then the bare board name. Base files
    never carry "@<revision>"; only revision overlays do.
    """
    parts = board.split("/")
    board_dirname = parts[0].split("@", 1)[0]
    rest = parts[1:]

    if rest:
        qualified = board_dir / f"{board_dirname}_{'_'.join(rest)}{suffix}"
        if qualified.exists():
            return qualified

    if len(rest) > 1:
        qualified = board_dir / f"{board_dirname}_{'_'.join(rest[1:])}{suffix}"
        if qualified.exists():
            return qualified

    exact = board_dir / f"{board_dirname}{suffix}"
    if exact.exists():
        return exact

    return None


def _find_revision_overlay(
    board_dir: Path, board: str, resolved_revision: str
) -> Path | None:
    """The overlay of ``board`` at ``resolved_revision``, if there is one; its file
    name adds the revision, dots as underscores, as one more segment."""
    parts = parse_board_string(board)
    revision_segment = resolved_revision.replace(".", "_")
    qualifiers = parts.qualifiers or ""  # starts with "/" when set
    synthetic = f"{parts.name}{qualifiers}/{revision_segment}"
    return _find_qualified_file(board_dir, synthetic, ".overlay")


def _find_dts_file(board_dir: Path, board: str) -> Path | None:
    """``board``'s .dts file; a folder can hold several, one per qualifier."""
    found = _find_qualified_file(board_dir, board, ".dts")
    if found is not None:
        return found

    candidates = [
        f for f in board_dir.glob("*.dts") if not f.name.endswith("_defconfig.dts")
    ]
    return candidates[0] if candidates else None


def _cpp_command(cpp: str, include_dirs: list[str], src_file: Path) -> list[str]:
    """The preprocessor command, shared by the real run and the candidate check."""
    cmd = [cpp, "-x", "assembler-with-cpp", "-nostdinc", "-E", "-P"]
    for d in include_dirs:
        cmd += ["-I", d]
    cmd.append(str(src_file))
    return cmd


def _preprocess_dts_file(
    src_file: Path, zephyr_base: Path, extra_include_dirs: list[str]
) -> str:
    """Run the C preprocessor over ``src_file`` the way Zephyr's devicetree build
    does. Raises _DtsReadError on failure."""
    cpp = _find_cpp()
    if cpp is None:
        raise _DtsReadError("No working C preprocessor (cpp, gcc or cc) found in PATH")

    include_dirs = _get_dts_include_paths(zephyr_base) + extra_include_dirs
    cmd = _cpp_command(cpp, include_dirs, src_file)

    try:
        result = subprocess.run(  # noqa: S603
            cmd, capture_output=True, text=True, check=True, timeout=30
        )
    except subprocess.CalledProcessError as exc:
        lines = (exc.stderr or "").strip().splitlines()
        raise _DtsReadError(
            f"cpp failed: {lines[-1] if lines else 'no output'}"
        ) from exc
    except subprocess.TimeoutExpired as exc:
        raise _DtsReadError("cpp timed out") from exc
    return result.stdout


def _cpp_candidate_works(path: str) -> bool:
    """On macOS, cpp is a clang alias that fails this command with a file argument."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "check.dts"
        src.write_text("")
        try:
            result = subprocess.run(  # noqa: S603
                _cpp_command(path, [], src),
                capture_output=True,
                check=False,
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired):
            return False
    return result.returncode == 0


def _find_cpp() -> str | None:
    """The first working cpp, gcc or cc on PATH, or None; found once per build."""
    zd = CORE.data[KEY_ZEPHYR]
    if zd[KEY_CPP_PATH] == "":
        found = None
        for name in ("cpp", "gcc", "cc"):
            path = shutil.which(name)
            if path is not None and _cpp_candidate_works(path):
                found = path
                break
        zd[KEY_CPP_PATH] = found
    return zd[KEY_CPP_PATH]


def _get_dts_include_paths(zephyr_base: Path) -> list[str]:
    """The preprocessor's include folders, found once per build."""
    zd = CORE.data[KEY_ZEPHYR]
    if zd[KEY_DTS_INCLUDE_PATHS] is None:
        dts_dir = zephyr_base / "dts"
        paths = [str(zephyr_base / "include"), str(dts_dir)]
        if dts_dir.is_dir():
            paths.extend(
                str(subdir)
                for subdir in dts_dir.iterdir()
                if subdir.is_dir() and subdir.name != "bindings"
            )
        # Some vendors (STM32, NXP, ...) keep SoC and pin devicetree files in their
        # HAL module, which board files #include directly
        hal_dir = zephyr_base.parent / "modules" / "hal"
        if hal_dir.is_dir():
            paths.extend(
                str(vendor_dts)
                for vendor_dir in hal_dir.iterdir()
                if (vendor_dts := vendor_dir / "dts").is_dir()
            )
        zd[KEY_DTS_INCLUDE_PATHS] = paths
    return zd[KEY_DTS_INCLUDE_PATHS]


def _iter_nodes(edt: Any) -> Iterator[Any]:
    """The nodes of ``edt``; its scc_order holds lists of nodes as well as nodes."""
    for scc in edt.scc_order:
        if isinstance(scc, list):
            yield from scc
        else:
            yield scc


def _lookup_bus_labels_by_property_match(
    board: str, property_label: str
) -> list[str] | None:
    """Labels of the nodes whose binding says ``bus: <property_label>``, enabled
    ones first, or None when the devicetree can't be read; vendors name these nodes
    too differently for a label pattern."""
    edt = _get_edt(board)
    if edt is None:
        return None

    enabled: list[str] = []
    disabled: list[str] = []
    for node in _iter_nodes(edt):
        if property_label in node.buses and node.labels:
            (enabled if node.status == "okay" else disabled).append(node.labels[0])

    result = sorted(set(enabled)) + sorted(set(disabled) - set(enabled))
    _LOGGER.debug(
        "Nodes with bus '%s' on board '%s': %s", property_label, board, result
    )
    return result


def get_console_uart_label(board: str) -> str | None:
    """The label of the board's ``zephyr,console`` UART, which is UART0, or None."""
    edt = _get_edt(board)
    if edt is None:
        return None
    node = edt.chosen_node("zephyr,console")
    if node is None or not node.labels:
        return None
    return node.labels[0]


def get_uart_controller_labels(board: str) -> list[str] | None:
    """The board's UART labels, enabled ones first, or None when the devicetree
    can't be read."""
    return _lookup_bus_labels_by_property_match(board, "uart")


def _discover_uart_node_labels(board: str) -> dict[str, str] | None:
    """UART0, UART1, ... mapped to the board's labels. UART0 is the console; the
    others follow by label, enabled ones first, so they are not the hardware
    numbers and can shift when a board file enables another UART."""
    console = get_console_uart_label(board)
    if console is None:
        return None
    others = [
        label for label in (get_uart_controller_labels(board) or []) if label != console
    ]
    mapping = {"UART0": console}
    mapping.update({f"UART{i + 1}": label for i, label in enumerate(others)})
    return mapping


def resolve_uart_node_label(board: str, hw_uart: str) -> str:
    """The devicetree label of ``hw_uart`` ("UART0", "UART1", ...) on ``board``.
    Runs during code generation, so it raises EsphomeError."""
    mapping = _discover_uart_node_labels(board)
    if mapping is None:
        raise EsphomeError(
            f"Cannot determine the console UART for board '{board}': its devicetree "
            "has no zephyr,console UART, or couldn't be read (see the warning above)"
        )
    label = mapping.get(hw_uart)
    if label is None:
        others = [v for k, v in mapping.items() if k != "UART0"]
        raise EsphomeError(
            f"Board '{board}' has no '{hw_uart}': besides its console "
            f"({mapping['UART0']}), its devicetree has {len(others)} other "
            f"UART(s): {', '.join(others) or 'none'}"
        )
    _LOGGER.info("%s on board '%s' is %s", hw_uart, board, label)
    return label


def get_watchdog_node_label(board: str) -> tuple[str | None, bool]:
    """``(label, already_working)`` for the board's watchdog. Found by its binding
    being in Zephyr's bindings/watchdog/ folder, so every vendor is covered;
    already_working is True when an enabled node already has the watchdog0 alias."""
    edt = _get_edt(board)
    if edt is None:
        return None, False
    for node in _iter_nodes(edt):
        if "watchdog0" in node.aliases and node.status == "okay":
            return (node.labels[0] if node.labels else None), True
    for node in _iter_nodes(edt):
        binding_path = node.binding_path
        if (
            binding_path
            and Path(binding_path).parent.name == "watchdog"
            and node.labels
        ):
            return node.labels[0], False
    return None, False


def _find_node_label_by_compat(
    board: str, compat: str, *, enabled_only: bool
) -> str | None:
    """The label of the first node whose ``compatible`` has ``compat``, or None.
    ``enabled_only=False`` finds nodes the SoC file ships disabled, such as STM32's RNG."""
    edt = _get_edt(board)
    if edt is None:
        return None
    for node in _iter_nodes(edt):
        if (
            compat in node.compats
            and node.labels
            and (not enabled_only or node.status == "okay")
        ):
            return node.labels[0]
    return None


def get_rng_node_label(board: str, compat: str = "st,stm32-rng") -> str | None:
    """The label of the board's ``compat`` RNG node, enabled or not, or None."""
    return _find_node_label_by_compat(board, compat, enabled_only=False)
