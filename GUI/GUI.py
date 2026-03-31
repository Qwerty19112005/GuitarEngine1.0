import dearpygui.dearpygui as dpg
from pythonosc import udp_client
import xml.etree.ElementTree as ET
import os
import json
import glob
import copy 

# Hardcoded directory (Change for custom directory. Make sure to change in backend code as well)
JSON_DIR = r"D:\Guitar Processor data\JSON"
JUCE_DIR = r"D:\Guitar Processor data\JUCE XML"

os.makedirs(JSON_DIR, exist_ok=True)
os.makedirs(JUCE_DIR, exist_ok=True)
os.chdir(JSON_DIR)

# Starting setup
osc = udp_client.SimpleUDPClient("127.0.0.1", 7001)

next_node_id = 3
active_links = {}         
preset_nodes = {}         
preset_links = {}         
node_positions = {}       
preset_dirty = {}         

juce_to_dpg_node = {}     
juce_to_dpg_pin = {}      

current_setlist_name = "Default"
setlist_data = {}
bank_tree_nodes = []

active_bank_val = 0
active_preset_val = 0
currently_selected_preset_str = None


copy_mode_active = False
override_initial_preset = None


plugin_pin_info = {}
def init_plugin_pin_info():
    plugin_pin_info.clear()
    cache_path = os.path.join(JUCE_DIR, "PluginCache.xml")
    if os.path.exists(cache_path):
        try:
            tree = ET.parse(cache_path)
            for p in tree.getroot().findall('PLUGIN'):
                name = p.get('name')
                if name:
                    plugin_pin_info[name] = {
                        'in': int(p.get('numInputs', '2')),
                        'out': int(p.get('numOutputs', '2'))
                    }
        except: pass
init_plugin_pin_info()

dpg.create_context()

with dpg.theme() as global_theme:
    with dpg.theme_component(dpg.mvAll):
        dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 8, 8, category=dpg.mvThemeCat_Core)
        dpg.add_theme_style(dpg.mvStyleVar_ItemSpacing, 10, 8, category=dpg.mvThemeCat_Core)
        dpg.add_theme_color(dpg.mvThemeCol_Button, [60, 60, 60])
        dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, [90, 90, 90])
        dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, [120, 120, 120])
        dpg.add_theme_color(dpg.mvThemeCol_Header, [70, 70, 70])
        dpg.add_theme_color(dpg.mvThemeCol_HeaderHovered, [100, 100, 100])
        dpg.add_theme_style(dpg.mvStyleVar_FrameBorderSize, 1, category=dpg.mvThemeCat_Core)
dpg.bind_theme(global_theme)

with dpg.theme(tag="highlight_text_theme"):
    with dpg.theme_component(dpg.mvMenuItem):
        dpg.add_theme_color(dpg.mvThemeCol_Text, [255, 200, 50])



def create_empty_setlist(name):
    return {
        "setlist_name": name,
        "num_banks": 0,
        "banks": {}
    }

def save_unified_setlist():
    with open(os.path.join(JSON_DIR, f"Setlist_{current_setlist_name}.json"), 'w') as f:
        json.dump(setlist_data, f, indent=4)

def refresh_setlist_dropdown():
    files = glob.glob(os.path.join(JSON_DIR, "Setlist_*.json"))
    list_names = []
    
    if not files:
        data = create_empty_setlist("Default")
        with open(os.path.join(JSON_DIR, "Setlist_Default.json"), 'w') as f: json.dump(data, f, indent=4)
        files = [os.path.join(JSON_DIR, "Setlist_Default.json")]
        
    for f in files:
        name = os.path.basename(f).replace("Setlist_", "").replace(".json", "")
        list_names.append(name)
        
    if dpg.does_item_exist("bank_list_combo"):
        dpg.configure_item("bank_list_combo", items=list_names)
    return list_names

