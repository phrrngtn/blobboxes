/**
 * CSS overlay highlighting for classified bboxes.
 *
 * Creates semi-transparent overlay divs positioned over matched bboxes.
 * Used in interactive mode (PySide6 CTP, Chrome extension) — not in
 * headless Playwright extraction.
 */

const HIGHLIGHT_CONTAINER_ID = '__blobboxes_highlights__';

/**
 * Remove all existing highlights.
 */
export function clearHighlights() {
    const existing = document.getElementById(HIGHLIGHT_CONTAINER_ID);
    if (existing) existing.remove();
}

/**
 * Highlight classified bboxes with colored overlays.
 *
 * @param {Array<{bbox: Object, matches: Array}>} classified
 * @param {Object} [opts]
 * @param {string} [opts.color] - overlay color (default: rgba(255,200,0,0.3))
 */
export function highlight(classified, opts) {
    clearHighlights();

    const color = (opts && opts.color) || 'rgba(255,200,0,0.3)';

    const container = document.createElement('div');
    container.id = HIGHLIGHT_CONTAINER_ID;
    container.style.cssText = 'position:fixed;top:0;left:0;width:0;height:0;z-index:2147483647;pointer-events:none;';

    for (const item of classified) {
        const b = item.bbox;
        const div = document.createElement('div');
        div.style.cssText = [
            'position:fixed',
            'top:' + b.y + 'px',
            'left:' + b.x + 'px',
            'width:' + b.w + 'px',
            'height:' + b.h + 'px',
            'background:' + color,
            'pointer-events:none',
        ].join(';') + ';';
        div.title = item.matches.map(m => m.domain + ' (' + m.score.toFixed(2) + ')').join(', ');
        container.appendChild(div);
    }

    document.body.appendChild(container);
}
