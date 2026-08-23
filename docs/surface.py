"""The published surface, written down, so that moving it is a diff.

The scorecard's stability item asks for the tool that tells a reviewer
the public surface moved, before a user finds out. C++ has no such tool
in the toolchain. A compiler is perfectly happy to see a method deleted,
a parameter added or a `noexcept` dropped, and the person who finds out
is whoever upgrades and rebuilds. What the other clients do about that
is keep the surface in a file: zu-node has an api-extractor report,
zu-go has an api/surface.txt in the shape of the api/go1.N.txt files the
language itself is held to. This is the same thing for a header.

`api/surface.txt` is every name `include/zu.hpp` publishes and the shape
it publishes it in, one per line, sorted, and it is generated rather
than written:

    cmake --build build --target surface-update

It is reviewed like any other file. The check below rebuilds it from the
header and fails when the two disagree, saying which names went, which
arrived and which changed shape, because those are three different
pieces of news. A name that arrived is a minor release. A name that went
or changed shape is a major one, or a mistake, and the point of the gate
is that a reviewer is told which they are looking at while it is still a
diff.

It reads the XML Doxygen already writes for the reference rather than
parsing C++ itself, which is the whole reason it is a page of selection
rules and not a compiler front end. Doxygen has already decided what is
public, what `zu::detail` hides and what the preprocessor kept, and
having one answer to those questions instead of two is worth more than
independence here: the reference and the surface disagreeing about what
this header publishes would be a bug in its own right.

That does leave one thing to say out loud. Doxygen extracts a member
only if somebody documented it, so a member with no comment on it is
missing from the XML and would be missing from here, which is a name
leaving the file without leaving the header. What catches that is the
other gate: WARN_IF_UNDOCUMENTED fires and reference.py fails on it.
The two are a pair, and turning either one off makes the other quieter
than it looks.

Two things it deliberately does not do. It does not know the ABI: this
is a header-only wrapper, everything in it is inline, and what a caller
depends on is whether their source still compiles rather than whether an
object file still links. And it says nothing about behaviour, so a
signature that held still while its meaning changed passes here and is
caught, if it is caught, by the suite that runs the thing.

    python3 docs/surface.py <Doxyfile> [--update]
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import reference

#: Where the surface is written down, relative to the top of the
#: repository. Beside the source rather than under docs/, because it is
#: a reviewed file and not a generated artifact of the documentation.
GOLDEN = "api/surface.txt"

#: What separates a name from its type, and a class from what it
#: derives from. Both mean "and here is the rest of it", and both are
#: where the identity of a line stops. See `key`.
ARROW = " -> "
BASE = " : "

#: The private struct and the `friend class X` grants are not a surface;
#: they are how the pieces reach each other. Doxygen marks both, so this
#: is a filter and not a list to keep up to date.
PUBLIC = ("public", "protected")


def text(node: ElementTree.Element | None) -> str:
    """One XML node flattened to the text it renders as.

    Doxygen writes a type as a mix of text and `<ref>` elements, one ref
    per name it could link, so the type of a member is spread across
    children rather than sitting in one string.
    """
    if node is None:
        return ""
    return collapse("".join(node.itertext()))


def collapse(s: str) -> str:
    """Whitespace flattened, so reflowing a declaration is not a change.

    A signature written across four lines in the header and the same
    signature written across one are one entry here.
    """
    return re.sub(r"\s+", " ", s).strip()


def entry(kind: str, name: str, tail: str = "") -> tuple[str, str]:
    """One line of the file, and the name it is filed under.

    The name is what the file is sorted by, so that everything about
    `zu::Connection` is in one block: a diff is read by whoever is
    changing a class, and a file sorted by kind would scatter that class
    across nine places.
    """
    return name, collapse(f"{kind} {name}{tail}")


def parameters(member: ElementTree.Element) -> str:
    """The parameter list, by type, with the names taken out.

    A parameter name is documentation in C++ and nothing else: there is
    no call that names one, so renaming it cannot break a caller and
    should not read as a change to the surface. A default argument is
    the opposite, since taking one away breaks every call that leaned on
    it, so it stays.
    """
    parts = []
    for param in member.findall("param"):
        one = text(param.find("type"))
        default = text(param.find("defval"))
        if default:
            one += f" = {default}"
        parts.append(one)
    return "(" + ", ".join(parts) + ")"


def qualifiers(member: ElementTree.Element) -> str:
    """The part after the parameter list, and it is load bearing.

    `const` decides whether a caller with a const handle can call it.
    `noexcept` is part of the type since C++17, so dropping one breaks
    anybody who took the address. `= delete` is a promise that a call
    does not compile, and turning it back into a definition is a change
    to the surface in the direction nobody notices. The ref qualifier
    decides whether a temporary can call it at all.

    All of it is in one string Doxygen already assembled, so this takes
    it from after the closing bracket rather than rebuilding it out of
    the attributes and getting the order wrong.
    """
    args = text(member.find("argsstring"))
    after = args[args.rfind(")") + 1 :] if ")" in args else ""
    # `=default` and `=delete` arrive without the spaces a person writes.
    return re.sub(r"\s*=\s*(default|delete)", r" = \1", after).rstrip()


def template(member: ElementTree.Element) -> str:
    """The template parameter list, written after the name.

    C++ writes it before, and a report that did the same would sort
    every template away from the class it belongs to, because these
    lines are grouped by the name they publish. So `get<class T>` here
    stands for `template <class T> ... get`. It is a report and not a
    declaration; zu-go's says `method Reader.Read func(...)` for the
    same reason.
    """
    params = member.find("templateparamlist")
    if params is None:
        return ""
    written = []
    for param in params:
        one = text(param.find("type"))
        # Doxygen puts the parameter's name in the type for `class T`
        # and in a declname of its own for `class... Ts`, so asking for
        # both and taking the name once is the only way to get `Ts`
        # rather than `class... Ts Ts`.
        declared = collapse(param.findtext("declname") or "")
        if declared and not one.endswith(declared):
            one += f" {declared}"
        written.append(one)
    return "<" + ", ".join(written) + ">"


def leading(member: ElementTree.Element) -> list[str]:
    """The keywords C++ writes in front of a declaration.

    `explicit` is the one that earns this function. Taking it off a one
    argument constructor compiles everywhere it did before and starts
    accepting conversions nobody wrote, and putting it on breaks every
    call that leaned on one, which is a break a compiler reports at the
    call site and this file reports at review.

    `virtual` and `constexpr` are here for the day one of them is used
    rather than because one is: a gate that only names the properties
    the header happens to have today is a gate that stops noticing the
    moment somebody adds one.
    """
    out = []
    if member.get("static") == "yes":
        out.append("static")
    if member.get("explicit") == "yes":
        out.append("explicit")
    if member.get("constexpr") == "yes":
        out.append("constexpr")
    if member.get("virt") in ("virtual", "pure-virtual"):
        out.append(member.get("virt", "").replace("-", " "))
    return out


def member_entry(member: ElementTree.Element) -> tuple[str, str] | None:
    """One member of a class, a struct or a namespace, as a line."""
    if member.get("prot") not in PUBLIC:
        return None

    kind = member.get("kind")
    name = collapse(member.findtext("qualifiedname") or member.findtext("name") or "")
    if not name:
        return None
    kind_type = text(member.find("type"))

    if kind in ("function", "friend"):
        # A friend defined inside a class is not a member of it. It is
        # found by argument dependent lookup, which is exactly how a
        # caller writes `a == b`, so it is published and it is named
        # apart from the members so that a reader is not misled about
        # where it lives.
        lead = " ".join(["friend" if kind == "friend" else "func"] + leading(member))
        signature = f"{template(member)}{parameters(member)}{qualifiers(member)}"
        # A constructor and a destructor have no return type, and an
        # arrow to nothing would read as one that went missing.
        return entry(lead, name, signature + (ARROW + kind_type if kind_type else ""))

    if kind == "variable":
        lead = "var static" if member.get("static") == "yes" else "var"
        return entry(lead, name, ARROW + kind_type)

    if kind == "typedef":
        return entry("using", name, template(member) + ARROW + kind_type)

    if kind == "define":
        # A macro a caller writes `#if` against. Its replacement is not
        # part of the surface: `ZU_HAS_EXPECTED` being 1 or 0 is what
        # the compiler decided, not what this header promises.
        return entry("define", name)

    return None


def enum_entries(member: ElementTree.Element) -> list[tuple[str, str]]:
    """An enum and every enumerator in it.

    The enumerators are listed one per line rather than as a set on the
    enum's line, because dropping one is the change this is here to
    catch and a set would bury it in a line that already changed for
    some other reason. Their initializers are on them: these are the
    `ZU_` constants of the C ABI under another spelling, and an
    enumerator quietly taking a different value is worse news than one
    disappearing.
    """
    if member.get("prot") not in PUBLIC:
        return []
    name = collapse(member.findtext("qualifiedname") or member.findtext("name") or "")
    lines = [entry("enum", name, ARROW + text(member.find("type")))]
    for value in member.findall("enumvalue"):
        if value.get("prot") not in PUBLIC:
            continue
        one = collapse(value.findtext("name") or "")
        init = text(value.find("initializer")).removeprefix("=").strip()
        lines.append(entry("enum value", f"{name}::{one}", ARROW + init))
    return lines


def compound_entry(compound: ElementTree.Element) -> tuple[str, str] | None:
    """A class or a struct, with what it derives from."""
    if compound.get("prot") not in PUBLIC:
        return None
    kind = compound.get("kind")
    if kind not in ("class", "struct"):
        return None
    name = collapse(compound.findtext("compoundname") or "")
    bases = []
    for base in compound.findall("basecompoundref"):
        prot = base.get("prot") or "public"
        bases.append(f"{prot} {collapse(''.join(base.itertext()))}")
    tail = BASE + ", ".join(bases) if bases else ""
    return entry(kind, name, tail)


def undefined(header: Path) -> set[str]:
    """The macros the header takes back before it ends.

    A macro that is `#undef`ed is not published, however defined it was
    in the middle. `ZU_FORMATTER` is the one: it exists for ten lines at
    the foot of this header and is gone by the closing guard, so a
    caller cannot write it and it is not a name this repository owes
    anybody. The specializations it made are published, and those are
    counted in `formatters` instead.
    """
    return {
        match.group(1)
        for line in header.read_text(encoding="utf-8").splitlines()
        if (match := re.match(r"#undef\s+(\w+)", line.strip()))
    }


def formatters(header: Path) -> list[tuple[str, str]]:
    """The std::formatter specializations, read out of the header.

    These are the one thing the XML cannot answer. The Doxyfile expands
    `ZU_FORMATTER` to nothing, deliberately, so that ten macro
    invocations do not arrive in the reference as ten classes nobody
    declared. But `std::format("{}", status)` is a call a user writes,
    and a specialization that stopped being generated is a surface that
    moved, so the invocations are counted here instead.
    """
    found = []
    for line in header.read_text(encoding="utf-8").splitlines():
        match = re.match(r"ZU_FORMATTER\((.+)\);", line.strip())
        if match:
            found.append(entry("formatter", f"std::formatter< {match.group(1)} >"))
    return found


def surface(xml: Path, header: Path) -> list[str]:
    """Every name this header publishes, sorted and without duplicates."""
    index = xml / "index.xml"
    if not index.is_file():
        raise SystemExit(f"doxygen wrote no index at {index}")

    found: list[tuple[str, str]] = []
    taken_back = undefined(header)
    for compound in ElementTree.parse(index).getroot():
        refid = compound.get("refid") or ""
        page = xml / f"{refid}.xml"
        if not page.is_file():
            continue
        for definition in ElementTree.parse(page).iter("compounddef"):
            # A directory is a compound to Doxygen and publishes
            # nothing. The file is where the macros live, and it is also
            # where every other name in the header turns up a second
            # time, so it is read for those and left alone otherwise.
            if definition.get("kind") == "dir":
                continue
            # A nested type that is private publishes nothing, and
            # neither do its members, however public they are inside it.
            # `Connection::Watch` is the case: the fields of it are
            # public to the class that owns it and there is no caller
            # anywhere who can name the type.
            if definition.get("kind") in ("class", "struct"):
                one = compound_entry(definition)
                if one is None:
                    continue
                found.append(one)
            wanted = ("define",) if definition.get("kind") == "file" else None
            for member in definition.iter("memberdef"):
                if wanted and member.get("kind") not in wanted:
                    continue
                if member.get("kind") == "define" and member.findtext("name") in taken_back:
                    continue
                if member.get("kind") == "enum":
                    found.extend(enum_entries(member))
                    continue
                one = member_entry(member)
                if one:
                    found.append(one)

    # Before the formatters, which are read out of the header and would
    # be ten names standing in an otherwise empty file. Doxygen exits 0
    # on a header it extracted nothing from, so an empty walk is not an
    # empty header, it is a Doxyfile that threw the file away.
    if not found:
        raise SystemExit(
            f"nothing at all was read out of {xml}, which is what an empty reference "
            "looks like rather than what a header looks like. Check PREDEFINED in the "
            "Doxyfile for the include guard."
        )

    lines = [line for _, line in sorted(set(found + formatters(header)))]

    # `compare` files both surfaces by key, so two lines sharing one
    # would mean one of them was never compared with anything and could
    # leave without a word. That cannot happen for a header a compiler
    # accepted, since two declarations with the same key are two
    # declarations a call could not choose between, so this is a check
    # on this file rather than on the header.
    seen: dict[str, str] = {}
    for line in lines:
        clash = seen.setdefault(key(line), line)
        if clash != line:
            raise SystemExit(
                f"two published names are filed under the same key, so one of them would "
                f"not be compared with anything:\n  {clash}\n  {line}\nkey: {key(line)}"
            )
    return lines


def key(line: str) -> str:
    """What identifies a line, which is not the whole of it.

    Two surfaces are compared by identity so that a thing which changed
    reads as one name changing rather than as one name leaving and a
    different one arriving. That distinction is the whole value of the
    gate: a reviewer who has to read every line of a diff to find out
    whether anything went is a reviewer who stops reading it.

    For a function, identity is what decides which overload a call
    picks, and that is the name and the types of the parameters and the
    const or ref qualifier. Everything else on the line is a property of
    the one function: the return type, `noexcept`, `= delete`, and the
    default arguments, each of which can change while the same call site
    still resolves to the same thing. Those read as a change, which is
    what they are.

    A parameter added is a different overload and reads as one going and
    one arriving. That is a difference from zu-go's file, which keys on
    the name alone, and the reason for it is that C++ has overloads:
    `to_string` is ten functions here, and a key that could not tell
    them apart would report nine of them as gone the day the tenth
    moved.

    Everywhere else identity is the name and the shape it is declared
    with. Everything after the first arrow is the type of the name in
    front of it, and for a class everything after the first colon is
    what it derives from.
    """
    head = line.split(ARROW, 1)[0].split(BASE, 1)[0]
    close = head.rfind(")")
    if close < 0:
        return head
    name, params = head[: head.find("(")], head[head.find("(") + 1 : close]
    # A default argument is a property of the declaration and not of the
    # overload: taking one away breaks callers and does not change which
    # function a call that passed the argument was picking.
    params = re.sub(r"\s*=\s*[^,]+", "", params)
    kept = [word for word in head[close + 1 :].split() if word in ("const", "&", "&&")]
    return collapse(f"{name}({params}) {' '.join(kept)}")


def compare(was: list[str], now: list[str]) -> tuple[list[str], list[str], list[tuple[str, str]]]:
    """Three answers, because they are three different pieces of news."""
    before = {key(line): line for line in was}
    after = {key(line): line for line in now}
    gone = [before[k] for k in before if k not in after]
    arrived = [after[k] for k in after if k not in before]
    changed = [(before[k], after[k]) for k in before if k in after and before[k] != after[k]]
    return sorted(gone), sorted(arrived), sorted(changed)


def report(gone: list[str], arrived: list[str], changed: list[tuple[str, str]]) -> None:
    """What moved, in the order a reviewer cares about it."""
    for line in gone:
        print(f"  went     {line}", file=sys.stderr)
    for old, new in changed:
        print(f"  was      {old}", file=sys.stderr)
        print(f"  is now   {new}", file=sys.stderr)
    for line in arrived:
        print(f"  arrived  {line}", file=sys.stderr)
    if gone or changed:
        print(
            "\nA name that went or changed shape breaks a caller who was using it, "
            f"which is a major version or a mistake. Regenerate {GOLDEN} with\n"
            "  cmake --build build --target surface-update\n"
            "and say in the pull request which of the two this is.",
            file=sys.stderr,
        )
    elif arrived:
        print(
            f"\nOnly new names, which is a minor version. Regenerate {GOLDEN} with\n"
            "  cmake --build build --target surface-update",
            file=sys.stderr,
        )


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3) or (len(argv) == 3 and argv[2] != "--update"):
        print(f"usage: {argv[0]} <Doxyfile> [--update]", file=sys.stderr)
        return 2
    update = len(argv) == 3

    doxyfile = Path(argv[1]).resolve()
    where = reference.settings(doxyfile)
    header = Path(where["INPUT"])
    output = Path(where["OUTPUT_DIRECTORY"])

    # The reference is rebuilt rather than found, so that this answers
    # about the header on disk and never about an XML tree left behind
    # by a checkout that has since moved on. Doxygen's own complaints
    # are reference.py's to fail on; what is needed here is the XML.
    reference.build(doxyfile)

    now = surface(output / "xml", header)

    # From this script rather than from the Doxyfile, because the
    # Doxyfile is configured into the build tree and every path in it
    # points there, and this one file is the repository's.
    golden = Path(__file__).resolve().parent.parent / GOLDEN
    if update:
        golden.parent.mkdir(parents=True, exist_ok=True)
        golden.write_text("\n".join(now) + "\n", encoding="utf-8")
        print(f"{len(now)} published names written to {golden}")
        return 0

    if not golden.is_file():
        print(f"there is no {golden} to check against", file=sys.stderr)
        return 1

    was = golden.read_text(encoding="utf-8").splitlines()
    gone, arrived, changed = compare(was, now)
    if not (gone or arrived or changed):
        print(f"{len(now)} published names, and {GOLDEN} says the same")
        return 0

    print(
        f"the surface moved: {len(gone)} went, {len(changed)} changed shape, "
        f"{len(arrived)} arrived",
        file=sys.stderr,
    )
    report(gone, arrived, changed)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