def load_unified_setlist(sender, app_data):
    global current_setlist_name, setlist_data, bank_tree_nodes
    global currently_selected_preset_str, active_bank_val, active_preset_val
    global override_initial_preset
    
    target_name = app_data
    if target_name == current_setlist_name and sender is not None: return
    
    current_setlist_name = target_name
    hard_clear_python_ui()
    bank_tree_nodes.clear()

    currently_selected_preset_str = None
    active_bank_val = 0
    active_preset_val = 0
    dpg.hide_item("NodeEditor")
    dpg.set_value("active_preset_label", "No Preset Selected")

    file_path = os.path.join(JSON_DIR, f"Setlist_{current_setlist_name}.json")
    if os.path.exists(file_path):
        with open(file_path, 'r') as f:
            setlist_data = json.load(f)
    else:
        setlist_data = create_empty_setlist(current_setlist_name)
        save_unified_setlist()

    osc.send_message("/setlist/load", [current_setlist_name])
    
    global next_node_id
    max_id = 2
    for b_key, b_val in setlist_data.get("banks", {}).items():
        for p_key, p_val in b_val.get("presets", {}).items():
            preset_str = f"Bank {b_key} - Preset {p_key}"
            for n in p_val.get("nodes", []):
                nid, name = n["id"], n["name"]
                pos = n.get("pos", [450, 300])
                max_id = max(max_id, nid)
                create_plugin_node_ui(nid, name, preset_str, pos)
            
            if preset_str not in preset_links: preset_links[preset_str] = []
            for l in p_val.get("links", []):
                s_n, s_c, d_n, d_c = l["src_node"], l["src_chan"], l["dst_node"], l["dst_chan"]
                s_pin = juce_to_dpg_pin.get((s_n, 'out', s_c))
                d_pin = juce_to_dpg_pin.get((d_n, 'in', d_c))
                if s_pin and d_pin and dpg.does_item_exist(s_pin) and dpg.does_item_exist(d_pin):
                    link_id = dpg.add_node_link(s_pin, d_pin, parent="NodeEditor")
                    active_links[link_id] = {'src_node': s_n, 'dst_node': d_n, 'src_chan': s_c, 'dst_chan': d_c, 'preset': int(p_key)}
                    preset_links[preset_str].append(link_id)

    next_node_id = max_id + 1
    refresh_entire_bank_ui()
    
    if setlist_data["num_banks"] > 0:
        if override_initial_preset:
            select_preset_callback(None, None, override_initial_preset)

def execute_create_bank():
    new_name = dpg.get_value("create_bank_input").strip()
    if not new_name: return
    
    b_idx = str(setlist_data["num_banks"] + 1)
    setlist_data["num_banks"] += 1
    setlist_data["banks"][b_idx] = {
        "name": new_name,
        "presets": {
            "1": {"name": "PRESET 1", "nodes": [], "links": []},
            "2": {"name": "PRESET 2", "nodes": [], "links": []},
            "3": {"name": "PRESET 3", "nodes": [], "links": []},
            "4": {"name": "PRESET 4", "nodes": [], "links": []}
        }
    }
    save_unified_setlist()
    dpg.configure_item("create_bank_modal", show=False)
    refresh_entire_bank_ui()

def create_new_bank_list(sender, app_data):
    new_name = dpg.get_value("new_list_input").strip()
    if new_name == "": return
    
    data = create_empty_setlist(new_name)
    with open(os.path.join(JSON_DIR, f"Setlist_{new_name}.json"), 'w') as f: json.dump(data, f, indent=4)
        
    refresh_setlist_dropdown()
    dpg.set_value("bank_list_combo", new_name)
    load_unified_setlist(None, new_name)

def delete_list_callback():
    global current_setlist_name
    meta_file = os.path.join(JSON_DIR, f"Setlist_{current_setlist_name}.json")
    if os.path.exists(meta_file): os.remove(meta_file)

    existing = refresh_setlist_dropdown()
    if existing:
        dpg.set_value("bank_list_combo", existing[0])
        load_unified_setlist(None, existing[0])
    else:
        dpg.set_value("new_list_input", "Default")
        create_new_bank_list(None, None)

# scanner helper fncts

