"""Mutation checks for the narrowly scoped actual-screen glyph comparison."""
import unittest
from ui_freeze_compare import glyph_rounding_equivalent, combo_glyph_regions, approved_v219_ssh_layout


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

    def test_approved_ssh_layout_still_rejects_lost_fields_and_overlap(self):
        rows=[[i,"Edit",10+(n%2)*120,70+(n//2)*50,110+(n%2)*120,98+(n//2)*50,True]
              for n,i in enumerate((1014,1016,1018,1020))]
        rows += [[i,"Button",10,200+(n*30),100,228+(n*30),True]
                 for n,i in enumerate((1028,1029,1031,1,2))]
        before={"size":[300,400],"geometry":rows}
        after={"size":[300,400],"geometry":[list(r) for r in rows]}
        self.assertEqual(approved_v219_ssh_layout(before,after)["status"],"PASS")
        after["geometry"][1][2:6]=after["geometry"][0][2:6]
        with self.assertRaises(AssertionError):
            approved_v219_ssh_layout(before,after)
        after["geometry"]=after["geometry"][1:]
        with self.assertRaises(AssertionError):
            approved_v219_ssh_layout(before,after)


if __name__=='__main__':unittest.main()
