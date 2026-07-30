//! lexbor's source list, enumerated rather than globbed.
//!
//! Generated once from the v3.0.0 tarball and committed, because 216 entries
//! would drown build.zig and because PLAN.md's rule is that a build states its
//! inputs. Regenerate on a version bump:
//!
//!     find <lexbor>/source -name '*.c' | sort
//!
//! Every module is compiled, not just the ones bboxes_html.cpp names. The HTML
//! and DOM entry points pull in core, tag, ns, encoding, unicode, punycode,
//! css and selectors transitively, and working out that closure by hand is a
//! way to get a link error later rather than a smaller binary now — the linker
//! drops what is unreachable anyway.


/// lexbor/core — 20 files
pub const core: []const []const u8 = &.{
    "source/lexbor/core/array.c",
    "source/lexbor/core/array_obj.c",
    "source/lexbor/core/avl.c",
    "source/lexbor/core/bst.c",
    "source/lexbor/core/bst_map.c",
    "source/lexbor/core/conv.c",
    "source/lexbor/core/diyfp.c",
    "source/lexbor/core/dobject.c",
    "source/lexbor/core/dtoa.c",
    "source/lexbor/core/hash.c",
    "source/lexbor/core/in.c",
    "source/lexbor/core/mem.c",
    "source/lexbor/core/mraw.c",
    "source/lexbor/core/plog.c",
    "source/lexbor/core/print.c",
    "source/lexbor/core/serialize.c",
    "source/lexbor/core/shs.c",
    "source/lexbor/core/str.c",
    "source/lexbor/core/strtod.c",
    "source/lexbor/core/utils.c",
};

/// lexbor/css — 26 files
pub const css: []const []const u8 = &.{
    "source/lexbor/css/at_rule.c",
    "source/lexbor/css/at_rule/state.c",
    "source/lexbor/css/blank.c",
    "source/lexbor/css/css.c",
    "source/lexbor/css/declaration.c",
    "source/lexbor/css/log.c",
    "source/lexbor/css/parser.c",
    "source/lexbor/css/property.c",
    "source/lexbor/css/property/state.c",
    "source/lexbor/css/rule.c",
    "source/lexbor/css/selectors/pseudo.c",
    "source/lexbor/css/selectors/pseudo_state.c",
    "source/lexbor/css/selectors/selector.c",
    "source/lexbor/css/selectors/selectors.c",
    "source/lexbor/css/selectors/state.c",
    "source/lexbor/css/state.c",
    "source/lexbor/css/stylesheet.c",
    "source/lexbor/css/syntax/anb.c",
    "source/lexbor/css/syntax/parser.c",
    "source/lexbor/css/syntax/state.c",
    "source/lexbor/css/syntax/syntax.c",
    "source/lexbor/css/syntax/token.c",
    "source/lexbor/css/syntax/tokenizer.c",
    "source/lexbor/css/syntax/tokenizer/error.c",
    "source/lexbor/css/unit.c",
    "source/lexbor/css/value.c",
};

/// lexbor/dom — 16 files
pub const dom: []const []const u8 = &.{
    "source/lexbor/dom/collection.c",
    "source/lexbor/dom/exception.c",
    "source/lexbor/dom/interface.c",
    "source/lexbor/dom/interfaces/attr.c",
    "source/lexbor/dom/interfaces/cdata_section.c",
    "source/lexbor/dom/interfaces/character_data.c",
    "source/lexbor/dom/interfaces/comment.c",
    "source/lexbor/dom/interfaces/document.c",
    "source/lexbor/dom/interfaces/document_fragment.c",
    "source/lexbor/dom/interfaces/document_type.c",
    "source/lexbor/dom/interfaces/element.c",
    "source/lexbor/dom/interfaces/event_target.c",
    "source/lexbor/dom/interfaces/node.c",
    "source/lexbor/dom/interfaces/processing_instruction.c",
    "source/lexbor/dom/interfaces/shadow_root.c",
    "source/lexbor/dom/interfaces/text.c",
};

/// lexbor/encoding — 7 files
pub const encoding: []const []const u8 = &.{
    "source/lexbor/encoding/decode.c",
    "source/lexbor/encoding/encode.c",
    "source/lexbor/encoding/encoding.c",
    "source/lexbor/encoding/multi_res.c",
    "source/lexbor/encoding/range_res.c",
    "source/lexbor/encoding/res.c",
    "source/lexbor/encoding/single_res.c",
};

/// lexbor/engine — 1 files
pub const engine: []const []const u8 = &.{
    "source/lexbor/engine/engine.c",
};