def scan_plugins_callback():
    osc.send_message("/system/scan", [])
    dpg.configure_item("scan_modal", show=True)

def populate_plugin_menu():
    dpg.delete_item("add_plugin_menu", children_only=True)
    
    cache_path = os.path.join(JUCE_DIR, "PluginCache.xml")
    if os.path.exists(cache_path):
        try:
            tree = ET.parse(cache_path)
            plugins = sorted([p.get('name') for p in tree.getroot().findall('PLUGIN')])
            dpg.add_menu_item(label="-- Select Plugin --", enabled=False, parent="add_plugin_menu")
            dpg.add_separator(parent="add_plugin_menu")
            for p_name in plugins: dpg.add_menu_item(label=p_name, callback=add_plugin_callback, user_data=p_name, parent="add_plugin_menu")
        except: pass
    init_plugin_pin_info()


# helper fncs

def hard_clear_python_ui():
    global next_node_id
    for links in preset_links.values():
        for link in links:
            if dpg.does_item_exist(link): dpg.delete_item(link)
    for nodes in preset_nodes.values():
        for node in nodes:
            if dpg.does_item_exist(node): dpg.delete_item(node)
            
    preset_nodes.clear()
    preset_links.clear()
    active_links.clear()
    node_positions.clear()
    preset_dirty.clear()
    next_node_id = 3
    
    keys_to_delete = list(juce_to_dpg_node.keys())
    for k in keys_to_delete: del juce_to_dpg_node[k]
        
    keys_to_delete = [k for k in juce_to_dpg_pin.keys() if k[0] != 1 and k[0] != 2]
    for k in keys_to_delete: del juce_to_dpg_pin[k]
    update_unsaved_indicator()

def check_node_movement():
    if not currently_selected_preset_str: return
    moved = False
    if currently_selected_preset_str in preset_nodes:
        for dpg_node_id in preset_nodes[currently_selected_preset_str]:
            if dpg.does_item_exist(dpg_node_id):
                current_pos = dpg.get_item_pos(dpg_node_id)
                stored_pos = node_positions.get(dpg_node_id)
                if stored_pos and (int(current_pos[0]) != int(stored_pos[0]) or int(current_pos[1]) != int(stored_pos[1])):
                    node_positions[dpg_node_id] = current_pos
                    moved = True
    if moved:
        preset_dirty[currently_selected_preset_str] = True
        update_unsaved_indicator()

def update_unsaved_indicator():
    if currently_selected_preset_str and preset_dirty.get(currently_selected_preset_str, False):
        dpg.show_item("unsaved_indicator")
    else: dpg.hide_item("unsaved_indicator")

def create_plugin_node_ui(juce_id, plugin_name, preset_str, pos):
    with dpg.node(parent="NodeEditor", label=plugin_name) as dpg_node_id:
        juce_to_dpg_node[juce_id] = dpg_node_id
        with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Static):
            with dpg.group(horizontal=True):
                dpg.add_button(label="Open UI", callback=show_ui_callback, user_data=juce_id)
                dpg.add_button(label="Remove", callback=delete_plugin_callback, user_data=juce_id)
        
        num_ins = plugin_pin_info.get(plugin_name, {}).get('in', 2)
        num_outs = plugin_pin_info.get(plugin_name, {}).get('out', 2)
        
        for i in range(num_ins):
            with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Input, user_data=(juce_id, i)) as pin_in:
                if i == 0: lbl = "In L"
                elif i == 1: lbl = "In R"
                else: lbl = f"Aux In {i+1}"
                dpg.add_text(lbl)
                juce_to_dpg_pin[(juce_id, 'in', i)] = pin_in
                
        for i in range(num_outs):
            with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Output, user_data=(juce_id, i)) as pin_out:
                if i == 0: lbl = "Out L"
                elif i == 1: lbl = "Out R"
                else: lbl = f"Aux Out {i+1}"
                dpg.add_text(lbl)
                juce_to_dpg_pin[(juce_id, 'out', i)] = pin_out

    dpg.set_item_pos(dpg_node_id, pos)
    if preset_str not in preset_nodes: preset_nodes[preset_str] = []
    preset_nodes[preset_str].append(dpg_node_id)
    node_positions[dpg_node_id] = pos


