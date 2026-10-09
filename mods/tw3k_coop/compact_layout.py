"""Keep the original four-player lobby; reduce only full-body artwork size."""
import re
import xml.etree.ElementTree as ET

CHARACTER_SCALE = 0.85
ART_WIDTH = 350
TRIM = 100


def compact_layout(source):
    # The native Character2DDisplayCreator uses a large_panel display canvas.
    # Retain its animated full-body composition, flags, contexts and all text.
    # Scale its existing 20x1750 canvas; keep its centre docking unchanged.
    pattern = re.compile(r'^\t\t<fullbody_character\n.*?^\t\t</fullbody_character>', re.M | re.S)

    def resize(match):
        block = match.group()
        width, height = round(20 * CHARACTER_SCALE), round(1750 * CHARACTER_SCALE)
        # Cached top-left agrees with the unchanged centre dock and -150 Y
        # displacement inside the vanilla 502x704 character holder.
        offset = f'{(ART_WIDTH - width) / 2:.2f},{(704 - height) / 2 - 150:.2f}'
        block = re.sub(r'offset="[^"]*"', f'offset="{offset}"', block, count=1)
        block = block.replace('width="20"', f'width="{width}"')
        block = block.replace('height="1750"', f'height="{height}"')
        return block

    result, count = pattern.subn(resize, source)
    if count != 4:
        raise ValueError(f'Expected four full-body displays, found {count}')
    root = ET.fromstring(result)
    components = {c.get('this'): c for c in root.find('components')}
    changed = {}

    def edit(node):
        c = components[node.get('this')]
        changed[c.get('this')] = c
        return c

    def width(c, value):
        for state in c.findall('states/*'):
            state.set('width', str(value))
            for image in state.findall('imagemetrics/image'):
                image.set('width', str(value))

    for slot in range(1, 5):
        panel = root.find(f'.//hierarchy//panel_player{slot}')
        right = slot in (2, 4)
        find = lambda tag: next(n for n in panel.iter() if n.tag == tag)
        # Vanilla's right slots offset these controls 138/139px into the
        # full-width panel. Align them above the avatar after narrowing.
        for tag, y in (('faction_dropdown', -45), ('faction_group_dropdown', -88)):
            edit(find(tag)).set('offset', f'1.00,{y:.2f}')
        if right:
            # Restore edge docking for the entire narrowed subtree, including
            # avatar, status and selectors, not just the backdrop.
            panel_def = edit(panel)
            for attr in ('offset', 'dock_offset'):
                x, y = map(float, panel_def.get(attr, '0.00,0.00').split(','))
                panel_def.set(attr, f'{x + TRIM:.2f},{y:.2f}')
        holder = edit(find('character_holder_left'))
        width(holder, ART_WIDTH)
        holder.set('offset', f'{256 - ART_WIDTH - TRIM if right else 0:.2f},-370.00')
        if right:
            holder.set('dock_offset', '-100.00,0.00')
        # Keep the previous left edge on BOTH sides. Narrowing then moves each
        # centred character 50px left, rather than moving right panels right.
        width(edit(find('gradient')), ART_WIDTH)
        if right:
            # The vanilla right card also has a dark character overlay. It is
            # a child of the holder, but has its own 514px state and 604px image:
            # narrowing the holder does not constrain this artwork. The inner
            # right card can therefore shade an adjacent seat after it empties.
            # Match both widths to the card; preserve its vertical fade/opacity.
            width(edit(find('character_overlay')), ART_WIDTH)
        ink = edit(find('ink_right' if right else 'ink_left'))
        width(ink, ART_WIDTH + 10)
        ink.set('offset', f'{256 - ART_WIDTH - 5 - TRIM if right else -5:.2f},-516.00')
        if right:
            ink.set('dock_offset', '-95.00,6.00')
        detail = edit(find('specialization_list_parent'))
        if not right:
            detail.set('offset', '32.00,10.00')
            detail.set('dock_offset', '32.00,10.00')
        else:
            detail.set('dock_offset', '-128.00,10.00')
        # Narrow the actual list, not just its artwork. Keep icon art sizes;
        # wrap feature cards into two columns and retain the vertical scroll.
        detail_tree = find('specialization_list_parent')
        for node in detail_tree.iter():
            c = components[node.get('this')]
            for state in c.findall('states/*'):
                old_width = int(state.get('width', '0'))
                if old_width >= 300:
                    edit(node)
                    state.set('width', str(old_width - TRIM))
                elif old_width > 255:
                    edit(node)
                    state.set('width', '255')
                if node.tag == 'label_description' and old_width == 203:
                    edit(node)
                    state.set('width', '177')
                if node.tag in ('label_description', 'label_name', 'title'):
                    edit(node)
                    state.set('texthbehaviour', 'Split by word')
            if node.tag == 'features_list':
                layout = edit(node).find('LayoutEngine')
                layout.set('itemsperrow', '2')
                layout.set('spacing', '12.00,10.00')
            layout = c.find('LayoutEngine')
            if layout is not None:
                # Leave feature-card sizing and wrapped description heights
                # intact; remove padding from repeated rows and sections.
                spacing = layout.get('spacing')
                if spacing in ('0.00,20.00', '0.00,10.00', '0.00,7.00'):
                    edit(node)
                    layout.set('spacing', '0.00,8.00' if spacing == '0.00,20.00' else '0.00,3.00')
                cb = c.find("callbackwithcontextlist/callback_with_context[@context_function_id='UniqueCharacterList']")
                if cb is not None:
                    edit(node)
                    layout.set('type', 'List')
                    layout.set('itemsperrow', '2')
                    layout.set('spacing', '8.00,6.00')
                    layout.set('margins', '0.00,0.00')
                    width(c, 240)
            if node.tag == 'template_effect':
                for state in edit(node).findall('states/*'):
                    state.set('height', '44')
        # Bring the right player's name block into the narrowed art bounds.
        if right:
            name = edit(find('top_parent'))
            name.set('offset', '3.00,270.00')
            name.set('dock_offset', '3.00,270.00')
        # Scrollbars stay right-docked for every slot, 100px left of before.
        edit(find('vslider')).set('offset', '5.00,0.00')

    def splice(match):
        g = re.search(r'this="([^"]+)"', match.group()).group(1)
        if g not in changed:
            return match.group()
        c = changed[g]
        ET.indent(c, space='\t', level=2)
        return '\t\t' + ET.tostring(c, encoding='unicode').rstrip()
    result = re.sub(r'^\t\t<(\w+)\n.*?^\t\t</\1>', splice, result, flags=re.M | re.S)
    ET.fromstring(result)
    return result
