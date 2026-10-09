"""Native battle UI treatment for the live recipient list."""
import xml.etree.ElementTree as ET
from itertools import combinations


def recipient_icon_expression(slot):
    """Visible-row ordinal, never an engine player ID; actions remain slot-bound."""
    previous = [f'BattleRoot.CanGiftToPlayer{i}' for i in range(slot)]
    path = lambda n: f'"ui/skins/coop/gift_recipient_{n}.png"'
    if not previous:
        return path(1)
    any_previous = ' || '.join(previous)
    result = f'GetIfElse({any_previous}, {path(2)}, {path(1)})'
    pairs = [f'({a} && {b})' for a, b in combinations(previous, 2)]
    if pairs:
        result = f'GetIfElse({" || ".join(pairs)}, {path(3)}, {result})'
    return result


def style_live_components(definitions, guid):
    result = []
    for definition in definitions:
        c = ET.fromstring(definition)
        cid = c.get('id')
        if cid == 'coop_gift_panel':
            c.find('LayoutEngine').set('spacing', '0.00,4.00')
            c.find('states/default').set('width', '240')
        elif cid.startswith('coop_gift_btn_'):
            c.set('soundcategory', 'UI_BAT_HUD_Generic_Click')
            images = c.find('componentimages')
            images.clear()
            assets = {
                'disc': ('ui/skins/default/button_round_off_backplate.png', 46, 46),
                'icon': ('ui/skins/coop/gift_recipient_1.png', 46, 46),
                'frame': ('ui/skins/default/button_round_frame_46.png', 46, 46),
                'hover': ('ui/skins/default/button_round_hover_highlight.png', 46, 46),
            }
            for key, (path, w, h) in assets.items():
                g = guid(f'{cid}/native/{key}')
                ET.SubElement(images, 'component_image', {
                    'this': g, 'uniqueguid': g,
                    'imagepath': path,
                    'width': str(w), 'height': str(h),
                })
            for state in c.findall('states/*'):
                state.set('width', '240')
                state.set('height', '46')
                state.set('text', '')
                state.set('font_m_font_name', 'Iskra-Bold')
                state.set('font_m_size', '18')
                state.set('font_m_colour', '#E8D49AFF')
                state.set('textvalign', 'Center')
                state.set('texthalign', 'Left')
                state.set('textxoffset', '17.00,0.00')
                metrics = state.find('imagemetrics')
                metrics.clear()
                keys = ['disc', 'icon', 'frame']
                if state.get('name') in ('hover', 'down'):
                    keys.append('hover')
                for key in keys:
                    _, w, h = assets[key]
                    g = guid(f'{cid}/{state.tag}/native/{key}')
                    ET.SubElement(metrics, 'image', {
                        'this': g, 'uniqueguid': g,
                        'componentimage': guid(f'{cid}/native/{key}'),
                        'width': str(w), 'height': str(h),
                    })
            callbacks = c.find('callbackwithcontextlist')
            cb = ET.SubElement(callbacks, 'callback_with_context', {
                'callback_id': 'ContextImageSetter',
                'context_function_id': recipient_icon_expression(int(cid[-1])),
            })
            props = ET.SubElement(cb, 'child_m_user_properties')
            ET.SubElement(props, 'property', {'name': 'image_index', 'value': '1'})
            ET.SubElement(props, 'property', {'name': 'update_constant', 'value': ''})
        else:
            c.set('docking', 'Top Left')
            c.set('component_anchor_point', '0.00,0.00')
            c.set('dock_offset', '54.00,0.00')
            c.set('offset', '54.00,0.00')
            c.set('isrelativeresize', 'false')
            for state in c.findall('states/*'):
                state.set('width', '178')
                state.set('height', '46')
                state.set('font_m_font_name', 'Iskra-Bold')
                state.set('font_m_size', '15')
                state.set('fontcat_name', 'item_header')
                state.set('font_m_colour', '#FFFFFFFF')
                state.set('texthalign', 'Left')
                # Native battle HUD shadow; render the name only once. Offset copies
                # picked up bright styling in-game and made the glyphs look bloated.
                state.set('text_shader_name', 'drop_shadow_t0')
                state.set('textshadervars', '-1.00,-1.00,1.00,0.00')
        ET.indent(c, space='\t', level=2)
        result.append('\t\t' + ET.tostring(c, encoding='unicode') + '\n')
    return result