# core callbacks

def add_plugin_callback(sender, app_data, user_data):
    global next_node_id
    if not currently_selected_preset_str: return
        
    plugin_name = user_data
    current_id = next_node_id
    next_node_id += 1
    
    osc.send_message("/node/add", [current_id, plugin_name, active_preset_val, active_bank_val])
    create_plugin_node_ui(current_id, plugin_name, currently_selected_preset_str, [450, 300])
    preset_dirty[currently_selected_preset_str] = True
    update_unsaved_indicator()

def delete_plugin_callback(sender, app_data, user_data):
    juce_id = user_data 
    if juce_id not in juce_to_dpg_node: return
    dpg_node_id = juce_to_dpg_node[juce_id]
    
    links_to_sever = [l for l, d in active_links.items() if d['src_node'] == juce_id or d['dst_node'] == juce_id]
    for link_id in links_to_sever: delink_callback("NodeEditor", link_id)
        
    for preset, nodes in preset_nodes.items():
        if dpg_node_id in nodes:
            nodes.remove(dpg_node_id)
            preset_dirty[preset] = True
            update_unsaved_indicator()
            break
            
    osc.send_message("/node/remove", [juce_id])
    dpg.delete_item(dpg_node_id)
    del juce_to_dpg_node[juce_id]
    
    for pin in list(juce_to_dpg_pin.keys()):
        if pin[0] == juce_id: juce_to_dpg_pin.pop(pin, None)

def link_callback(sender, app_data):
    if not currently_selected_preset_str: return
    src_pin_ui, dst_pin_ui = app_data[0], app_data[1]
    link_id = dpg.add_node_link(src_pin_ui, dst_pin_ui, parent=sender)
    
    src_juce_node, src_channel = dpg.get_item_user_data(src_pin_ui)
    dst_juce_node, dst_channel = dpg.get_item_user_data(dst_pin_ui)
    
    active_links[link_id] = {'src_node': src_juce_node, 'dst_node': dst_juce_node, 'src_chan': src_channel, 'dst_chan': dst_channel, 'preset': active_preset_val}
    if currently_selected_preset_str not in preset_links: preset_links[currently_selected_preset_str] = []
    preset_links[currently_selected_preset_str].append(link_id)
    
    osc.send_message("/node/connect", [src_juce_node, src_channel, dst_juce_node, dst_channel])
    preset_dirty[currently_selected_preset_str] = True
    update_unsaved_indicator()

def delink_callback(sender, app_data):
    link_id = app_data
    if link_id in active_links:
        data = active_links[link_id]
        osc.send_message("/node/disconnect", [data['src_node'], data['src_chan'], data['dst_node'], data['dst_chan']])
        del active_links[link_id]
        
    for preset, links in preset_links.items():
        if link_id in links:
            links.remove(link_id)
            preset_dirty[preset] = True
            update_unsaved_indicator()
            break
    if dpg.does_item_exist(link_id): dpg.delete_item(link_id)

def show_ui_callback(sender, app_data, user_data): osc.send_message("/node/show_ui", [user_data])

def global_remove_callback():
    selected_links = dpg.get_selected_links("NodeEditor")
    if selected_links:
        for link in selected_links: delink_callback("NodeEditor", link)

