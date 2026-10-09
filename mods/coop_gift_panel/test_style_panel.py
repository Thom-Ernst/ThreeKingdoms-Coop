"""Structural regression checks; these do not replace in-game rendering checks."""
import itertools
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from build_panel import build
from style_panel import recipient_icon_expression


class GiftStyleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(__file__).parent / 'extracted/ui/battle ui/hud_battle.twui.xml'
        cls.root = ET.fromstring(build(source.read_text(encoding='utf-8'), live=True)[0])
        cls.components = {c.get('id'): c for c in cls.root.find('components')}

    def test_actions_and_visibility(self):
        for slot in range(4):
            button = self.components[f'coop_gift_btn_p{slot}']
            callbacks = button.findall('callbackwithcontextlist/callback_with_context')
            expressions = [c.get('context_function_id', '') for c in callbacks]
            self.assertIn(f'BattleRoot.GiftUnits({slot})', expressions)
            self.assertIn(f'BattleRoot.CanGiftToPlayer{slot}', expressions)
            for state in button.findall('states/*'):
                self.assertEqual(state.get('text'), '')
            paths = [i.get('imagepath') for i in button.findall('componentimages/*')]
            self.assertFalse(any('rectangular' in p for p in paths))
            self.assertEqual(paths[:3], [
                'ui/skins/default/button_round_off_backplate.png',
                'ui/skins/coop/gift_recipient_1.png',
                'ui/skins/default/button_round_frame_46.png'])
            image_cb = next(c for c in callbacks if c.get('callback_id') == 'ContextImageSetter')
            self.assertEqual(image_cb.find("child_m_user_properties/property[@name='image_index']").get('value'), '1')

    def test_numbering_all_recipient_subsets(self):
        for flags in itertools.product((False, True), repeat=4):
            if sum(flags) > 3:
                continue  # The local player can never receive their own units.
            for slot, visible in enumerate(flags):
                if not visible:
                    continue
                expr = recipient_icon_expression(slot)
                for i, value in enumerate(flags):
                    expr = expr.replace(f'BattleRoot.CanGiftToPlayer{i}', str(value))
                expr = expr.replace('&&', ' and ').replace('||', ' or ')
                actual = eval(expr, {'__builtins__': {}, 'GetIfElse': lambda c, a, b: a if c else b})
                self.assertEqual(actual, f'ui/skins/coop/gift_recipient_{1 + sum(flags[:slot])}.png')

    def test_single_shadowed_labels_and_hierarchy(self):
        definitions = {c.get('this') for c in self.root.find('components')}
        self.assertFalse(any('_outline' in cid for cid in self.components if cid))
        for slot in range(4):
            for suffix in ('',):
                cid = f'coop_gift_txt_p{slot}{suffix}'
                label = self.components[cid]
                nodes = [n for n in self.root.iter(cid) if n.get('id') is None]
                self.assertEqual(len(nodes), 1)
                self.assertIn(nodes[0].get('this'), definitions)
                self.assertEqual(nodes[0].get('this'), label.get('this'))
                for state in label.findall('states/*'):
                    self.assertEqual(state.get('font_m_colour'), '#FFFFFFFF')
                    self.assertEqual(state.get('text_shader_name'), 'drop_shadow_t0')
                    self.assertEqual(state.get('textshadervars'), '-1.00,-1.00,1.00,0.00')
                cb = label.find('callbackwithcontextlist/callback_with_context')
                self.assertEqual(cb.get('context_function_id'), f'BattleRoot.PlayerName{slot}')
                self.assertIsNotNone(cb.find("child_m_user_properties/property[@name='update_constant']"))


if __name__ == '__main__':
    unittest.main()
