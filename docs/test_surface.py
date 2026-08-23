"""The cases that decide whether the stability gate says anything.

docs/surface.py answers three questions about two surfaces: what went,
what arrived, and what changed shape while keeping its name. Every one
of those answers is a judgement about which parts of a C++ declaration
are its identity and which are its shape, and getting that wrong does
not make the gate fail. It makes it pass, or makes it report a rename of
a parameter as forty names leaving and forty arriving, which is a gate a
reviewer learns to skip and is the same thing.

So the judgements are the tests. There is no Doxygen here and no header:
these run on XML written out by hand, small enough to read, because what
is being checked is the rule and not the parse.

    python3 -m unittest discover -s docs -p 'test_*.py'

stdlib unittest rather than pytest, because the docs job installs
Doxygen and a Python and nothing else, and a gate that needed a package
manager to run is a gate that is off on the machine that needed it.
"""

from __future__ import annotations

import unittest
import xml.etree.ElementTree as ElementTree

import surface


def member(xml: str) -> ElementTree.Element:
    """One memberdef, written out."""
    return ElementTree.fromstring(xml)


def line(xml: str) -> str:
    """The line surface.py would file that member under."""
    filed = surface.member_entry(member(xml))
    assert filed is not None, "the member was not published at all"
    return filed[1]


class TestOneMemberAsALine(unittest.TestCase):
    """What a declaration comes out as, on the cases that carry news."""

    def test_a_plain_method(self):
        self.assertEqual(
            line("""
              <memberdef kind="function" prot="public" static="no" const="no">
                <type>Result</type>
                <name>query</name>
                <qualifiedname>zu::Connection::query</qualifiedname>
                <argsstring>(std::string_view q)</argsstring>
                <param><type>std::string_view</type><declname>q</declname></param>
              </memberdef>"""),
            "func zu::Connection::query(std::string_view) -> Result",
        )

    def test_a_parameter_name_is_not_published(self):
        """There is no call that names one, so renaming one breaks nobody."""
        named = line("""
          <memberdef kind="function" prot="public" static="no">
            <type>void</type><name>f</name><qualifiedname>zu::f</qualifiedname>
            <argsstring>(int rows)</argsstring>
            <param><type>int</type><declname>rows</declname></param>
          </memberdef>""")
        renamed = line("""
          <memberdef kind="function" prot="public" static="no">
            <type>void</type><name>f</name><qualifiedname>zu::f</qualifiedname>
            <argsstring>(int n)</argsstring>
            <param><type>int</type><declname>n</declname></param>
          </memberdef>""")
        self.assertEqual(named, renamed)

    def test_a_default_argument_is(self):
        """Taking one away breaks every call that leaned on it."""
        self.assertIn(
            "bool = false",
            line("""
              <memberdef kind="function" prot="public" static="no">
                <type>Transaction</type><name>transaction</name>
                <qualifiedname>zu::Connection::transaction</qualifiedname>
                <argsstring>(bool read_only=false)</argsstring>
                <param>
                  <type>bool</type><declname>read_only</declname>
                  <defval>false</defval>
                </param>
              </memberdef>"""),
        )

    def test_const_and_noexcept_survive_from_the_argsstring(self):
        self.assertEqual(
            line("""
              <memberdef kind="function" prot="public" static="no" const="yes" noexcept="yes">
                <type>zu_conn *</type><name>raw</name>
                <qualifiedname>zu::Connection::raw</qualifiedname>
                <argsstring>() const noexcept</argsstring>
              </memberdef>"""),
            "func zu::Connection::raw() const noexcept -> zu_conn *",
        )

    def test_deleted_is_written_the_way_a_person_writes_it(self):
        self.assertEqual(
            line("""
              <memberdef kind="function" prot="public" static="no">
                <type></type><name>Transaction</name>
                <qualifiedname>zu::Transaction::Transaction</qualifiedname>
                <argsstring>(const Transaction &amp;)=delete</argsstring>
                <param><type>const Transaction &amp;</type></param>
              </memberdef>"""),
            "func zu::Transaction::Transaction(const Transaction &) = delete",
        )

    def test_a_constructor_has_no_arrow(self):
        """An arrow to nothing reads as a return type that went missing."""
        self.assertNotIn(
            surface.ARROW,
            line("""
              <memberdef kind="function" prot="public" static="no" explicit="yes">
                <type></type><name>Connection</name>
                <qualifiedname>zu::Connection::Connection</qualifiedname>
                <argsstring>(zu_conn *c) noexcept</argsstring>
                <param><type>zu_conn *</type><declname>c</declname></param>
              </memberdef>"""),
        )

    def test_explicit_and_static_are_written_down(self):
        """Both decide what a call site is allowed to be."""
        self.assertTrue(
            line("""
              <memberdef kind="function" prot="public" static="yes" explicit="no">
                <type>Connection</type><name>open</name>
                <qualifiedname>zu::Connection::open</qualifiedname>
                <argsstring>(std::string_view path)</argsstring>
                <param><type>std::string_view</type><declname>path</declname></param>
              </memberdef>""").startswith("func static ")
        )
        self.assertTrue(
            line("""
              <memberdef kind="function" prot="public" static="no" explicit="yes">
                <type></type><name>Value</name>
                <qualifiedname>zu::Value::Value</qualifiedname>
                <argsstring>(zu_value *v)</argsstring>
                <param><type>zu_value *</type><declname>v</declname></param>
              </memberdef>""").startswith("func explicit ")
        )

    def test_a_template_parameter_is_named_once(self):
        """Doxygen writes `class T` one way and `class... Ts` another."""
        self.assertEqual(
            line("""
              <memberdef kind="function" prot="public" static="no">
                <templateparamlist>
                  <param><type>class... Ts</type><declname>Ts</declname></param>
                </templateparamlist>
                <type>Appender &amp;</type><name>row</name>
                <qualifiedname>zu::Appender::row</qualifiedname>
                <argsstring>(const Ts &amp;... v)</argsstring>
                <param><type>const Ts &amp;...</type></param>
              </memberdef>"""),
            "func zu::Appender::row<class... Ts>(const Ts &...) -> Appender &",
        )

    def test_a_private_member_is_not_published(self):
        self.assertIsNone(
            surface.member_entry(member("""
              <memberdef kind="function" prot="private" static="no">
                <type>void</type><name>close</name>
                <qualifiedname>zu::Connection::close</qualifiedname>
                <argsstring>()</argsstring>
              </memberdef>"""))
        )

    def test_a_reflowed_declaration_is_the_same_line(self):
        """A signature broken across four lines and the same one across
        one are one entry, so reformatting the header is not news."""
        self.assertEqual(
            line("""
              <memberdef kind="function" prot="public" static="no">
                <type>void</type><name>on_progress</name>
                <qualifiedname>zu::Connection::on_progress</qualifiedname>
                <argsstring>(std::chrono::milliseconds every,
                             Progress watcher)</argsstring>
                <param><type>std::chrono::milliseconds</type></param>
                <param><type>Progress</type></param>
              </memberdef>"""),
            "func zu::Connection::on_progress(std::chrono::milliseconds, Progress) -> void",
        )