/// lexbor/html — 122 files
pub const html: []const []const u8 = &.{
    "source/lexbor/html/attribute_steps.c",
    "source/lexbor/html/common.c",
    "source/lexbor/html/element_steps.c",
    "source/lexbor/html/encoding.c",
    "source/lexbor/html/interface.c",
    "source/lexbor/html/interfaces/anchor_element.c",
    "source/lexbor/html/interfaces/area_element.c",
    "source/lexbor/html/interfaces/audio_element.c",
    "source/lexbor/html/interfaces/base_element.c",
    "source/lexbor/html/interfaces/body_element.c",
    "source/lexbor/html/interfaces/br_element.c",
    "source/lexbor/html/interfaces/button_element.c",
    "source/lexbor/html/interfaces/canvas_element.c",
    "source/lexbor/html/interfaces/d_list_element.c",
    "source/lexbor/html/interfaces/data_element.c",
    "source/lexbor/html/interfaces/data_list_element.c",
    "source/lexbor/html/interfaces/details_element.c",
    "source/lexbor/html/interfaces/dialog_element.c",
    "source/lexbor/html/interfaces/directory_element.c",
    "source/lexbor/html/interfaces/div_element.c",
    "source/lexbor/html/interfaces/document.c",
    "source/lexbor/html/interfaces/element.c",
    "source/lexbor/html/interfaces/embed_element.c",
    "source/lexbor/html/interfaces/field_set_element.c",
    "source/lexbor/html/interfaces/font_element.c",
    "source/lexbor/html/interfaces/form_element.c",
    "source/lexbor/html/interfaces/frame_element.c",
    "source/lexbor/html/interfaces/frame_set_element.c",
    "source/lexbor/html/interfaces/head_element.c",
    "source/lexbor/html/interfaces/heading_element.c",
    "source/lexbor/html/interfaces/hr_element.c",
    "source/lexbor/html/interfaces/html_element.c",
    "source/lexbor/html/interfaces/iframe_element.c",
    "source/lexbor/html/interfaces/image_element.c",
    "source/lexbor/html/interfaces/input_element.c",
    "source/lexbor/html/interfaces/label_element.c",
    "source/lexbor/html/interfaces/legend_element.c",
    "source/lexbor/html/interfaces/li_element.c",
    "source/lexbor/html/interfaces/link_element.c",
    "source/lexbor/html/interfaces/map_element.c",
    "source/lexbor/html/interfaces/marquee_element.c",
    "source/lexbor/html/interfaces/media_element.c",
    "source/lexbor/html/interfaces/menu_element.c",
    "source/lexbor/html/interfaces/meta_element.c",
    "source/lexbor/html/interfaces/meter_element.c",
    "source/lexbor/html/interfaces/mod_element.c",
    "source/lexbor/html/interfaces/o_list_element.c",
    "source/lexbor/html/interfaces/object_element.c",
    "source/lexbor/html/interfaces/opt_group_element.c",
    "source/lexbor/html/interfaces/option_element.c",
    "source/lexbor/html/interfaces/output_element.c",
    "source/lexbor/html/interfaces/paragraph_element.c",
    "source/lexbor/html/interfaces/param_element.c",
    "source/lexbor/html/interfaces/picture_element.c",
    "source/lexbor/html/interfaces/pre_element.c",
    "source/lexbor/html/interfaces/progress_element.c",
    "source/lexbor/html/interfaces/quote_element.c",
    "source/lexbor/html/interfaces/script_element.c",
    "source/lexbor/html/interfaces/search_element.c",
    "source/lexbor/html/interfaces/select_element.c",
    "source/lexbor/html/interfaces/selectedcontent_element.c",
    "source/lexbor/html/interfaces/slot_element.c",
    "source/lexbor/html/interfaces/source_element.c",
    "source/lexbor/html/interfaces/span_element.c",
    "source/lexbor/html/interfaces/style_element.c",
    "source/lexbor/html/interfaces/table_caption_element.c",
    "source/lexbor/html/interfaces/table_cell_element.c",
    "source/lexbor/html/interfaces/table_col_element.c",
    "source/lexbor/html/interfaces/table_element.c",
    "source/lexbor/html/interfaces/table_row_element.c",
    "source/lexbor/html/interfaces/table_section_element.c",
    "source/lexbor/html/interfaces/template_element.c",
    "source/lexbor/html/interfaces/text_area_element.c",
    "source/lexbor/html/interfaces/time_element.c",
    "source/lexbor/html/interfaces/title_element.c",
    "source/lexbor/html/interfaces/track_element.c",
    "source/lexbor/html/interfaces/u_list_element.c",
    "source/lexbor/html/interfaces/unknown_element.c",
    "source/lexbor/html/interfaces/video_element.c",
    "source/lexbor/html/interfaces/window.c",
    "source/lexbor/html/node.c",
    "source/lexbor/html/parser.c",
    "source/lexbor/html/serialize.c",
    "source/lexbor/html/serialize_ext.c",
    "source/lexbor/html/tag.c",
    "source/lexbor/html/token.c",
    "source/lexbor/html/token_attr.c",
    "source/lexbor/html/tokenizer.c",
    "source/lexbor/html/tokenizer/error.c",
    "source/lexbor/html/tokenizer/state.c",
    "source/lexbor/html/tokenizer/state_comment.c",
    "source/lexbor/html/tokenizer/state_doctype.c",
    "source/lexbor/html/tokenizer/state_rawtext.c",
    "source/lexbor/html/tokenizer/state_rcdata.c",
    "source/lexbor/html/tokenizer/state_script.c",
    "source/lexbor/html/tree.c",
    "source/lexbor/html/tree/active_formatting.c",
    "source/lexbor/html/tree/error.c",
    "source/lexbor/html/tree/insertion_mode/after_after_body.c",
    "source/lexbor/html/tree/insertion_mode/after_after_frameset.c",
    "source/lexbor/html/tree/insertion_mode/after_body.c",
    "source/lexbor/html/tree/insertion_mode/after_frameset.c",
    "source/lexbor/html/tree/insertion_mode/after_head.c",
    "source/lexbor/html/tree/insertion_mode/before_head.c",
    "source/lexbor/html/tree/insertion_mode/before_html.c",
    "source/lexbor/html/tree/insertion_mode/foreign_content.c",
    "source/lexbor/html/tree/insertion_mode/in_body.c",
    "source/lexbor/html/tree/insertion_mode/in_caption.c",
    "source/lexbor/html/tree/insertion_mode/in_cell.c",
    "source/lexbor/html/tree/insertion_mode/in_column_group.c",
    "source/lexbor/html/tree/insertion_mode/in_frameset.c",
    "source/lexbor/html/tree/insertion_mode/in_head.c",
    "source/lexbor/html/tree/insertion_mode/in_head_noscript.c",
    "source/lexbor/html/tree/insertion_mode/in_row.c",
    "source/lexbor/html/tree/insertion_mode/in_table.c",
    "source/lexbor/html/tree/insertion_mode/in_table_body.c",
    "source/lexbor/html/tree/insertion_mode/in_table_text.c",
    "source/lexbor/html/tree/insertion_mode/in_template.c",
    "source/lexbor/html/tree/insertion_mode/initial.c",
    "source/lexbor/html/tree/insertion_mode/text.c",
    "source/lexbor/html/tree/open_elements.c",
    "source/lexbor/html/tree/template_insertion.c",
};