def save_preset_callback():
    if not currently_selected_preset_str: return
    

    global setlist_data
    file_path = os.path.join(JSON_DIR, f"Setlist_{current_setlist_name}.json")
    if os.path.exists(file_path):
        with open(file_path, 'r') as f:
            try:
                disk_data = json.load(f)
                if "banks" in disk_data:
                    setlist_data["banks"] = disk_data["banks"]
            except:
                pass
    
    nodes_to_save = []
    
    if currently_selected_preset_str in preset_nodes:
        for dpg_node_id in preset_nodes[currently_selected_preset_str]:
            if dpg.does_item_exist(dpg_node_id):
                juce_id = next((j for j, d in juce_to_dpg_node.items() if d == dpg_node_id), None)
                if juce_id is not None:
                    existing_state = ""
                    bKey, pKey = str(active_bank_val), str(active_preset_val)
                    if setlist_data["banks"].get(bKey, {}).get("presets", {}).get(pKey, {}).get("nodes"):
                        for old_n in setlist_data["banks"][bKey]["presets"][pKey]["nodes"]:
                            if old_n["id"] == juce_id: existing_state = old_n.get("state", "")
                                
                    nodes_to_save.append({
                        "id": juce_id, 
                        "name": dpg.get_item_label(dpg_node_id), 
                        "pos": dpg.get_item_pos(dpg_node_id),
                        "state": existing_state
                    })
                    
    links_to_save = [active_links[l] for l in preset_links.get(currently_selected_preset_str, []) if l in active_links]
    
    bKey, pKey = str(active_bank_val), str(active_preset_val)
    setlist_data["banks"][bKey]["presets"][pKey]["nodes"] = nodes_to_save
    setlist_data["banks"][bKey]["presets"][pKey]["links"] = links_to_save
    save_unified_setlist()
        
    preset_dirty[currently_selected_preset_str] = False
    update_unsaved_indicator() 
    
    osc.send_message("/preset/save", [active_bank_val, active_preset_val])

def toggle_copy_mode():
    global copy_mode_active
    if not currently_selected_preset_str: return
    copy_mode_active = True
    dpg.configure_item("copy_preset_btn", label="[ SELECT DESTINATION PRESET ]")
    dpg.bind_item_theme("copy_preset_btn", "highlight_text_theme")
    dpg.show_item("cancel_copy_btn")

def cancel_copy_mode():
    global copy_mode_active
    copy_mode_active = False
    dpg.configure_item("copy_preset_btn", label="Save to another preset")
    dpg.bind_item_theme("copy_preset_btn", 0)
    dpg.hide_item("cancel_copy_btn")


def select_preset_callback(sender, app_data, user_data):
    global currently_selected_preset_str, active_bank_val, active_preset_val
    global copy_mode_active, next_node_id, override_initial_preset
    
    bank_num, preset_num = int(user_data[0]), int(user_data[1])
    internal_preset_str = f"Bank {bank_num} - Preset {preset_num}"
    
    if copy_mode_active:
        if currently_selected_preset_str:
            save_preset_callback() 
            
            src_b = str(active_bank_val)
            src_p = str(active_preset_val)
            dst_b = str(bank_num)
            dst_p = str(preset_num)
            
            src_nodes = copy.deepcopy(setlist_data["banks"][src_b]["presets"][src_p].get("nodes", []))
            src_links = copy.deepcopy(setlist_data["banks"][src_b]["presets"][src_p].get("links", []))
            
            id_map = {}
            for n in src_nodes:
                old_id = n["id"]
                new_id = next_node_id
                next_node_id += 1
                id_map[old_id] = new_id
                n["id"] = new_id
                
            for l in src_links:
                if l["src_node"] in id_map: l["src_node"] = id_map[l["src_node"]]
                if l["dst_node"] in id_map: l["dst_node"] = id_map[l["dst_node"]]
                l["preset"] = int(dst_p)
                
            setlist_data["banks"][dst_b]["presets"][dst_p]["nodes"] = src_nodes
            setlist_data["banks"][dst_b]["presets"][dst_p]["links"] = src_links
            save_unified_setlist()
            
            cancel_copy_mode()
            
            override_initial_preset = (bank_num, preset_num)
            load_unified_setlist(None, current_setlist_name)
            override_initial_preset = None
        return
    

    preset_name = setlist_data["banks"][str(bank_num)]["presets"][str(preset_num)].get("name", f"PRESET {preset_num}")
    bank_name = setlist_data["banks"][str(bank_num)].get("name", f"BANK {bank_num}")
    list_name = current_setlist_name
    
    if currently_selected_preset_str == internal_preset_str: return

    active_bank_val = bank_num
    active_preset_val = preset_num
    
    if currently_selected_preset_str and currently_selected_preset_str in preset_nodes:
        for dpg_node_id in preset_nodes[currently_selected_preset_str]:
            if dpg.does_item_exist(dpg_node_id): node_positions[dpg_node_id] = dpg.get_item_pos(dpg_node_id)
                
    currently_selected_preset_str = internal_preset_str
    
    dpg.show_item("NodeEditor")
    
    full_display_str = f"[{list_name}]  >  {bank_name}  >  {preset_name}"
    dpg.set_value("active_preset_label", full_display_str)
    
    for preset_str, node_list in preset_nodes.items():
        for dpg_node_id in node_list:
            if dpg.does_item_exist(dpg_node_id):
                if preset_str == currently_selected_preset_str:
                    dpg.show_item(dpg_node_id)
                    if dpg_node_id in node_positions: dpg.set_item_pos(dpg_node_id, node_positions[dpg_node_id])
                else: dpg.hide_item(dpg_node_id)
                    
    for preset_str, link_list in preset_links.items():
        for link_id in link_list:
            if dpg.does_item_exist(link_id):
                if preset_str == currently_selected_preset_str: dpg.show_item(link_id)
                else: dpg.hide_item(link_id)
                    
    osc.send_message("/preset/switch", [active_bank_val, active_preset_val])
    update_unsaved_indicator()

