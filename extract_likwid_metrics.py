#!/usr/bin/env python3
"""Extract FLOPS and memory-volume groups from the LIKWID source tree."""

from __future__ import annotations

import argparse
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_REPO = "https://github.com/RRZE-HPC/likwid.git"
DEFAULT_BRANCH = "master"
FLOPS_FILE_RE = re.compile(r"^FLOPS(?:_[A-Za-z0-9]+)?\.txt$")
MEMORY_FILE_RE = re.compile(r"^MEM(?:[0-9]+)?\.txt$")
MEMORY_NAME_RE = re.compile(r"\b(memory|dram)\b", re.IGNORECASE)
MEMORY_VOLUME_RE = re.compile(r"\b(volume|data volume)\b", re.IGNORECASE)
EXCLUDED_MEMORY_RE = re.compile(r"\b(cache|cpi|latency|utili[sz]|miss|hit)\b", re.IGNORECASE)
FLOP_WORD_RE = re.compile(r"FLOP", re.IGNORECASE)
UNITS_SUFFIX_RE = re.compile(r"\s+\[([^\]]+)\]$")
SCALE_FACTOR_RE = re.compile(r"\b1(?:\.0+)?[Ee]-0*[69]\s*\*\s*")
TIME_DIVISION_RE = re.compile(r"/\s*time", re.IGNORECASE)
ASSUMED_ANNOTATION_RE = re.compile(r"\([^()]*\bassumed\b\)\s*", re.IGNORECASE)
FLOPS_UNITS = "FLOP"
MEMORY_UNITS = "Bytes"


@dataclass
class GroupMetric:
    name: str
    units: str
    formula: str
    source_file: str
    source_architecture_directory: str
    events: dict[str, str] = field(default_factory=dict)
    unresolved_dependencies: list[str] = field(default_factory=list)

    def as_dict(self) -> dict[str, object]:
        result: dict[str, object] = {
            "name": self.name,
            "units": self.units,
            "formula": self.formula,
            "events": self.events,
        }
        if self.unresolved_dependencies:
            result["unresolved_dependencies"] = self.unresolved_dependencies
        return result


@dataclass
class ParsedGroup:
    eventset: dict[str, str]
    metrics: list[tuple[str, str]]


def download_likwid_source(repo: str, branch: str, keep_workdir: bool) -> tuple[Path, str, Path | None]:
    """Clone the requested branch into a temporary directory and return its commit."""
    workdir = Path(tempfile.mkdtemp(prefix="likwid-source-"))
    try:
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", branch, repo, str(workdir / "likwid")],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        checkout = workdir / "likwid"
        commit = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        logging.info("Downloaded LIKWID commit %s", commit)
        if keep_workdir:
            return checkout, commit, workdir
        return checkout, commit, None
    except (OSError, subprocess.CalledProcessError) as exc:
        if not keep_workdir:
            shutil.rmtree(workdir, ignore_errors=True)
        detail = getattr(exc, "stderr", "") or str(exc)
        raise RuntimeError(f"Could not retrieve LIKWID from {repo}: {detail.strip()}") from exc