class TestAnEnumIsItsValuesToo(unittest.TestCase):
    """Every enumerator on a line of its own, with what it is equal to.

    These are the ZU_ constants of the C ABI under another spelling, and
    one of them quietly taking a different number is worse news than one
    disappearing.
    """

    SOURCE = """
      <memberdef kind="enum" prot="public">
        <type>int</type><name>Status</name><qualifiedname>zu::Status</qualifiedname>
        <enumvalue prot="public"><name>ok</name><initializer>= ZU_OK</initializer></enumvalue>
        <enumvalue prot="public"><name>io</name><initializer>= ZU_IO</initializer></enumvalue>
      </memberdef>"""

    def test_the_enum_and_each_value(self):
        self.assertEqual(
            [written for _, written in surface.enum_entries(member(self.SOURCE))],
            [
                "enum zu::Status -> int",
                "enum value zu::Status::ok -> ZU_OK",
                "enum value zu::Status::io -> ZU_IO",
            ],
        )


class TestAClassAndWhatItDerivesFrom(unittest.TestCase):
    def test_a_base_is_on_the_line(self):
        compound = ElementTree.fromstring("""
          <compounddef kind="class" prot="public">
            <compoundname>zu::SyntaxError</compoundname>
            <basecompoundref prot="public">zu::Exception</basecompoundref>
          </compounddef>""")
        self.assertEqual(
            surface.compound_entry(compound)[1],
            "class zu::SyntaxError : public zu::Exception",
        )

    def test_a_private_nested_type_publishes_nothing(self):
        compound = ElementTree.fromstring("""
          <compounddef kind="struct" prot="private">
            <compoundname>zu::Connection::Watch</compoundname>
          </compounddef>""")
        self.assertIsNone(surface.compound_entry(compound))