def bank_clicked_callback(sender, app_data, user_data):
    clicked_bank_id = user_data 
    for bank_id in bank_tree_nodes:
        if bank_id != clicked_bank_id and dpg.does_item_exist(bank_id): 
            dpg.set_value(bank_id, False)

def refresh_entire_bank_ui():
    for item in dpg.get_item_children("bank_list_group", 1): dpg.delete_item(item)
    bank_tree_nodes.clear()

    for b_key, b_val in setlist_data.get("banks", {}).items():
        b_idx = int(b_key)
        bank_display_name = b_val.get("name", f"BANK {b_idx}")
        
        with dpg.group(parent="bank_list_group"):
            tree_tag = dpg.generate_uuid()
            with dpg.tree_node(label=bank_display_name, selectable=False, tag=tree_tag) as new_bank:
                bank_tree_nodes.append(tree_tag)
                with dpg.item_handler_registry() as bank_handler:
                    dpg.add_item_toggled_open_handler(callback=bank_clicked_callback, user_data=tree_tag)
                dpg.bind_item_handler_registry(new_bank, bank_handler)
                
                with dpg.group(horizontal=True):
                    dpg.add_text("  Options: ")
                    b_opts = dpg.add_button(label="...")
                dpg.add_separator()

                for p_key, p_val in b_val.get("presets", {}).items():
                    p_idx = int(p_key)
                    p_name = p_val.get("name", f"PRESET {p_idx}")
                    with dpg.group(horizontal=True):
                        dpg.add_button(label=p_name, width=125, callback=select_preset_callback, user_data=(b_idx, p_idx))
                        p_opts = dpg.add_button(label="...")
            dpg.add_separator()

def add_bank_callback():
    dpg.set_value("create_bank_input", "")
    dpg.configure_item("create_bank_modal", show=True)
    dpg.focus_item("create_bank_input")


# UI

