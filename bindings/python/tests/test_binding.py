from unittest import TestCase

import tree_sitter, tree_sitter_cpp_ai


class TestLanguage(TestCase):
    def test_can_load_grammar(self):
        try:
            tree_sitter.Language(tree_sitter_cpp_ai.language())
        except Exception:
            self.fail("Error loading C++ grammar")

    def test_recovers_unexpanded_macro_type(self):
        parser = tree_sitter.Parser(tree_sitter.Language(tree_sitter_cpp_ai.language()))
        root = parser.parse(b"void f() { MACRO(a) g(1); }").root_node
        self.assertFalse(root.has_error, str(root))
        self.assertIn("uncertain_macro_type", str(root))
