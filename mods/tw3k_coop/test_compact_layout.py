"""Offline regression for the dark artwork beside the narrowed right cards."""
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from compact_layout import ART_WIDTH


class LobbyOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run([sys.executable, str(HERE / 'dupe_panels.py'), '--write'],
                       check=True, capture_output=True)
        cls.root = ET.parse(HERE / 'build/ui/frontend ui/mp_grand_campaign.twui.xml').getroot()
        cls.components = {c.get('this'): c for c in cls.root.find('components')}
        vanilla = ET.parse(HERE / 'extracted/ui/frontend ui/mp_grand_campaign.twui.xml').getroot()
        cls.original = vanilla.find("components/character_overlay")

    def test_right_overlay_fits_holder_in_every_state_and_image(self):
        for slot in (2, 4):
            with self.subTest(slot=slot):
                panel = self.root.find(f'.//hierarchy//panel_player{slot}')
                holder = panel.find('character_holder_left')
                overlay = holder.find('character_overlay')
                component = self.components[overlay.get('this')]
                self.assertEqual(component.get('offset'), self.original.get('offset'))
                self.assertEqual(component.get('priority'), self.original.get('priority'))
                for state, original in zip(component.findall('states/*'),
                                           self.original.findall('states/*'), strict=True):
                    self.assertEqual(int(state.get('width')), ART_WIDTH)
                    self.assertEqual(state.get('height'), original.get('height'))
                    for image, old_image in zip(state.findall('imagemetrics/image'),
                                                original.findall('imagemetrics/image'), strict=True):
                        self.assertEqual(int(image.get('width')), ART_WIDTH)
                        for attr in ('height', 'colour', 'isrelativeresize'):
                            self.assertEqual(image.get(attr), old_image.get(attr))
                self.assertEqual(component.find('animations'), None)

    def test_overlay_ownership_and_occupancy_gate(self):
        owners = []
        for slot in range(1, 5):
            panel = self.root.find(f'.//hierarchy//panel_player{slot}')
            callbacks = self.components[panel.get('this')].find('callbackwithcontextlist')
            self.assertIsNotNone(callbacks.find("callback_with_context[@callback_id='MPCampaignPlayer']"))
            gate = callbacks.find("callback_with_context[@callback_id='ContextVisibilitySetter']")
            self.assertEqual(gate.get('context_object_id'), 'CcoFrontendFactionLeader')
            self.assertEqual(gate.get('context_function_id'), 'IsValidContext')
            overlays = panel.findall('.//character_overlay')
            self.assertEqual(len(overlays), int(slot in (2, 4)))
            owners.extend(n.get('this') for n in overlays)
        self.assertEqual(len(set(owners)), 2)


if __name__ == '__main__':
    unittest.main()