with dpg.window(label="Pedalboard UI", width=1200, height=800, tag="MainWindow"):
    with dpg.menu_bar():
        with dpg.menu(label="Add Plugin", tag="add_plugin_menu"):
            pass 
        
        dpg.add_menu_item(label="Scan for new VSTs", callback=scan_plugins_callback)
                    
        with dpg.menu(label="Setlists"):
            dpg.add_text("Create New:")
            dpg.add_input_text(tag="new_list_input", width=150)
            dpg.add_button(label="Create List", callback=create_new_bank_list)
            dpg.add_separator()
            dpg.add_text("Load Existing:")
            with dpg.group(horizontal=True):
                dpg.add_listbox(items=[], tag="bank_list_combo", width=150, callback=load_unified_setlist)
                list_opts = dpg.add_button(label="...")
                with dpg.popup(list_opts, mousebutton=dpg.mvMouseButton_Left):
                    dpg.add_menu_item(label="Delete Current List", callback=delete_list_callback)

        dpg.add_menu_item(label="Remove Selected Wire", callback=global_remove_callback)
        dpg.add_menu_item(label="Save Preset", callback=save_preset_callback)
        
        # Copying buttons
        dpg.add_menu_item(label="Save to another preset", tag="copy_preset_btn", callback=toggle_copy_mode)
        dpg.add_menu_item(label="Cancel", tag="cancel_copy_btn", show=False, callback=cancel_copy_mode)

        dpg.add_text("No Preset Selected", tag="active_preset_label", color=[150, 255, 150])
        dpg.add_text(" [ UNSAVED ] ", tag="unsaved_indicator", color=[255, 100, 100], show=False)

    with dpg.group(horizontal=True):
        with dpg.child_window(width=220, height=-1):
            with dpg.group(tag="bank_list_group"): pass 
            dpg.add_button(label="ADD BANK", width=-1, height=40, callback=add_bank_callback)

        with dpg.child_window(width=-1, height=-1):
            with dpg.node_editor(callback=link_callback, delink_callback=delink_callback, tag="NodeEditor", show=False):
                with dpg.node(label="AudioBox INPUT", pos=[50, 50]) as hw_in:
                    with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Output, user_data=(1, 0)) as pin_in_L:
                        dpg.add_text("Input 1")
                        juce_to_dpg_pin[(1, 'out', 0)] = pin_in_L
                    with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Output, user_data=(1, 1)) as pin_in_R:
                        dpg.add_text("Input 2")
                        juce_to_dpg_pin[(1, 'out', 1)] = pin_in_R
                        
                with dpg.node(label="AudioBox OUTPUT", pos=[900, 600]) as hw_out:
                    with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Input, user_data=(2, 0)) as pin_out_L:
                        dpg.add_text("Speaker Out L")
                        juce_to_dpg_pin[(2, 'in', 0)] = pin_out_L
                    with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Input, user_data=(2, 1)) as pin_out_R:
                        dpg.add_text("Speaker Out R")
                        juce_to_dpg_pin[(2, 'in', 1)] = pin_out_R

# MODALS
with dpg.window(label="Create New Bank", modal=True, show=False, tag="create_bank_modal", width=300, no_resize=True):
    dpg.add_text("Enter Bank Name:")
    dpg.add_input_text(tag="create_bank_input", width=-1, on_enter=True, callback=execute_create_bank)
    dpg.add_spacer(height=5)
    with dpg.group(horizontal=True):
        dpg.add_button(label="Create", width=135, callback=execute_create_bank)
        dpg.add_button(label="Cancel", width=135, callback=lambda: dpg.configure_item("create_bank_modal", show=False))

with dpg.window(label="Scanning VSTs...", modal=True, show=False, tag="scan_modal", width=300, no_resize=True):
    dpg.add_text("C++ Engine is scanning your folders.\nThis may freeze the C++ console for\na few seconds.\n\nClick OK when the console says:\n'Cache updated successfully'.")
    dpg.add_spacer(height=5)
    dpg.add_button(label="OK, Refresh Plugin Menu", width=-1, callback=lambda: [dpg.configure_item("scan_modal", show=False), populate_plugin_menu()])

with dpg.handler_registry(): 
    dpg.add_key_press_handler(dpg.mvKey_Delete, callback=global_remove_callback)
    dpg.add_mouse_release_handler(callback=check_node_movement)

dpg.create_viewport(title='Custom DSP Controller', width=1280, height=800)
dpg.setup_dearpygui()
dpg.show_viewport()
dpg.set_primary_window("MainWindow", True)

populate_plugin_menu()

existing_lists = refresh_setlist_dropdown()
if existing_lists:
    start_list = existing_lists[0]
    dpg.set_value("bank_list_combo", start_list)
    load_unified_setlist(None, start_list)

dpg.start_dearpygui()
dpg.destroy_context()
