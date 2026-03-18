/**
 * TreeWalker + Range.getClientRects() text bbox extraction.
 *
 * Visits every visible text node in the DOM and returns a bounding box
 * record for each. Uses Range.getClientRects() which does not trigger
 * layout reflow.
 */

/**
 * Extract all visible text bounding boxes from the current document.
 *
 * @param {Object} opts
 * @param {number} opts.maxTextLength - skip text nodes longer than this
 * @param {string} opts.mutationType - "snapshot" | "added" | "changed"
 * @param {Element} [opts.root] - root element (default: document.body)
 * @returns {Array<Object>} array of bbox records
 */
export function extractBBoxes(opts) {
    const maxLen = opts.maxTextLength || 200;
    const mutationType = opts.mutationType || "snapshot";
    const root = opts.root || document.body;
    const results = [];

    const walker = document.createTreeWalker(
        root,
        NodeFilter.SHOW_TEXT,
        {
            acceptNode: (node) => {
                const text = node.textContent.trim();
                if (!text || text.length > maxLen) return NodeFilter.FILTER_REJECT;
                const parent = node.parentElement;
                if (!parent) return NodeFilter.FILTER_REJECT;
                const style = window.getComputedStyle(parent);
                if (style.display === 'none' || style.visibility === 'hidden'
                    || style.opacity === '0') return NodeFilter.FILTER_REJECT;
                return NodeFilter.FILTER_ACCEPT;
            }
        }
    );

    while (walker.nextNode()) {
        const textNode = walker.currentNode;
        const text = textNode.textContent.trim();
        if (!text) continue;

        const range = document.createRange();
        range.selectNodeContents(textNode);
        const rects = range.getClientRects();
        if (rects.length === 0) continue;

        const rect = rects[0];
        if (rect.width === 0 || rect.height === 0) continue;

        const parent = textNode.parentElement;
        const style = window.getComputedStyle(parent);

        results.push({
            text: text,
            x: Math.round(rect.x * 10) / 10,
            y: Math.round(rect.y * 10) / 10,
            w: Math.round(rect.width * 10) / 10,
            h: Math.round(rect.height * 10) / 10,
            font_family: style.fontFamily.split(',')[0].trim().replace(/['"]/g, ''),
            font_size: parseFloat(style.fontSize),
            font_weight: style.fontWeight,
            color: style.color,
            tag: parent.tagName.toLowerCase(),
            cls: (parent.className || '').toString().substring(0, 80),
            t_ms: Math.round(performance.now() * 100) / 100,
            mutation_type: mutationType,
        });
    }

    return results;
}