def parse_likwid_architecture_mapping(source_root: Path) -> list[dict[str, object]]:
    """Parse LIKWID's model constants and topology_setName source mapping.

    The mapping intentionally comes from LIKWID source rather than a Python table.
    LIKWID's ``short_name`` is the architecture code used by the groups directory.
    """
    header = (source_root / "src/includes/topology.h").read_text(encoding="utf-8")
    topology = (source_root / "src/topology.c").read_text(encoding="utf-8")
    constants = {
        name: int(value, 0)
        for name, value in re.findall(r"^\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)U?\s*$", header, re.MULTILINE)
    }
    name_values = dict(re.findall(r'static const char \*([A-Za-z0-9_]+)\s*=\s*"([^"]+)";', topology))
    mappings: dict[str, dict[str, object]] = {}
    function_match = re.search(r"int topology_setName\(void\)(.*?)(?=\nint |\nvoid )", topology, re.DOTALL)
    if not function_match:
        raise ValueError("LIKWID topology_setName function was not found")
    pending_cases: list[str] = []
    previous_cases: list[str] = []
    pending_display_name = ""
    branch_condition: str | None = None
    inverse_condition: str | None = None
    family_constant: str | None = None
    for line in function_match.group(1).splitlines():
        family_case_match = re.match(r"\s{4}case\s+(\w+)\s*:", line)
        if family_case_match:
            family_constant = family_case_match.group(1)
        case_match = re.search(r"\bcase\s+(\w+)\s*:", line)
        if case_match:
            if not pending_cases:
                branch_condition = None
                inverse_condition = None
            pending_cases.append(case_match.group(1))
        condition_match = re.search(r"if\s*\(\s*cpuid_info\.stepping\s*([<>]=?)\s*(\d+)\s*\)", line)
        if condition_match:
            operator, threshold = condition_match.groups()
            branch_condition = f"stepping {operator} {threshold}"
            inverse_operator = ">=" if operator in {"<", "<="} else "<"
            inverse_condition = f"stepping {inverse_operator} {threshold}"
        elif re.search(r"\belse\b", line) and branch_condition:
            branch_condition, inverse_condition = inverse_condition, branch_condition
        name_match = re.search(r"\bname\s*=\s*(\w+)\s*;", line)
        if name_match:
            pending_display_name = name_values.get(name_match.group(1), "")
        short_match = re.search(r"\bshort_name\s*=\s*(\w+)\s*;", line)
        if not short_match:
            continue
        cases = pending_cases or previous_cases
        code = name_values.get(short_match.group(1))
        if code:
            entry = mappings.setdefault(code, {"architecture_code": code, "model_definitions": [], "architecture_names": []})
            for constant in cases:
                if constant in constants:
                    definition = {
                        "family_constant": family_constant,
                        "family": constants.get(family_constant) if family_constant else None,
                        "model_name": constant,
                        "model_code": constants[constant],
                    }
                    if branch_condition:
                        definition["condition"] = branch_condition
                    if definition not in entry["model_definitions"]:
                        entry["model_definitions"].append(definition)
            if pending_display_name and pending_display_name not in entry["architecture_names"]:
                entry["architecture_names"].append(pending_display_name)
        previous_cases = list(cases)
        pending_cases = []
        pending_display_name = ""
    return sorted(mappings.values(), key=lambda item: str(item["architecture_code"]))


def _clean_group_lines(text: str) -> list[str]:
    lines = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("-"):
            continue
        lines.append(line)
    return lines


def parse_eventset_line(line: str) -> tuple[str, str] | None:
    """Parse an EVENTSET line as exactly ``counter event_name``."""
    fields = line.split()
    if len(fields) != 2:
        return None
    return fields[0], fields[1]


def parse_group_file(path: Path) -> ParsedGroup:
    """Parse one event/value pair or metric name/unit/formula per section line."""
    section = ""
    eventset: dict[str, str] = {}
    metrics: list[tuple[str, str]] = []
    for line in _clean_group_lines(path.read_text(encoding="utf-8")):
        upper = line.upper()
        if upper in {"EVENTSET", "METRICS", "LONG"}:
            section = upper
            continue
        if upper.startswith("FORMULAS:"):
            section = "LONG"
            continue
        if section == "EVENTSET":
            event = parse_eventset_line(line)
            if event is not None:
                counter, event_name = event
                eventset[counter] = event_name
        elif section == "METRICS":
            metric_match = re.match(r"^(.+?)\s+(\[[^\]]+\])\s+(.+)$", line)
            if metric_match:
                metrics.append((f"{metric_match.group(1).strip()} {metric_match.group(2)}", metric_match.group(3).strip()))
    return ParsedGroup(eventset=eventset, metrics=metrics)


def _is_flops_metric(name: str, formula: str) -> bool:
    return bool(FLOP_WORD_RE.search(f"{name} {formula}"))


