"""Mutation checks for the narrowly scoped actual-screen glyph comparison."""
import unittest
from ui_freeze_compare import glyph_rounding_equivalent, combo_glyph_regions


class GlyphComparisonTest(unittest.TestCase):
    def setUp(self):
        self.before=bytes([100,120,140,0]*100)
        self.regions=[[3,3,7,7]]

    def changed(self,pixel,delta):
        after=bytearray(self.before);after[pixel*4]+=delta
        return glyph_rounding_equivalent(self.before,after,pixel,10,self.regions)

    def test_two_levels_inside_original_glyph_are_bounded(self):
        self.assertTrue(self.changed(44,2))

    def test_three_levels_inside_original_glyph_are_rejected(self):
        self.assertFalse(self.changed(44,3))

    def test_one_level_in_field_background_is_rejected(self):
        self.assertFalse(self.changed(42,1))

    def test_only_combo_arrow_geometry_gets_a_region(self):
        rows=[[1,'ComboBox',10,20,210,48,True],[2,'Edit',10,60,210,88,True]]
        self.assertEqual(combo_glyph_regions(rows,96),[[185,25,203,43]])


if __name__=='__main__':unittest.main()
