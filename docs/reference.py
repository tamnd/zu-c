"""The generated reference, and the check that it is a reference.

A reference written by hand beside a header is wrong by the second
release and wrong in the expensive way, because it looks maintained. So
this one is generated out of `include/zu.hpp`, and the release publishes
what this produced rather than a copy somebody kept in step.

The interesting failure is the quiet one. Doxygen only reads `/**` and
`/*!`, and with EXTRACT_ALL off it has nothing to say about a file it
found nothing extractable in: no pages, no warnings, exit status 0. That
is not a hypothetical. A `ZU_HPP` left in PREDEFINED defined this
header's own include guard, the preprocessor dropped the whole file, and
the build was green and empty for a day.

WARN_IF_UNDOCUMENTED cannot catch that, because there is nothing there
to be undocumented. So the gate reads the header a second time and by
different means: every class and struct `zu.hpp` declares at namespace
scope has to turn up in the XML Doxygen wrote. One grep and one parser
disagreeing is what an empty reference looks like from the outside.

    python3 docs/reference.py build/docs/Doxyfile

The Doxyfile is the configured one in the build tree, and the paths come
out of it rather than being passed again, so there is one place that
says where the header is and where the reference goes.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

#: The tags this script reads back out of the Doxyfile. Everything else
#: in there is Doxygen's business.
WANTED = ("INPUT", "OUTPUT_DIRECTORY", "WARN_LOGFILE")


def settings(doxyfile: Path) -> dict[str, str]:
    """The handful of paths the Doxyfile says, read from the Doxyfile.

    A crude parse on purpose: these three tags are one line each and are
    written that way two directories up. Continuations and += are
    Doxygen's to understand and are not needed to find out where it was
    told to put its output.
    """
    found: dict[str, str] = {}
    for line in doxyfile.read_text(encoding="utf-8").splitlines():
        match = re.match(r"\s*(\w+)\s*=\s*(.*?)\s*$", line)
        if match and match.group(1) in WANTED:
            found[match.group(1)] = match.group(2).strip('"')
    missing = [tag for tag in WANTED if tag not in found]
    if missing:
        raise SystemExit(f"{doxyfile} says nothing about {', '.join(missing)}")
    return found


def declared(header: Path) -> list[str]:
    """The types `zu.hpp` declares for a caller, read out of the source.

    Namespace scope and column zero, which in this header is the only
    place a public type is defined; the nested iterators are indented
    inside the class that owns them and are documented on its page.

    Two things are skipped. A line ending in a semicolon is a forward
    declaration and the definition is further down. Everything between
    `namespace detail {` and the comment that closes it is the
    machinery, which the reference excludes and a caller does not name.
    """
    names = []
    inside_detail = False
    for line in header.read_text(encoding="utf-8").splitlines():
        if line.startswith("namespace detail {"):
            inside_detail = True
            continue
        if line.startswith("}  // namespace detail"):
            inside_detail = False
            continue
        if inside_detail or line.rstrip().endswith(";"):
            continue
        match = re.match(r"(?:class|struct)\s+(\w+)\b", line)
        if match:
            names.append(match.group(1))
    return names


def documented(xml: Path) -> set[str]:
    """The classes and structs the reference has a page for.

    Read off the XML index rather than the HTML, because the file names
    Doxygen gives its pages are mangled and the index is where it says
    in plain text what it wrote about.
    """
    index = xml / "index.xml"
    if not index.is_file():
        raise SystemExit(f"doxygen wrote no index at {index}")
    found = set()
    for compound in ElementTree.parse(index).getroot():
        if compound.get("kind") in ("class", "struct"):
            name = compound.findtext("name") or ""
            found.add(name.removeprefix("zu::"))
    return found


def build(doxyfile: Path) -> str:
    """Run Doxygen over a clean output directory, answer its warnings.

    Clean rather than merged, because a page for a class that was
    renamed is worse than no page: it is a name a reader can still find,
    still in the search index built beside it, and gone from the header.

    Doxygen splits what it has to say in two. What it thinks of the
    Doxyfile goes to stderr, and that is where a note about a tag some
    other version of Doxygen spells differently turns up, so it is
    passed through for a person to read rather than failed on. What it
    thinks of the source goes to the log file, and that is the half this
    repository is answerable for.
    """
    # Resolved, because Doxygen is run from the output directory and a
    # path somebody typed from the top of the repository stops meaning
    # what it meant the moment the working directory changes.
    doxyfile = doxyfile.resolve()
    where = settings(doxyfile)
    output = Path(where["OUTPUT_DIRECTORY"])
    warnings = Path(where["WARN_LOGFILE"])
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    warnings.parent.mkdir(parents=True, exist_ok=True)
    warnings.unlink(missing_ok=True)

    doxygen = shutil.which("doxygen")
    if doxygen is None:
        raise SystemExit("doxygen is not on PATH, and it is what builds the reference")
    run = subprocess.run([doxygen, str(doxyfile)], cwd=output.parent, check=False)
    if run.returncode != 0:
        raise SystemExit(f"doxygen exited {run.returncode}")
    return warnings.read_text(encoding="utf-8") if warnings.is_file() else ""


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <Doxyfile>", file=sys.stderr)
        return 2

    doxyfile = Path(argv[1])
    where = settings(doxyfile)
    header = Path(where["INPUT"])
    output = Path(where["OUTPUT_DIRECTORY"])

    complaints = build(doxyfile)

    wanted = declared(header)
    absent = [name for name in wanted if name not in documented(output / "xml")]
    pages = sorted(output.glob("html/*.html"))

    if absent:
        print(
            f"{len(absent)} of the {len(wanted)} types {header.name} declares have no page: "
            + ", ".join(absent),
            file=sys.stderr,
        )
        if len(absent) == len(wanted):
            print(
                "all of them, which is what an empty reference looks like. "
                "Check PREDEFINED in the Doxyfile for the include guard.",
                file=sys.stderr,
            )
    if complaints:
        print(complaints, end="", file=sys.stderr)
        print("doxygen had that much to say about the header", file=sys.stderr)

    print(f"{len(pages)} pages in {output / 'html'}, {len(wanted)} types")
    return 1 if absent or complaints else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