def _is_memory_volume_metric(name: str, formula: str) -> bool:
    combined = f"{name} {formula}"
    return bool(MEMORY_NAME_RE.search(combined) and MEMORY_VOLUME_RE.search(combined) and not EXCLUDED_MEMORY_RE.search(combined))


def split_metric_name(name: str) -> tuple[str, str]:
    match = UNITS_SUFFIX_RE.search(name)
    if not match:
        raise ValueError(f"Metric name has no trailing units: {name}")
    return name[:match.start()].rstrip(), match.group(1)


def normalize_formula(formula: str) -> str:
    formula = SCALE_FACTOR_RE.sub("", formula)
    formula = TIME_DIVISION_RE.sub("", formula)
    return ASSUMED_ANNOTATION_RE.sub("", formula)


def resolve_metric_dependencies(formula: str, eventset: dict[str, str]) -> tuple[dict[str, str], list[str]]:
    """Resolve eventset counters and event names referenced by a formula."""
    counters = {
        counter
        for counter in eventset
        if re.search(rf"\b{re.escape(counter)}\b", formula)
    }
    event_to_counters: dict[str, list[str]] = {}
    for counter, event in eventset.items():
        event_name = event.split()[0]
        event_to_counters.setdefault(event_name, []).append(counter)
    for event_name in event_to_counters:
        if re.search(rf"\b{re.escape(event_name)}\b", formula):
            counters.update(event_to_counters[event_name])
    events = {counter: eventset[counter] for counter in sorted(counters) if counter in eventset}
    return events, []


def select_metrics(group: ParsedGroup, architecture: str, source_file: str, flops: bool) -> list[GroupMetric]:
    selected = []
    for name, formula in group.metrics:
        wanted = _is_flops_metric(name, formula) if flops else _is_memory_volume_metric(name, formula)
        if not wanted:
            continue
        name, _source_units = split_metric_name(name)
        units = FLOPS_UNITS if flops else MEMORY_UNITS
        formula = normalize_formula(formula)
        events, unresolved = resolve_metric_dependencies(formula, group.eventset)
        selected.append(GroupMetric(name, units, formula, source_file, architecture, events, unresolved))
    return selected


def build_architecture_records(source_root: Path, mappings: list[dict[str, object]]) -> dict[str, list[dict[str, object]]]:
    records: dict[str, list[dict[str, object]]] = {}
    groups = source_root / "groups"
    for directory in sorted(path for path in groups.iterdir() if path.is_dir()):
        flops: list[GroupMetric] = []
        memory: list[GroupMetric] = []
        for path in sorted(directory.iterdir()):
            if not path.is_file():
                continue
            try:
                parsed = parse_group_file(path) if MEMORY_FILE_RE.match(path.name) or FLOPS_FILE_RE.match(path.name) else None
            except (OSError, UnicodeDecodeError) as exc:
                logging.warning("%s/%s: could not parse group file: %s", directory.name, path.name, exc)
                continue
            if parsed is None:
                continue
            if MEMORY_FILE_RE.match(path.name):
                memory.extend(select_metrics(parsed, directory.name, path.name, False))
            else:
                flops.extend(select_metrics(parsed, directory.name, path.name, True))
        if not flops and not memory:
            continue
        mapping = next((item for item in mappings if item["architecture_code"] == directory.name), None)
        if mapping is None:
            logging.warning("Could not resolve architecture code for groups/%s", directory.name)
            continue
        model_definitions = mapping["model_definitions"]
        primary_definition = model_definitions[0] if model_definitions else None
        if primary_definition is None:
            logging.warning("Could not resolve CPU family for groups/%s", directory.name)
            continue
        family_key = str(primary_definition["family"])
        family_constant = primary_definition["family_constant"]
        output_model_definitions = [
            {key: value for key, value in definition.items() if key not in {"family_constant", "family"}}
            for definition in model_definitions
        ]
        family_records = records.setdefault(family_key, [])
        full_names = mapping["architecture_names"]
        family_records.append({
            "definitions": {
                "family_constant": family_constant,
                "models": output_model_definitions,
                "architecture_full_name": full_names[0] if full_names else None,
                "architecture_abbreviation": directory.name,
            },
            "metrics": {"flops": [metric.as_dict() for metric in flops], "memory_volume": [metric.as_dict() for metric in memory]},
        })
        logging.info("Architecture: %s (FLOPS metrics: %d, memory-volume metrics: %d)", directory.name, len(flops), len(memory))
    return records


