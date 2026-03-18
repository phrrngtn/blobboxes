/**
 * Domain classification via FNV-1a hashing + roaring bitmap probing.
 *
 * Tokenizes bbox text, hashes tokens with FNV-1a (matching the blobfilters
 * C implementation), builds a roaring bitmap, and probes against domain
 * filter bitmaps to find matches.
 */

/**
 * FNV-1a 32-bit hash — must match blobfilters C implementation.
 *
 * @param {string} str
 * @returns {number} unsigned 32-bit hash
 */
export function fnv1a(str) {
    let hash = 0x811c9dc5; // FNV offset basis
    for (let i = 0; i < str.length; i++) {
        hash ^= str.charCodeAt(i);
        hash = Math.imul(hash, 0x01000193); // FNV prime
    }
    return hash >>> 0; // unsigned 32-bit
}

/**
 * Classify a single bbox against loaded domain filters.
 *
 * @param {Object} bbox - bbox record with .text
 * @param {Array<{domain: string, bitmap: RoaringBitmap32}>} filters
 * @param {Function} RoaringBitmap32 - constructor from roaring-wasm
 * @returns {Array<{domain: string, score: number}>} matching domains
 */
export function classifyBBox(bbox, filters, RoaringBitmap32) {
    const tokens = bbox.text.toLowerCase().split(/\s+/).filter(t => t.length > 0);
    if (tokens.length === 0) return [];

    const textBitmap = new RoaringBitmap32();
    for (const token of tokens) {
        textBitmap.add(fnv1a(token));
    }

    const matches = [];
    for (const f of filters) {
        const overlap = f.bitmap.andCardinality(textBitmap);
        if (overlap > 0) {
            matches.push({
                domain: f.domain,
                score: overlap / textBitmap.size,
            });
        }
    }

    textBitmap.dispose();
    return matches;
}

/**
 * Classify an array of bboxes, returning only those with matches.
 *
 * @param {Array<Object>} bboxes
 * @param {Array<{domain: string, bitmap: RoaringBitmap32}>} filters
 * @param {Function} RoaringBitmap32
 * @returns {Array<{bbox: Object, matches: Array}>}
 */
export function classifyAll(bboxes, filters, RoaringBitmap32) {
    const results = [];
    for (const bbox of bboxes) {
        const matches = classifyBBox(bbox, filters, RoaringBitmap32);
        if (matches.length > 0) {
            results.push({ bbox, matches });
        }
    }
    return results;
}