class TestWhatIdentifiesALine(unittest.TestCase):
    """The rule the three answers rest on.

    Everything that can change while the same call still picks the same
    function is shape. Everything that decides which function a call
    picks is identity.
    """

    def test_shape_is_not_identity(self):
        for one, other in [
            # A return type.
            (
                "func zu::Connection::query(std::string_view) -> Result",
                "func zu::Connection::query(std::string_view) -> Statement",
            ),
            # noexcept, which is part of the type and not of the overload.
            (
                "func zu::Connection::interrupt() -> void",
                "func zu::Connection::interrupt() noexcept -> void",
            ),
            # A deletion undone.
            (
                "func zu::Transaction::Transaction(const Transaction &) = delete",
                "func zu::Transaction::Transaction(const Transaction &)",
            ),
            # A default argument taken away.
            (
                "func zu::Connection::transaction(bool = false) -> Transaction",
                "func zu::Connection::transaction(bool) -> Transaction",
            ),
            # What a class derives from.
            (
                "class zu::SyntaxError : public zu::Exception",
                "class zu::SyntaxError : public zu::ProgrammingError",
            ),
        ]:
            self.assertEqual(surface.key(one), surface.key(other), one)

    def test_identity_is_identity(self):
        for one, other in [
            # A parameter type.
            (
                "func zu::Row::get(std::string_view) const -> T",
                "func zu::Row::get(std::uint32_t) const -> T",
            ),
            # const, which is what a const handle can call.
            (
                "func zu::Result::rows() const -> std::uint64_t",
                "func zu::Result::rows() -> std::uint64_t",
            ),
            # static, which decides whether there is an object at all.
            (
                "func static zu::Connection::open(std::string_view) -> Connection",
                "func zu::Connection::open(std::string_view) -> Connection",
            ),
            # A whole parameter, which is a different overload.
            (
                "func zu::Connection::appender(std::string_view) -> Appender",
                "func zu::Connection::appender(std::string_view, bool = false) -> Appender",
            ),
            # And the ten to_string overloads, which a key that could not
            # tell them apart would report nine of as gone.
            (
                "func zu::to_string(Status) noexcept -> std::string_view",
                "func zu::to_string(Severity) noexcept -> std::string_view",
            ),
        ]:
            self.assertNotEqual(surface.key(one), surface.key(other), one)


class TestWhatMovedIsToldApartFromWhatArrived(unittest.TestCase):
    """Every one of these would read as a name leaving and another
    arriving if the identity of a line were the whole line, which is the
    mistake that makes such a gate noise a reviewer learns to skip."""

    WAS = [
        "class zu::Result",
        "func zu::Connection::interrupt() -> void",
        "func zu::Result::rows() const -> std::uint64_t",
        "func zu::Row::get(std::string_view) const -> T",
    ]
    NOW = [
        "class zu::Result",
        # noexcept added, which is a change and not a replacement.
        "func zu::Connection::interrupt() noexcept -> void",
        # rows is gone.
        # columns has arrived.
        "func zu::Result::columns() const -> std::uint32_t",
        "func zu::Row::get(std::string_view) const -> T",
    ]

    def setUp(self):
        self.gone, self.arrived, self.changed = surface.compare(self.WAS, self.NOW)

    def test_what_went(self):
        self.assertEqual(len(self.gone), 1)
        self.assertIn("rows()", self.gone[0])

    def test_what_arrived(self):
        self.assertEqual(len(self.arrived), 1)
        self.assertIn("columns()", self.arrived[0])

    def test_what_changed_shape(self):
        self.assertEqual(len(self.changed), 1)
        self.assertIn("noexcept", self.changed[0][1])
        self.assertNotIn("noexcept", self.changed[0][0])

    def test_what_did_not_move_is_in_none_of_the_three(self):
        moved = self.gone + self.arrived + [old for old, _ in self.changed]
        for line in ("class zu::Result", "func zu::Row::get(std::string_view) const -> T"):
            self.assertNotIn(line, moved)

    def test_two_surfaces_that_agree_are_three_empty_answers(self):
        self.assertEqual(surface.compare(self.WAS, self.WAS), ([], [], []))


class TestTheHeaderIsReadForWhatDoxygenCannotSee(unittest.TestCase):
    """The `std::formatter` specializations and the macros taken back.

    The Doxyfile expands ZU_FORMATTER to nothing on purpose, so ten
    invocations do not arrive in the reference as ten classes nobody
    declared. But `std::format("{}", status)` is a call a user writes.
    """

    SOURCE = """#define ZU_FORMATTER(TYPE) \\
  template <> struct std::formatter<TYPE> {}

ZU_FORMATTER(zu::Status);
ZU_FORMATTER(zu::Value);

#undef ZU_FORMATTER
"""

    def setUp(self):
        import tempfile
        from pathlib import Path

        self.dir = tempfile.TemporaryDirectory()
        self.header = Path(self.dir.name) / "zu.hpp"
        self.header.write_text(self.SOURCE, encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def test_the_specializations_are_published(self):
        self.assertEqual(
            [written for _, written in surface.formatters(self.header)],
            [
                "formatter std::formatter< zu::Status >",
                "formatter std::formatter< zu::Value >",
            ],
        )

    def test_a_macro_the_header_takes_back_is_not(self):
        self.assertEqual(surface.undefined(self.header), {"ZU_FORMATTER"})


if __name__ == "__main__":
    unittest.main()