/// lexbor/ns — 1 files
pub const ns: []const []const u8 = &.{
    "source/lexbor/ns/ns.c",
};

/// lexbor/ports — per-platform, NOT all of them.
///
/// posix/ and windows_nt/ define the same three functions (fs, memory, perf).
/// Compiling both is a duplicate-symbol error at best; here it failed earlier,
/// on windows.h being absent. Pick one by target.
pub const ports_posix: []const []const u8 = &.{
    "source/lexbor/ports/posix/lexbor/core/fs.c",
    "source/lexbor/ports/posix/lexbor/core/memory.c",
    "source/lexbor/ports/posix/lexbor/core/perf.c",
};

pub const ports_windows: []const []const u8 = &.{
    "source/lexbor/ports/windows_nt/lexbor/core/fs.c",
    "source/lexbor/ports/windows_nt/lexbor/core/memory.c",
    "source/lexbor/ports/windows_nt/lexbor/core/perf.c",
};

/// lexbor/punycode — 1 files
pub const punycode: []const []const u8 = &.{
    "source/lexbor/punycode/punycode.c",
};

/// lexbor/selectors — 1 files
pub const selectors: []const []const u8 = &.{
    "source/lexbor/selectors/selectors.c",
};

/// lexbor/style — 9 files
pub const style: []const []const u8 = &.{
    "source/lexbor/style/attribute_steps.c",
    "source/lexbor/style/dom/interfaces/document.c",
    "source/lexbor/style/dom/interfaces/element.c",
    "source/lexbor/style/element_steps.c",
    "source/lexbor/style/html/interfaces/document.c",
    "source/lexbor/style/html/interfaces/element.c",
    "source/lexbor/style/html/interfaces/option_element.c",
    "source/lexbor/style/html/interfaces/style_element.c",
    "source/lexbor/style/style.c",
};

/// lexbor/tag — 1 files
pub const tag: []const []const u8 = &.{
    "source/lexbor/tag/tag.c",
};

/// lexbor/unicode — 2 files
pub const unicode: []const []const u8 = &.{
    "source/lexbor/unicode/idna.c",
    "source/lexbor/unicode/unicode.c",
};

/// lexbor/url — 1 files
pub const url: []const []const u8 = &.{
    "source/lexbor/url/url.c",
};

/// lexbor/utils — 2 files
pub const utils: []const []const u8 = &.{
    "source/lexbor/utils/http.c",
    "source/lexbor/utils/warc.c",
};

/// Every platform-independent module. Add the right `ports_*` alongside.
pub const common: []const []const u8 = core ++ css ++ dom ++ encoding ++ engine ++ html ++ ns ++ punycode ++ selectors ++ style ++ tag ++ unicode ++ url ++ utils;
