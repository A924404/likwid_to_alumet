# LIKWID metrics extractor

`extract_likwid_metrics.py` downloads the selected branch of the official LIKWID repository, discovers every directory under `groups/`, and regenerates `likwid_metrics.json` atomically.

## Usage

```sh
python3 extract_likwid_metrics.py
python3 extract_likwid_metrics.py --branch master --output likwid_metrics.json --verbose
```

Use `--repo` for a fork or mirror and `--keep-workdir` to retain the checkout for inspection. Git, network access, and Python 3 are required. A failed download or parse returns a non-zero status and leaves the previous JSON untouched.

## Architecture detection

The script parses LIKWID's `src/includes/topology.h` model constants and the `topology_setName` switch in `src/topology.c`. The generated JSON includes the source model constants, numeric model IDs, LIKWID display names, and the architecture code used by `groups/<code>`.

## Metric selection and schema

FLOPS files are discovered by the `FLOPS*.txt` pattern, and memory files by `MEM(?:[0-9]+)?.txt` (`MEM.txt`, `MEM1.txt`, and so on). Each `EVENTSET` line is parsed as one event/counter pair, and each `METRICS` line is parsed as one metric name, bracketed unit, and formula.

The `architectures` object is nested first by numeric CPU family, then by an array of architecture records. For example, iterating `architectures["6"]` reaches the family-6 architecture records, and the record with `definitions.architecture_abbreviation == "SPR"` is Sapphire Rapids. Each architecture record contains `definitions` and `metrics.flops` / `metrics.memory_volume`. `definitions` contains the shared LIKWID `family_constant`, every associated `models` entry, `architecture_full_name`, and `architecture_abbreviation`. LIKWID assigns the full name and abbreviation together with the architecture/group mapping, so they describe the whole architecture record rather than individual models. Each model entry contains `model_name`, `model_code`, and any conditional mapping information.

Unresolved dependencies are retained as warnings rather than silently changing formulas. Formulas are never evaluated.

The generated `source` object records repository, branch, commit SHA, and generation time. Run the script again to refresh all architectures and replace the JSON from scratch.

## References
Visit the following LIKWID wiki pages for more information:
* [How LIKWID saves FLOPS and memory volume?](https://github.com/RRZE-HPC/likwid/wiki/likwid-perfctr/f36667c84871fe9193ce076931d1ea98817cad20?#defining-custom-performance-groups)
* [Where to get LIKWID architecture info?](https://github.com/RRZE-HPC/likwid/wiki/AddX86Support)