def validate_output(data: dict[str, object]) -> None:
    for family_code, family_architectures in data["architectures"].items():
        if not isinstance(family_architectures, list):
            raise ValueError(f"Family {family_code} is not an architecture list")
        for architecture in family_architectures:
            architecture_code = architecture.get("definitions", {}).get("architecture_abbreviation", "unknown")
            definitions = architecture.get("definitions")
            if not isinstance(definitions, dict):
                raise ValueError(f"Architecture {family_code}/{architecture_code} has no definitions")
            for key in ("family_constant", "models", "architecture_full_name", "architecture_abbreviation"):
                if key not in definitions:
                    raise ValueError(f"Architecture {family_code}/{architecture_code} has no definitions.{key}")
            for category in ("flops", "memory_volume"):
                for metric in architecture["metrics"][category]:
                    for key in ("name", "units", "formula", "events"):
                        if key not in metric or not isinstance(metric[key], (str, dict)):
                            raise ValueError(f"Invalid {category} metric in {family_code}/{architecture_code}: {key}")
                    if not metric["units"]:
                        raise ValueError(f"Empty units in {family_code}/{architecture_code}/{metric['name']}")
                    if not isinstance(metric["formula"], str) or not metric["formula"]:
                        raise ValueError(f"Empty formula in {family_code}/{architecture_code}/{metric['name']}")
                    if metric.get("unresolved_dependencies"):
                        logging.warning("%s/%s/%s has unresolved dependencies: %s", family_code, architecture_code, metric["name"], metric["unresolved_dependencies"])


def write_json(data: dict[str, object], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent, text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(data, handle, indent=2, sort_keys=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, output)
    except Exception:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=DEFAULT_REPO, help="LIKWID Git repository URL")
    parser.add_argument("--branch", default=DEFAULT_BRANCH, help="Git branch or tag to retrieve")
    parser.add_argument("--output", type=Path, default=Path("likwid_metrics.json"), help="Output JSON path")
    parser.add_argument("--keep-workdir", action="store_true", help="Keep the downloaded checkout and print its path")
    parser.add_argument("--verbose", action="store_true", help="Enable debug logging")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(levelname)s: %(message)s")
    workdir: Path | None = None
    try:
        logging.info("Downloading LIKWID source...")
        source_root, commit, workdir = download_likwid_source(args.repo, args.branch, args.keep_workdir)
        directories = [path for path in (source_root / "groups").iterdir() if path.is_dir()]
        logging.info("Found %d architecture directories.", len(directories))
        mappings = parse_likwid_architecture_mapping(source_root)
        data = {
            "source": {"repository": args.repo, "groups_path": "groups", "branch": args.branch, "commit": commit, "generated_at": datetime.now(timezone.utc).isoformat()},
            "architectures": build_architecture_records(source_root, mappings),
        }
        validate_output(data)
        logging.info("Writing %s...", args.output)
        write_json(data, args.output)
        logging.info("Done.")
        if args.keep_workdir:
            logging.info("Source checkout kept at %s", workdir)
        return 0
    except (OSError, RuntimeError, ValueError, KeyError) as exc:
        logging.error("%s", exc)
        return 1
    finally:
        if workdir is None and 'source_root' in locals():
            shutil.rmtree(source_root.parent, ignore_errors=True)




if __name__ == "__main__":
    sys.exit(main())